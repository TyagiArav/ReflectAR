"""Convert the MediaPipe .tflite models to ONNX, for the ONNX Runtime backend.

The .onnx files are committed, so this only needs re-running if a model changes. The depth
model is translated from its OpenVINO IR by tools/ir_to_onnx.py instead.
Needs a separate environment (TensorFlow pins an older numpy):

    python3.11 -m venv .venv-convert
    .venv-convert/bin/pip install "tensorflow-cpu==2.17.*" "tf2onnx==1.16.1" "numpy<2" onnx onnxconverter-common
    .venv-convert/bin/python tools/convert_models.py           # from perception/

Each model is written twice, both post-processed for Core ML (see `for_coreml`):
  <name>.onnx       float32: CPU, Core ML GPU, CUDA / TensorRT / DirectML
  <name>_fp16.onnx  float16 inside, float32 in/out: the Neural Engine runs only fp16 ops, so
                    Core ML puts an fp32 model on its CPU path even with MLComputeUnits=CPUAndNeuralEngine.
                    Landmarks move by ~0.7-0.9 px on 1280x720 frames vs fp32 (visible joints).
`--from-onnx` redoes only that post-processing on the existing .onnx (needs just onnx +
onnxconverter-common, no TensorFlow).

pose_detection.tflite stores most weights as sparse fp16 tensors expanded by a DENSIFY op,
which tf2onnx can't read; they are densified here before conversion. Outputs were checked
against OpenVINO running the .tflite: same order, max difference ~1e-3 on logits ~800.
"""
import argparse, os
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
MODELS = os.path.join(HERE, "..", "models")


# ------------------------------------------------------------------ Core ML fixes
def explicit_pads(model):
    """tf2onnx leaves `pads` off Conv / MaxPool that follow an explicit Pad (TF "VALID").
    ONNX Runtime's Core ML EP (checked up to 1.31) then emits no `pad` param and the ML Program
    fails to compile ("Required param 'pad' is missing"), so the model falls back to the CPU."""
    from onnx import helper
    for n in model.graph.node:
        names = {a.name for a in n.attribute}
        if n.op_type in ("Conv", "MaxPool", "AveragePool") and not names & {"pads", "auto_pad"}:
            rank = len(next(a.ints for a in n.attribute if a.name == "kernel_shape"))
            n.attribute.append(helper.make_attribute("pads", [0] * 2 * rank))
    return model


def to_fp16(model):
    """Float16 weights and activations (Neural Engine), float32 inputs / outputs."""
    import onnx
    from onnx import numpy_helper
    from onnxconverter_common import float16
    # Resize is blocked by default; an fp32 island splits the graph and the ANE compiler then
    # rejects pose_landmark_full, so convert it too and keep only its roi / scales in fp32.
    block = [op for op in float16.DEFAULT_OP_BLOCK_LIST if op != "Resize"]
    model = float16.convert_float_to_float16(model, keep_io_types=True, op_block_list=block)
    inits = {t.name: t for t in model.graph.initializer}
    for n in model.graph.node:
        for k in (1, 2) if n.op_type == "Resize" else ():
            t = inits.get(n.input[k]) if k < len(n.input) else None
            if t is not None and t.data_type == onnx.TensorProto.FLOAT16:
                name = t.name + "_fp32"
                if name not in inits:
                    inits[name] = numpy_helper.from_array(numpy_helper.to_array(t).astype(np.float32), name)
                    model.graph.initializer.append(inits[name])
                n.input[k] = name
    used = {i for n in model.graph.node for i in n.input}
    keep = [t for t in model.graph.initializer if t.name in used]
    del model.graph.initializer[:]
    model.graph.initializer.extend(keep)
    return model


def save_for_coreml(model, path):
    """Saves with a content hash as COREML_CACHE_KEY: ONNX Runtime keys its Core ML cache by the
    model path otherwise, so a regenerated model would load the stale compiled program."""
    import hashlib, onnx
    del model.metadata_props[:]
    onnx.checker.check_model(model, full_check=True)
    key = hashlib.sha256(model.SerializeToString()).hexdigest()[:32]
    onnx.helper.set_model_props(model, {"COREML_CACHE_KEY": key})
    onnx.save(model, path)


def for_coreml(name):
    import onnx
    src = os.path.join(MODELS, f"{name}.onnx")
    model = explicit_pads(onnx.load(src))
    save_for_coreml(model, src)
    save_for_coreml(to_fp16(model), os.path.join(MODELS, f"{name}_fp16.onnx"))


