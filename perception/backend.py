"""Inference backends: ONNX Runtime (Core ML on Macs, TensorRT / CUDA / DirectML on Windows,
CPU anywhere) or OpenVINO (CPU). Every model is loaded through `Backend.load` and called as
`model(x) -> [outputs...]`, in the same output order on both backends.

Devices, per model group ("pose" = detector + landmarks + hands, "depth"):
  ane  Apple Neural Engine via Core ML (falls back to the GPU elsewhere)
  gpu  Apple GPU via Core ML, or NVIDIA via TensorRT / CUDA, or any GPU via DirectML
  cpu  ONNX Runtime CPU, or OpenVINO with the backend set to "openvino"
  auto ane on Macs, gpu where an NVIDIA / DirectML GPU is available, else cpu
"""
import os, platform
import numpy as np

MODEL_DIR = "models"
CACHE_DIR = os.path.join(MODEL_DIR, "cache")
FILES = {  # name: (onnx, openvino)
    "pose_detection": ("pose_detection.onnx", "pose_detection.tflite"),
    "pose_landmark_full": ("pose_landmark_full.onnx", "pose_landmark_full.tflite"),
    "hand_landmark_full": ("hand_landmark_full.onnx", "hand_landmark_full.tflite"),
    "depth": ("da2_metric_indoor_small.onnx", "da2_metric_indoor_small.xml"),
}
GPU_EPS = ("TensorrtExecutionProvider", "CUDAExecutionProvider", "DmlExecutionProvider")
# The Neural Engine only runs fp16 ops: given an fp32 model, Core ML quietly uses its CPU path.
# These float16 copies (float32 in/out) come from tools/convert_models.py.
ANE_FILES = {name: f"{name}_fp16.onnx" for name in ("pose_detection", "pose_landmark_full", "hand_landmark_full")}


def resolve_device(device, available):
    if device != "auto":
        return device
    if "CoreMLExecutionProvider" in available:
        return "ane"
    return "gpu" if any(e in available for e in GPU_EPS) else "cpu"


def onnx_providers(device, available):
    """ONNX Runtime execution providers (with options) for a device, best first, CPU last."""
    cpu = ("CPUExecutionProvider", {})
    coreml = "CoreMLExecutionProvider" in available
    eps = []
    if device in ("ane", "gpu") and coreml:
        eps.append(("CoreMLExecutionProvider", {
            "ModelFormat": "MLProgram",
            "MLComputeUnits": "CPUAndNeuralEngine" if device == "ane" else "CPUAndGPU",
            "RequireStaticInputShapes": "1",
            "ModelCacheDirectory": os.path.abspath(os.path.join(CACHE_DIR, "coreml")),
        }))
    elif device in ("ane", "gpu"):
        if "TensorrtExecutionProvider" in available:
            eps.append(("TensorrtExecutionProvider", {
                "trt_fp16_enable": True, "trt_engine_cache_enable": True,
                "trt_engine_cache_path": os.path.abspath(os.path.join(CACHE_DIR, "tensorrt")),
            }))
        if "CUDAExecutionProvider" in available:
            eps.append(("CUDAExecutionProvider", {"device_id": 0, "cudnn_conv_algo_search": "HEURISTIC"}))
        elif "DmlExecutionProvider" in available:
            eps.append(("DmlExecutionProvider", {"device_id": 0}))
    return eps + [cpu]


class OnnxModel:
    def __init__(self, ort, path, providers, threads):
        so = ort.SessionOptions()
        so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        if threads:
            so.intra_op_num_threads = int(threads)
        names = [p for p, _ in providers]
        try:
            self.sess = ort.InferenceSession(path, so, providers=providers)
        except Exception as e:                         # e.g. TensorRT listed but its libraries missing
            print(f"[backend] {os.path.basename(path)}: {names[0]} failed ({str(e).splitlines()[0]}); trying without it")
            self.sess = ort.InferenceSession(path, so, providers=providers[1:])
        self.inp = self.sess.get_inputs()[0].name
        self.provider = self.sess.get_providers()[0]

    def __call__(self, x):
        return self.sess.run(None, {self.inp: x})


