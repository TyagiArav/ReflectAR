"""Builds ReflectAR's materials in /Game/ReflectAR (binary assets, so they are generated, not committed).

Runs on the first editor start (init_unreal.py), or by hand: Tools > Execute Python Script, or in
the Output Log's Python console:  import reflectar_setup; reflectar_setup.build()

Parameter names must match Source/ReflectAR (ReflectARMirror.cpp):
  MPC_ReflectAR    vectors MaskM0..2 (body -> camera image rows), W2B0..2 (world cm -> body m rows),
                   CamBody (viewpoint, body coords), CoverA0..11 / CoverB0..11 (garment outline
                   capsules: ends, A.w = radius), Flags (x feed brightness, y mask threshold,
                   z cover count, w proxy debug view), Ambient (rgb, luminance of white)
  M_Fabric(Fade)   BaseColorMap, NormalMap, MaskMap, Tile, Tint, Metallic, Sheen (+ Alpha)
  M_Trim(Fade)     Tint, Metallic, Roughness (+ Alpha)
  M_Proxy          MaskTex, FeedTex; per vertex UV1 = mask-motion xy, UV2 = (motion z, push m), UV3.x = trim
  M_Feed           FeedTex, Dim
"""
import unreal

DIR = "/Game/ReflectAR"
DEFAULT_TEX = "/Engine/EngineResources/DefaultTexture.DefaultTexture"

tools = unreal.AssetToolsHelpers.get_asset_tools()
mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary
P = unreal.MaterialProperty

VECTORS = (["MaskM0", "MaskM1", "MaskM2", "W2B0", "W2B1", "W2B2", "CamBody", "Flags", "Ambient"]
           + ["CoverA%d" % i for i in range(12)] + ["CoverB%d" % i for i in range(12)])


def recreate(name, cls, factory):
    path = "%s/%s" % (DIR, name)
    if eal.does_asset_exist(path):
        eal.delete_asset(path)
    return tools.create_asset(name, DIR, cls, factory)


def node(mat, cls, x, y, **props):
    e = mel.create_material_expression(mat, cls, x, y)
    for k, v in props.items():
        e.set_editor_property(k, v)
    return e


def link(src, out, dst, inp):
    if not mel.connect_material_expressions(src, out, dst, inp):
        unreal.log_warning("ReflectAR setup: could not connect %s.%s -> %s.%s" % (src.get_name(), out, dst.get_name(), inp))


def custom(mat, x, y, code, inputs, out_type):
    c = node(mat, unreal.MaterialExpressionCustom, x, y, code=code, output_type=out_type)
    ins = []
    for name in inputs:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", name)
        ins.append(ci)
    c.set_editor_property("inputs", ins)
    return c


def scalar(mat, name, value, x, y):
    return node(mat, unreal.MaterialExpressionScalarParameter, x, y, parameter_name=name, default_value=value)


def vector(mat, name, value, x, y):
    return node(mat, unreal.MaterialExpressionVectorParameter, x, y, parameter_name=name, default_value=value)


def texture(mat, name, uv, x, y):
    t = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, x, y, parameter_name=name,
             texture=unreal.load_asset(DEFAULT_TEX))
    if uv is not None:
        link(uv, "", t, "UVs")
    return t


def collection(mat, mpc, name, x, y):
    return node(mat, unreal.MaterialExpressionCollectionParameter, x, y, collection=mpc, parameter_name=name)


def mul(mat, a, b, x, y, a_out="", b_out=""):
    m = node(mat, unreal.MaterialExpressionMultiply, x, y)
    link(a, a_out, m, "A")
    link(b, b_out, m, "B")
    return m


def inverse_exposure(mat, x, y):
    """Output = 1 / camera exposure, so emissive images (the camera feed) keep their brightness."""
    cls = getattr(unreal, "MaterialExpressionEyeAdaptationInverse", None)
    if cls is not None:
        return node(mat, cls, x, y)                 # unconnected: light value 1, alpha 1
    unreal.log_warning("ReflectAR setup: no EyeAdaptationInverse node; using 1 / exposure for EV100 9.9")
    return node(mat, unreal.MaterialExpressionConstant, x, y, r=1.2 * 2 ** 9.907)


