"""Convert the MediaPipe .tflite models to ONNX, for the ONNX Runtime backend.

The .onnx files are committed, so this only needs re-running if a model changes. The depth
model is translated from its OpenVINO IR by tools/ir_to_onnx.py instead.
Needs a separate environment (TensorFlow pins an older numpy):

    python3.11 -m venv .venv-convert
    .venv-convert/bin/pip install "tensorflow-cpu==2.17.*" "tf2onnx==1.16.1" "numpy<2" onnx
    .venv-convert/bin/python tools/convert_models.py           # from perception/

pose_detection.tflite stores most weights as sparse fp16 tensors expanded by a DENSIFY op,
which tf2onnx can't read; they are densified here before conversion. Outputs were checked
against OpenVINO running the .tflite: same order, max difference ~1e-3 on logits ~800.
"""
import argparse, os
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
MODELS = os.path.join(HERE, "..", "models")


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
    args = ap.parse_args()
    for name in ("pose_detection", "pose_landmark_full", "hand_landmark_full"):
        if args.only in (None, name):
            print(f"[convert] {name}")
            convert_tflite(os.path.join(MODELS, f"{name}.tflite"), os.path.join(MODELS, f"{name}.onnx"), args.opset)


if __name__ == "__main__":
    main()