class OpenVinoModel:
    def __init__(self, core, path, config):
        self.model = core.compile_model(path, "CPU", config)
        self.req = self.model.create_infer_request()
        self.provider = "OpenVINO CPU"

    def __call__(self, x):
        res = self.req.infer({0: x})
        return [res[o] for o in self.model.outputs]


class Backend:
    def __init__(self, cfg):
        """cfg: the "compute" block of calib/config.json."""
        self.cfg = cfg
        self.kind = cfg.get("backend", "auto")
        self.ort = self.core = None
        if self.kind in ("auto", "onnx"):
            try:
                import onnxruntime as ort
                if hasattr(ort, "preload_dlls") and "CUDAExecutionProvider" in ort.get_available_providers():
                    try:                               # CUDA / cuDNN from the nvidia-* pip wheels, if installed
                        ort.preload_dlls()
                    except Exception:
                        pass
                if cfg.get("verbose"):
                    ort.set_default_logger_severity(1)
                self.ort, self.kind = ort, "onnx"
            except ImportError:
                if self.kind == "onnx":
                    raise
                self.kind = "openvino"
        ncpu = os.cpu_count() or 4
        self.threads = {"depth": cfg.get("depth_threads") or max(1, ncpu // 2)}
        self.threads["pose"] = cfg.get("pose_threads") or max(1, ncpu - self.threads["depth"])
        self.devices = {g: resolve_device(cfg.get(f"{g}_device", "auto"), self.available) for g in ("pose", "depth")}
        if self.kind == "openvino" or "cpu" in self.devices.values():
            try:                                       # CPU inference: OpenVINO where installed
                import openvino as ov
                self.core = ov.Core()
                self.core.set_property("CPU", {"CACHE_DIR": CACHE_DIR})
            except ImportError:
                if self.kind == "openvino":
                    raise
        self.loaded = {}

    @property
    def available(self):
        return self.ort.get_available_providers() if self.ort else []

    def _ov_config(self, threads):
        # With shared defaults, depth inference makes pose inference ~4x slower on the CPU
        # (8 -> 35 ms on a 4-core test box); separate budgets with pinning off keep it ~10-14 ms.
        supported = self.core.get_property("CPU", "SUPPORTED_PROPERTIES")
        cfg = {"PERFORMANCE_HINT": "LATENCY", "INFERENCE_NUM_THREADS": int(threads), "ENABLE_CPU_PINNING": False}
        return {k: v for k, v in cfg.items() if k in supported}

    def load(self, name, group):
        onnx_file, ov_file = FILES[name]
        device, threads = self.devices[group], self.threads[group]
        use_ov = self.kind == "openvino" or (device == "cpu" and self.core is not None)
        if not use_ov and not os.path.exists(os.path.join(MODEL_DIR, onnx_file)):
            raise FileNotFoundError(f"{onnx_file} missing; see tools/convert_models.py / tools/ir_to_onnx.py")
        if device == "ane" and "CoreMLExecutionProvider" in self.available and name in ANE_FILES \
                and os.path.exists(os.path.join(MODEL_DIR, ANE_FILES[name])):
            onnx_file = ANE_FILES[name]
        if use_ov:
            m = OpenVinoModel(self.core, os.path.join(MODEL_DIR, ov_file), self._ov_config(threads))
        else:
            m = OnnxModel(self.ort, os.path.join(MODEL_DIR, onnx_file),
                          onnx_providers(device, self.available), threads)
        self.loaded[name] = m.provider + (" fp16" if onnx_file.endswith("_fp16.onnx") else "")
        return m

    def summary(self):
        host = f"{platform.system()} {platform.machine()}"
        eps = ", ".join(e.replace("ExecutionProvider", "") for e in self.available) or "-"
        models = ", ".join(f"{k}={v.replace('ExecutionProvider', '')}" for k, v in self.loaded.items())
        return f"[backend] {host}, {self.kind} (available: {eps}): {models}"


def warmup(model, shape, dtype=np.float32):
    """First calls compile kernels / Core ML programs / TensorRT engines: do it before tracking."""
    x = np.zeros(shape, dtype)
    for _ in range(2):
        model(x)