def finish(mat):
    mel.layout_material_expressions(mat)
    mel.recompile_material(mat)
    eal.save_loaded_asset(mat)


# ---------------------------------------------------------------------------- collection
def build_collection():
    mpc = recreate("MPC_ReflectAR", unreal.MaterialParameterCollection, unreal.MaterialParameterCollectionFactoryNew())
    params = []
    for n in VECTORS:
        p = unreal.CollectionVectorParameter()
        p.set_editor_property("parameter_name", n)
        p.set_editor_property("default_value", unreal.LinearColor(0, 0, 0, 0))
        params.append(p)
    mpc.set_editor_property("vector_parameters", params)
    eal.save_loaded_asset(mpc)
    return mpc


# ---------------------------------------------------------------------------- garments
NORMAL_CODE = """float3 n = N * 2 - 1;
// Cancel the two-sided back-face flip: the garment is lit by its outward normal on both sides, so
// shading doesn't depend on triangle winding (the CameraFeed view is mirrored).
return float3(n.xy, n.z * Sign);"""


def build_fabric(mpc, fade):
    name = "M_FabricFade" if fade else "M_Fabric"
    m = recreate(name, unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("two_sided", True)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_CLOTH)
    if fade:
        m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
        m.set_editor_property("translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    uv0 = node(m, unreal.MaterialExpressionTextureCoordinate, -1400, 0)
    uv = mul(m, uv0, scalar(m, "Tile", 4.0, -1400, 150), -1200, 0)
    base = texture(m, "BaseColorMap", uv, -1000, -300)
    nrm = texture(m, "NormalMap", uv, -1000, 0)
    msk = texture(m, "MaskMap", uv, -1000, 300)
    col = mul(m, base, vector(m, "Tint", unreal.LinearColor(1, 1, 1, 1), -1000, -500), -700, -300, a_out="RGB")
    mel.connect_material_property(col, "", P.MP_BASE_COLOR)
    n = custom(m, -700, 0, NORMAL_CODE, ["N", "Sign"], unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    link(nrm, "RGB", n, "N")
    link(node(m, unreal.MaterialExpressionTwoSidedSign, -1000, 150), "", n, "Sign")
    mel.connect_material_property(n, "", P.MP_NORMAL)
    rough = node(m, unreal.MaterialExpressionOneMinus, -700, 300)
    link(msk, "A", rough, "")
    mel.connect_material_property(rough, "", P.MP_ROUGHNESS)
    mel.connect_material_property(msk, "G", P.MP_AMBIENT_OCCLUSION)
    mel.connect_material_property(scalar(m, "Metallic", 0.0, -700, 450), "", P.MP_METALLIC)
    # cloth: fuzz colour = a lighter version of the base, amount = Sheen
    sheen = scalar(m, "Sheen", 0.25, -700, 600)
    lighter = node(m, unreal.MaterialExpressionLinearInterpolate, -450, 500, const_b=1.0, const_alpha=0.4)
    link(col, "", lighter, "A")
    mel.connect_material_property(mul(m, lighter, sheen, -250, 500), "", P.MP_SUBSURFACE_COLOR)
    mel.connect_material_property(sheen, "", P.MP_CUSTOM_DATA0)
    # the room's bounce light (there is no environment to light the cloth indirectly)
    amb = mul(m, mul(m, col, msk, -450, 200, b_out="G"), collection(m, mpc, "Ambient", -700, 750), -250, 200)
    mel.connect_material_property(amb, "", P.MP_EMISSIVE_COLOR)
    if fade:
        mel.connect_material_property(scalar(m, "Alpha", 1.0, -250, 750), "", P.MP_OPACITY)
    finish(m)


def build_trim(mpc, fade):
    name = "M_TrimFade" if fade else "M_Trim"
    m = recreate(name, unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("two_sided", True)
    if fade:
        m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
        m.set_editor_property("translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    tint = vector(m, "Tint", unreal.LinearColor(0.9, 0.9, 0.9, 1), -800, -200)
    metal = scalar(m, "Metallic", 0.0, -800, 0)
    mel.connect_material_property(tint, "", P.MP_BASE_COLOR)
    mel.connect_material_property(metal, "", P.MP_METALLIC)
    mel.connect_material_property(scalar(m, "Roughness", 0.45, -800, 150), "", P.MP_ROUGHNESS)
    n = custom(m, -500, 300, "return float3(0, 0, Sign);", ["Sign"], unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    link(node(m, unreal.MaterialExpressionTwoSidedSign, -800, 300), "", n, "Sign")
    mel.connect_material_property(n, "", P.MP_NORMAL)
    dielectric = node(m, unreal.MaterialExpressionOneMinus, -600, 0)
    link(metal, "", dielectric, "")
    amb = mul(m, mul(m, tint, dielectric, -400, -100), collection(m, mpc, "Ambient", -600, 450), -200, -100)
    mel.connect_material_property(amb, "", P.MP_EMISSIVE_COLOR)
    if fade:
        mel.connect_material_property(scalar(m, "Alpha", 1.0, -200, 300), "", P.MP_OPACITY)
    finish(m)


# ---------------------------------------------------------------------------- body proxy
BODY = "float3 p = float3(dot(W0.xyz, WorldPos) + W0.w, dot(W1.xyz, WorldPos) + W1.w, dot(W2.xyz, WorldPos) + W2.w);\n"

PROXY_MASK_CODE = BODY + """
if (Trim.x < 0.5) return 1;
// over the garment the mask can't tell a hand from the torso behind it: keep the shell off the
// garment's outline on screen (a fitted core hides the cloth there)
float4 CA[12] = { CA0, CA1, CA2, CA3, CA4, CA5, CA6, CA7, CA8, CA9, CA10, CA11 };
float4 CB[12] = { CB0, CB1, CB2, CB3, CB4, CB5, CB6, CB7, CB8, CB9, CB10, CB11 };
float3 o = Cam.xyz, d = normalize(p - o);
int n = (int)Flags.z;
for (int k = 0; k < 12; k++)
{
    if (k >= n) break;
    float3 a = CA[k].xyz, ab = CB[k].xyz - a, ao = a - o;
    float de = dot(d, ab), ee = dot(ab, ab), den = ee - de * de;
    float s = den > 1e-8 ? saturate((dot(d, ao) * de - dot(ab, ao)) / den) : 0;
    float3 w = a + ab * s - o;
    float t = dot(w, d), r = CA[k].w;
    if (dot(w, w) - t * t < r * r) return 0;
}
// the person mask, where this part was when the mask was captured
float3 b = p - float3(Off1.x, Off1.y, Off2.x);
float3 q = float3(dot(M0.xyz, b) + M0.w, dot(M1.xyz, b) + M1.w, dot(M2.xyz, b) + M2.w);
float2 uv = q.xy / max(q.z, 1e-4);
if (q.z <= 0 || any(uv <= 0) || any(uv >= 1)) return 0;
return Texture2DSampleLevel(MaskTex, MaskTexSampler, uv, 0).r - Flags.y + 0.5;
"""

PROXY_COLOR_CODE = BODY + """
if (Flags.w > 0.5)
{
    float3 c = 0.2 + 0.5 * abs(normalize(Nrm));
    return Trim.x > 0.5 ? c * float3(1, 0.75, 0.45) : c;      // debug view: trimmed parts warm
}
if (Flags.x <= 0) return 0;                                    // Mirror: black, the mirror shows the body
// CameraFeed: the feed pixel this point of the body projects to, so the proxy reads as the body
float3 q = float3(dot(M0.xyz, p) + M0.w, dot(M1.xyz, p) + M1.w, dot(M2.xyz, p) + M2.w);
float2 uv = saturate(q.xy / max(q.z, 1e-4));
return Texture2DSampleLevel(FeedTex, FeedTexSampler, uv, 0).rgb * Flags.x;
"""

PROXY_PUSH_CODE = "return normalize(WorldPos - CamPos) * Push.y * 100;   // away from the viewer, m -> cm"


def build_proxy(mpc):
    m = recreate("M_Proxy", unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("two_sided", True)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    m.set_editor_property("opacity_mask_clip_value", 0.5)
    wp = node(m, unreal.MaterialExpressionWorldPosition, -1600, 0,
              world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    uv1 = node(m, unreal.MaterialExpressionTextureCoordinate, -1600, 150, coordinate_index=1)
    uv2 = node(m, unreal.MaterialExpressionTextureCoordinate, -1600, 250, coordinate_index=2)
    uv3 = node(m, unreal.MaterialExpressionTextureCoordinate, -1600, 350, coordinate_index=3)
    mask_tex = node(m, unreal.MaterialExpressionTextureObjectParameter, -1600, 450, parameter_name="MaskTex",
                    texture=unreal.load_asset(DEFAULT_TEX))
    feed_tex = node(m, unreal.MaterialExpressionTextureObjectParameter, -1600, 600, parameter_name="FeedTex",
                    texture=unreal.load_asset(DEFAULT_TEX))
    c = {}
    y = 750
    for n in ["MaskM0", "MaskM1", "MaskM2", "W2B0", "W2B1", "W2B2", "CamBody", "Flags"] + \
             ["CoverA%d" % i for i in range(12)] + ["CoverB%d" % i for i in range(12)]:
        c[n] = collection(m, mpc, n, -1600, y)
        y += 60

    def wire(cn, extra):
        for pin, src in [("WorldPos", wp), ("M0", c["MaskM0"]), ("M1", c["MaskM1"]), ("M2", c["MaskM2"]),
                         ("W0", c["W2B0"]), ("W1", c["W2B1"]), ("W2", c["W2B2"]), ("Flags", c["Flags"]), ("Trim", uv3)] + extra:
            link(src, "", cn, pin)

    names = ["WorldPos", "M0", "M1", "M2", "W0", "W1", "W2", "Flags", "Trim"]
    covers = ["CA%d" % i for i in range(12)] + ["CB%d" % i for i in range(12)]
    opacity = custom(m, -800, 0, PROXY_MASK_CODE, names + ["Cam", "Off1", "Off2", "MaskTex"] + covers,
                     unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    wire(opacity, [("Cam", c["CamBody"]), ("Off1", uv1), ("Off2", uv2), ("MaskTex", mask_tex)]
         + [("CA%d" % i, c["CoverA%d" % i]) for i in range(12)] + [("CB%d" % i, c["CoverB%d" % i]) for i in range(12)])
    mel.connect_material_property(opacity, "", P.MP_OPACITY_MASK)

    color = custom(m, -800, 400, PROXY_COLOR_CODE, names + ["Nrm", "FeedTex"], unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    wire(color, [("Nrm", node(m, unreal.MaterialExpressionVertexNormalWS, -1100, 500)), ("FeedTex", feed_tex)])
    mel.connect_material_property(mul(m, color, inverse_exposure(m, -800, 650), -500, 400), "", P.MP_EMISSIVE_COLOR)

    push = custom(m, -800, 800, PROXY_PUSH_CODE, ["WorldPos", "CamPos", "Push"], unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    link(node(m, unreal.MaterialExpressionWorldPosition, -1100, 800), "", push, "WorldPos")
    link(node(m, unreal.MaterialExpressionCameraPositionWS, -1100, 900), "", push, "CamPos")
    link(uv2, "", push, "Push")
    mel.connect_material_property(push, "", P.MP_WORLD_POSITION_OFFSET)
    finish(m)


def build_feed():
    m = recreate("M_Feed", unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("two_sided", True)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    tex = texture(m, "FeedTex", node(m, unreal.MaterialExpressionTextureCoordinate, -900, 0), -700, 0)
    col = mul(m, tex, scalar(m, "Dim", 1.0, -700, 250), -450, 0, a_out="RGB")
    mel.connect_material_property(mul(m, col, inverse_exposure(m, -450, 250), -250, 0), "", P.MP_EMISSIVE_COLOR)
    finish(m)


def build():
    if not eal.does_directory_exist(DIR):
        eal.make_directory(DIR)
    mpc = build_collection()
    build_fabric(mpc, False)
    build_fabric(mpc, True)
    build_trim(mpc, False)
    build_trim(mpc, True)
    build_proxy(mpc)
    build_feed()
    unreal.log("ReflectAR setup: materials written to " + DIR)


if __name__ == "__main__":
    build()
