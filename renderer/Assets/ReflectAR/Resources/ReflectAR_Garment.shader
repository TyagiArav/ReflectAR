// Fabric shader for simulated garments (URP; under HDRP garments use HDRP/Lit, see GarmentMaterials).
// - Fabric texture is projected triplanar from each vertex's REST position (uv2) and rest normal
//   (uv3), so the pattern is seamless and stays glued to the cloth while it moves and wrinkles.
//   (uv0 holds the pattern-piece layout that HDRP/Lit uses.)
// - Two-sided wrapped diffuse + a soft sheen rim (cloth scatters light at grazing angles).
// - Weave height (texture alpha) gives micro shading; silhouette alpha feathers for the mirror.
Shader "ReflectAR/Garment"
{
    Properties
    {
        _BaseMap ("Fabric (rgb colour, a weave height)", 2D) = "white" {}
        _BaseColor ("Tint", Color) = (1,1,1,1)
        _Tile ("Repeats per metre", Float) = 4
        _Alpha ("Alpha", Range(0,1)) = 1
        _Boost ("Brightness", Float) = 1.0
        _Sheen ("Sheen", Range(0,1)) = 0.25
        _Feather ("Edge feather", Range(0,0.6)) = 0.15
    }
    SubShader
    {
        Tags { "RenderPipeline"="UniversalPipeline" "Queue"="Geometry+50" "RenderType"="Transparent" }
        Pass
        {
            Name "Garment"
            Tags { "LightMode"="UniversalForward" }
            Cull Off
            ZWrite On
            ZTest LEqual
            Blend SrcAlpha OneMinusSrcAlpha
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"
            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Lighting.hlsl"

            TEXTURE2D(_BaseMap); SAMPLER(sampler_BaseMap);
            CBUFFER_START(UnityPerMaterial)
                float4 _BaseMap_ST;
                half4 _BaseColor;
                float _Tile;
                half _Alpha, _Boost, _Sheen, _Feather;
            CBUFFER_END

            struct A { float4 pos : POSITION; float3 n : NORMAL; float3 rest : TEXCOORD2; float3 restN : TEXCOORD3; };
            struct V { float4 pos : SV_POSITION; float3 n : TEXCOORD0; float3 rest : TEXCOORD1; float3 restN : TEXCOORD2; float3 wp : TEXCOORD3; };

            V vert (A i)
            {
                V o;
                o.wp = TransformObjectToWorld(i.pos.xyz);
                o.pos = TransformWorldToHClip(o.wp);
                o.n = TransformObjectToWorldNormal(i.n);
                o.rest = i.rest;
                o.restN = i.restN;
                return o;
            }

            half4 Tri(float3 p, float3 n)
            {
                float3 w = pow(abs(n), 4.0);
                w /= max(w.x + w.y + w.z, 1e-4);
                p *= _Tile;
                half4 cx = SAMPLE_TEXTURE2D(_BaseMap, sampler_BaseMap, p.zy);
                half4 cy = SAMPLE_TEXTURE2D(_BaseMap, sampler_BaseMap, p.xz);
                half4 cz = SAMPLE_TEXTURE2D(_BaseMap, sampler_BaseMap, p.xy);
                return cx * w.x + cy * w.y + cz * w.z;
            }

            half4 frag (V i) : SV_Target
            {
                float3 n = normalize(i.n);
                float3 v = normalize(_WorldSpaceCameraPos - i.wp);
                float ndv = dot(n, v);
                if (ndv < 0) { n = -n; ndv = -ndv; }
                Light l = GetMainLight();
                half4 fab = Tri(i.rest, normalize(i.restN + 1e-4));
                half3 albedo = fab.rgb * _BaseColor.rgb;
                half ndl = dot(n, l.direction);
                half diff = saturate(ndl * 0.7 + 0.3);
                half sheen = pow(1.0 - ndv, 3.0) * _Sheen * saturate(ndl * 0.5 + 0.5);
                half weave = lerp(0.88, 1.06, fab.a);
                half3 col = (albedo * (0.26 + 0.82 * diff * l.color) * weave + sheen * lerp(albedo, 1.0, 0.4)) * _Boost;
                half edge = smoothstep(0.0, _Feather, ndv);
                return half4(col, _Alpha * lerp(0.6, 1.0, edge));
            }
            ENDHLSL
        }
    }
}
