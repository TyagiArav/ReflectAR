"""Translate the Depth Anything OpenVINO IR (models/da2_metric_indoor_small.xml) to ONNX.

Use this when the original PyTorch weights can't be fetched (convert_models.py's depth
export needs Hugging Face). The input is fixed to depth.py's size and OpenVINO's constant
folding removes the shape arithmetic, so only static compute ops remain; those are mapped
one-to-one. Weights are stored as fp16 (as in the IR) with a Cast to fp32.

    .venv/bin/python tools/ir_to_onnx.py      # from perception/; needs openvino + onnx
Then compares the result against OpenVINO on a test input (needs onnxruntime).
"""
import argparse, math
import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper
import openvino as ov
from openvino import passes

IN_H, IN_W = 266, 476                           # = depth.IN_H, IN_W


class Builder:
    def __init__(self):
        self.nodes, self.inits, self.names, self.n = [], [], {}, 0

    def tmp(self, base="t"):
        self.n += 1
        return f"{base}_{self.n}"

    def const(self, arr, name=None):
        arr = np.asarray(arr)
        name = name or self.tmp("c")
        if arr.dtype == np.float32 and arr.size > 64:         # weights: fp16 storage, cast on load
            self.inits.append(numpy_helper.from_array(arr.astype(np.float16), name + "_f16"))
            self.node("Cast", [name + "_f16"], [name], to=TensorProto.FLOAT)
        else:
            self.inits.append(numpy_helper.from_array(arr, name))
        return name

    def node(self, op, ins, outs=None, **attrs):
        outs = outs or [self.tmp(op.lower())]
        self.nodes.append(helper.make_node(op, ins, outs, **attrs))
        return outs[0]


def out_name(output):
    return f"{output.get_node().get_friendly_name()}:{output.get_index()}"