# ------------------------------------------------------------------ tflite
def _index_vector(dm, which):
    """Values of a DimensionMetadata array_segments / array_indices union as a numpy array."""
    from tf2onnx.tflite import Int32Vector, Uint16Vector, Uint8Vector
    from tf2onnx.tflite_utils import lookup_enum
    kind = lookup_enum(getattr(dm, f"Array{which}Type")(), "SparseIndexVector")
    vec = {"Int32Vector": Int32Vector.Int32Vector, "Uint16Vector": Uint16Vector.Uint16Vector,
           "Uint8Vector": Uint8Vector.Uint8Vector}[kind]()
    tab = getattr(dm, f"Array{which}")()
    vec.Init(tab.Bytes, tab.Pos)
    return vec.ValuesAsNumpy()


def _densify(tensor, data):
    """Expand a TFLite sparse tensor whose leading dims are dense and whose last dim is CSR
    (the only layout MediaPipe's models use). Returns the dense raw bytes."""
    sp = tensor.Sparsity()
    shape = tensor.ShapeAsNumpy().tolist()
    order = sp.TraversalOrderAsNumpy().tolist()
    dims = [sp.DimMetadata(i) for i in range(sp.DimMetadataLength())]
    if order != list(range(len(shape))) or not sp.BlockMapIsNone() \
            or any(d.Format() != 0 for d in dims[:-1]) or dims[-1].Format() != 1:
        raise NotImplementedError(f"sparse layout of {tensor.Name()} not supported")
    from tf2onnx.tflite.TensorType import TensorType
    dtype = {TensorType.FLOAT16: np.float16, TensorType.FLOAT32: np.float32, TensorType.INT8: np.int8}[tensor.Type()]
    vals = np.frombuffer(data, dtype)
    seg, idx = _index_vector(dims[-1], "Segments"), _index_vector(dims[-1], "Indices")
    rows = int(np.prod(shape[:-1]))
    dense = np.zeros((rows, shape[-1]), dtype)
    for r in range(rows):
        a, b = int(seg[r]), int(seg[r + 1])
        dense[r, idx[a:b]] = vals[a:b]
    return dense.reshape(shape).tobytes()


def convert_tflite(src, dst, opset):
    import tf2onnx
    from tf2onnx import tflite_utils
    from tf2onnx.handler import tfl_op
    from tf2onnx.tflite.Model import Model

    @tfl_op("TFL_DENSIFY", tf_op="Identity")     # weights are dense already (see _densify)
    class _Densify:
        @classmethod
        def to_tf(cls, ctx, node, **kwargs):
            pass

    class _Buf:
        def __init__(self, data): self.data = np.frombuffer(data, np.uint8)
        def DataIsNone(self): return False
        def DataAsNumpy(self): return self.data
        def DataLength(self): return self.data.size

    class _ModelWithDenseBuffers:
        def __init__(self, model, dense): self._m, self._dense = model, dense
        def Buffers(self, i): return _Buf(self._dense[i]) if i in self._dense else self._m.Buffers(i)
        def __getattr__(self, name): return getattr(self._m, name)

    def read_tflite_model(path):
        # Same as tf2onnx's, minus the TFLite interpreter (it segfaults on sparse models;
        # it's only used to refine shapes, which the file has) plus densified buffers.
        with open(path, "rb") as f:
            model = Model.GetRootAsModel(f.read(), 0)
        opcodes = {}
        for i in range(model.OperatorCodesLength()):
            oc = model.OperatorCodes(i)
            code = tflite_utils.lookup_enum(oc.DeprecatedBuiltinCode(), "BuiltinOperator")
            if code == "PLACEHOLDER_FOR_GREATER_OP_CODES":
                code = tflite_utils.lookup_enum(oc.BuiltinCode(), "BuiltinOperator")
            if code == "CUSTOM":
                code = oc.CustomCode().decode()
            opcodes[i] = code
        dense = {}
        for g in range(model.SubgraphsLength()):
            sg = model.Subgraphs(g)
            for t in range(sg.TensorsLength()):
                tensor = sg.Tensors(t)
                if tensor.Sparsity() is not None:
                    dense[tensor.Buffer()] = _densify(tensor, model.Buffers(tensor.Buffer()).DataAsNumpy().tobytes())
        if dense:
            print(f"  densified {len(dense)} sparse weight tensors")
        return tflite_utils.get_model_subgraphs(model), opcodes, _ModelWithDenseBuffers(model, dense), {}

    tflite_utils.read_tflite_model = read_tflite_model
    tf2onnx.convert.from_tflite(src, opset=opset, output_path=dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--opset", type=int, default=17)
    ap.add_argument("--only", choices=["pose_detection", "pose_landmark_full", "hand_landmark_full"])
    ap.add_argument("--from-onnx", action="store_true", help="only redo the Core ML post-processing")
    args = ap.parse_args()
    for name in ("pose_detection", "pose_landmark_full", "hand_landmark_full"):
        if args.only in (None, name):
            print(f"[convert] {name}")
            if not args.from_onnx:
                convert_tflite(os.path.join(MODELS, f"{name}.tflite"), os.path.join(MODELS, f"{name}.onnx"), args.opset)
            for_coreml(name)


if __name__ == "__main__":
    main()
