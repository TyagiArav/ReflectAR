// Body-proxy occlusion prepass: writes depth only, so garment parts behind the body are hidden.
Shader "ReflectAR/DepthOnly"
{
    Properties { _ColorMask ("Color mask (0 = depth only, 15 = debug view)", Float) = 0 }
    SubShader
    {
        Tags { "RenderPipeline"="UniversalPipeline" "RenderType"="Opaque" "Queue"="Geometry-10" }
        Pass
        {
            Name "DepthOnlyProxy"
            Tags { "LightMode"="UniversalForward" }
            ColorMask [_ColorMask]
            ZWrite On
            ZTest LEqual
            Cull Off
            Offset 1, 1
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"
            CBUFFER_START(UnityPerMaterial)
                float _ColorMask;
            CBUFFER_END
            struct A { float4 pos : POSITION; float3 n : NORMAL; };
            struct V { float4 pos : SV_POSITION; float3 n : TEXCOORD0; };
            V vert (A i) { V o; o.pos = TransformObjectToHClip(i.pos.xyz); o.n = TransformObjectToWorldNormal(i.n); return o; }
            half4 frag (V i) : SV_Target { return half4(0.2 + 0.5 * abs(normalize(i.n)), 1); }
            ENDHLSL
        }
    }
}
