// Fabric shader for simulated garments.
// - Fabric texture is projected triplanar from each vertex's REST position (uv0) and rest normal
//   (uv1), so the pattern is seamless and stays glued to the cloth while it moves and wrinkles.
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
        _Placket ("Front closure (0 none, 1 buttons, 2 zipper)", Float) = 0
        _Front ("x neck y, y hem y, z closure bottom y, w body unit", Vector) = (0.55, -0.05, -0.05, 1)
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
                float _Placket;
                float4 _Front;
            CBUFFER_END

            struct A { float4 pos : POSITION; float3 n : NORMAL; float3 rest : TEXCOORD0; float3 restN : TEXCOORD1; };
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
                // front closure, placed on the rest-pose centre front so it rides along with the fabric
                if (_Placket > 0.5 && i.rest.z > 0 && i.restN.z > 0.25 && i.rest.y < _Front.x && i.rest.y > _Front.z)
                {
                    float u = _Front.w, dx = abs(i.rest.x);
                    float edge = 1.0 - smoothstep(0.0, 0.0025 * u, abs(dx - 0.017 * u));     // placket stitch lines
                    albedo *= 1.0 - 0.35 * edge;
                    if (_Placket < 1.5)
                    {
                        float pitch = 0.085 * u;
                        float yb = fmod(_Front.x - 0.03 * u - i.rest.y + 100.0 * pitch, pitch) - pitch * 0.5;
                        float r = length(float2(i.rest.x, yb));
                        if (r < 0.007 * u) albedo = lerp(albedo, half3(0.86, 0.84, 0.8), 0.9) * (0.75 + 0.5 * smoothstep(0.007 * u, 0.0, r));
                    }
                    else if (dx < 0.0045 * u)
                    {
                        float teeth = step(0.5, frac(i.rest.y / (0.004 * u)));
                        albedo = lerp(half3(0.18, 0.18, 0.19), half3(0.55, 0.55, 0.58), teeth * 0.6);
                    }
                }
                half3 col = (albedo * (0.26 + 0.82 * diff * l.color) * weave + sheen * lerp(albedo, 1.0, 0.4)) * _Boost;
                half edge = smoothstep(0.0, _Feather, ndv);
                return half4(col, _Alpha * lerp(0.6, 1.0, edge));
            }
            ENDHLSL
        }
    }
}