def translate(model):
    b = Builder()
    consts = {}
    inp = model.inputs[0]
    graph_in = helper.make_tensor_value_info("pixel_values", TensorProto.FLOAT, list(inp.get_shape()))
    for op in model.get_ordered_ops():
        t = op.get_type_name()
        a = op.get_attributes()
        ins = [out_name(i.get_source_output()) for i in op.inputs()]
        cin = [consts.get(n) for n in ins]                      # numpy value if the input is constant
        out = out_name(op.output(0)) if op.get_output_size() else None

        def C(i):                                               # constant input as numpy
            if cin[i] is None:
                raise NotImplementedError(f"{t}: input {i} is not constant")
            return cin[i]

        def X(i):                                               # input as an ONNX tensor name
            if cin[i] is not None and ins[i] not in b.names:
                b.names[ins[i]] = b.const(cin[i], b.tmp("w"))
            return b.names.get(ins[i], ins[i])

        if t == "Parameter":
            b.names[out] = "pixel_values"
        elif t == "Constant":
            consts[out] = op.get_data().copy()
        elif t == "Result":
            b.node("Identity", [X(0)], ["depth"])
        elif t in ("Add", "Multiply", "Divide"):
            b.node({"Add": "Add", "Multiply": "Mul", "Divide": "Div"}[t], [X(0), X(1)], [out])
        elif t in ("Relu", "Sigmoid"):
            b.node(t, [X(0)], [out])
        elif t == "MatMul":
            x = X(0)
            if a["transpose_a"]:
                x = b.node("Transpose", [x], perm=_swap_last(op.input(0).get_partial_shape().rank.get_length()))
            if a["transpose_b"] and cin[1] is not None:         # fold into the weight
                y = b.const(np.swapaxes(C(1), -1, -2).copy(), b.tmp("w"))
            elif a["transpose_b"]:
                y = b.node("Transpose", [X(1)], perm=_swap_last(op.input(1).get_partial_shape().rank.get_length()))
            else:
                y = X(1)
            b.node("MatMul", [x, y], [out])
        elif t == "Reshape":
            if not a["special_zero"] and 0 in C(1):
                raise NotImplementedError("Reshape with literal 0 dims")
            b.node("Reshape", [X(0), b.const(C(1).astype(np.int64))], [out])
        elif t == "Transpose":
            b.node("Transpose", [X(0)], [out], perm=[int(v) for v in C(1)])
        elif t == "Concat":
            b.node("Concat", [X(i) for i in range(len(ins))], [out], axis=int(a["axis"]))
        elif t == "Squeeze":
            b.node("Squeeze", [X(0), b.const(C(1).astype(np.int64).reshape(-1))], [out])
        elif t == "Slice":
            b.node("Slice", [X(0)] + [b.const(C(i).astype(np.int64)) for i in (1, 2, 4, 3)], [out])
        elif t == "Convolution":
            _check_explicit(a)
            b.node("Conv", [X(0), X(1)], [out], strides=a["strides"], dilations=a["dilations"],
                   pads=list(a["pads_begin"]) + list(a["pads_end"]))
        elif t == "ConvolutionBackpropData":
            _check_explicit(a)
            if len(ins) > 2:
                raise NotImplementedError("ConvolutionBackpropData with output_shape")
            b.node("ConvTranspose", [X(0), X(1)], [out], strides=a["strides"], dilations=a["dilations"],
                   pads=list(a["pads_begin"]) + list(a["pads_end"]), output_padding=a["output_padding"])
        elif t == "MVN":
            axes = [int(v) for v in C(1)]
            rank = op.input(0).get_partial_shape().rank.get_length()
            axes = sorted(ax % rank for ax in axes)
            if not a["normalize_variance"]:
                raise NotImplementedError("MVN without variance normalisation")
            x = X(0)
            mean = b.node("ReduceMean", [x], axes=axes, keepdims=1)          # opset 17: axes attribute
            d = b.node("Sub", [x, mean])
            var = b.node("ReduceMean", [b.node("Mul", [d, d])], axes=axes, keepdims=1)
            eps = b.const(np.array(a["eps"], np.float32))
            if a["eps_mode"].upper() == "INSIDE_SQRT":
                den = b.node("Sqrt", [b.node("Add", [var, eps])])
            else:
                den = b.node("Add", [b.node("Sqrt", [var]), eps])
            b.node("Div", [d, den], [out])
        elif t == "Gelu":
            x = X(0)
            if a["approximation_mode"].upper() == "ERF":
                e = b.node("Erf", [b.node("Div", [x, b.const(np.array(math.sqrt(2), np.float32))])])
            else:
                k = b.const(np.array(math.sqrt(2 / math.pi), np.float32))
                c3 = b.node("Mul", [b.const(np.array(0.044715, np.float32)), b.node("Pow", [x, b.const(np.array(3, np.float32))])])
                e = b.node("Tanh", [b.node("Mul", [k, b.node("Add", [x, c3])])])
            half = b.node("Mul", [x, b.const(np.array(0.5, np.float32))])
            b.node("Mul", [half, b.node("Add", [e, b.const(np.array(1, np.float32))])], [out])
        elif t == "ScaledDotProductAttention":
            if a["causal"]:
                raise NotImplementedError("causal attention")
            q, k, v = X(0), X(1), X(2)
            d = op.input(0).get_partial_shape()[-1].get_length()
            scale = float(C(4)) if len(ins) > 4 else 1.0 / math.sqrt(d)
            rank = op.input(1).get_partial_shape().rank.get_length()
            s = b.node("MatMul", [q, b.node("Transpose", [k], perm=_swap_last(rank))])
            s = b.node("Mul", [s, b.const(np.array(scale, np.float32))])
            if len(ins) > 3:
                mask = C(3)
                if mask.dtype == np.bool_:
                    raise NotImplementedError("boolean attention mask")
                if np.any(mask != 0):
                    s = b.node("Add", [s, X(3)])
            b.node("MatMul", [b.node("Softmax", [s], axis=-1), v], [out])
        elif t == "Interpolate":
            if any(a["pads_begin"]) or any(a["pads_end"]) or a["antialias"] or (
                    a["shape_calculation_mode"] == "scales" and a["coordinate_transformation_mode"] != "align_corners"):
                raise NotImplementedError(f"Interpolate {a}")   # (align_corners only depends on in/out size)
            sizes = list(op.output(0).get_shape())                 # static after folding
            mode = {"linear_onnx": "linear", "linear": "linear", "nearest": "nearest", "cubic": "cubic"}[a["mode"]]
            extra = {"cubic_coeff_a": float(a["cube_coeff"])} if mode == "cubic" else {}
            if mode == "nearest":
                extra["nearest_mode"] = {"floor": "floor", "ceil": "ceil", "round_prefer_floor": "round_prefer_floor",
                                         "round_prefer_ceil": "round_prefer_ceil", "simple": "floor"}[a["nearest_mode"]]
            b.node("Resize", [X(0), "", "", b.const(np.array(sizes, np.int64))], [out], mode=mode,
                   coordinate_transformation_mode=a["coordinate_transformation_mode"], **extra)
        else:
            raise NotImplementedError(f"op {t}")
        if out is not None and out not in b.names and t != "Constant":
            b.names[out] = out

    out = model.outputs[0]
    graph_out = helper.make_tensor_value_info("depth", TensorProto.FLOAT, list(out.get_shape()))
    g = helper.make_graph(b.nodes, "da2_metric_indoor_small", [graph_in], [graph_out], b.inits)
    m = helper.make_model(g, opset_imports=[helper.make_opsetid("", 17)], producer_name="ir_to_onnx")
    m.ir_version = 8
    onnx.checker.check_model(m)
    return m


def _swap_last(rank):
    p = list(range(rank))
    p[-1], p[-2] = p[-2], p[-1]
    return p


def _check_explicit(a):
    if a.get("auto_pad", "explicit") not in ("explicit", "notset"):
        raise NotImplementedError(f"auto_pad={a['auto_pad']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="models/da2_metric_indoor_small.xml")
    ap.add_argument("--dst", default="models/da2_metric_indoor_small.onnx")
    args = ap.parse_args()
    model = ov.Core().read_model(args.src)
    model.reshape([1, 3, IN_H, IN_W])
    pm = passes.Manager()
    pm.register_pass(passes.ConstantFolding())
    pm.run_passes(model)
    onnx.save(translate(model), args.dst)
    print(f"wrote {args.dst}")

    try:
        import onnxruntime as ort
    except ImportError:
        return
    rng = np.random.default_rng(0)
    x = rng.normal(0, 1, (1, 3, IN_H, IN_W)).astype(np.float32)
    ref = ov.Core().compile_model(args.src, "CPU", {"INFERENCE_PRECISION_HINT": "f32"}).create_infer_request().infer({0: x})
    ref = list(ref.values())[0]
    got = ort.InferenceSession(args.dst, providers=["CPUExecutionProvider"]).run(None, {"pixel_values": x})[0]
    print(f"check vs OpenVINO: max |diff| {np.abs(got - ref).max():.2e} m, mean {np.abs(got - ref).mean():.2e} m "
          f"(depth range {ref.min():.2f}-{ref.max():.2f} m)")


if __name__ == "__main__":
    main()
