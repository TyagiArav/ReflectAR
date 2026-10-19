// Body-proxy occlusion prepass: writes depth only, so garment parts behind the body are hidden.
// Exposed parts (_UseMask = 1) are oversized and trimmed here to the person mask: each fragment is
// projected into the camera image, at where that body part was when the mask was captured
// (_MaskOffset), and dropped if the mask says it isn't the person. _DepthPush moves the part away
// from the viewer by its oversize, so its front stays at the real body surface.
//
// HDRP: depth goes in the depth prepass (DepthForwardOnly), so the body also hides opaque
// garments drawn in the GBuffer and feeds SSAO / contact shadows. The colour pass then draws
// what the camera feed shows at that spot (CameraFeed mode) or black (Mirror), because in HDRP
// the feed backdrop is drawn after the prepass and can't show through the proxies' depth.
// The proxies also cast shadows (hands, arms and head shadowing the garment).
Shader "ReflectAR/DepthOnly"
{
    Properties
    {
        _ColorMask ("Color mask (0 = depth only, 15 = debug view)", Float) = 0
        _UseMask ("Trim to person mask", Float) = 0
        _DepthPush ("Push back (m)", Float) = 0
        _MaskOffset ("Motion since mask capture (body coords)", Vector) = (0, 0, 0, 0)
    }
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
                float _ColorMask, _UseMask, _DepthPush;
                float4 _MaskOffset;
            CBUFFER_END
            TEXTURE2D(_ReflectARMask); SAMPLER(sampler_ReflectARMask);
            float4x4 _ReflectARMaskMatrix;     // body coords -> (u / w, v / h, 1) * camera depth
            float4x4 _ReflectARWorldToBody;    // world -> body coords (MirrorWorld local: the unreflected body)
            float _ReflectARMaskThreshold;

            struct A { float4 pos : POSITION; float3 n : NORMAL; };
            struct V { float4 pos : SV_POSITION; float3 n : TEXCOORD0; float3 body : TEXCOORD1; };

            V vert (A i)
            {
                V o;
                float3 w = TransformObjectToWorld(i.pos.xyz);
                float3 vs = TransformWorldToView(w);
                vs.z -= _DepthPush;                 // view space looks down -z: further from the viewer
                o.pos = TransformWViewToHClip(vs);
                o.n = TransformObjectToWorldNormal(i.n);
                o.body = mul(_ReflectARWorldToBody, float4(w, 1)).xyz - _MaskOffset.xyz;
                return o;
            }

            half4 frag (V i) : SV_Target
            {
                if (_UseMask > 0.5)
                {
                    float3 q = mul(_ReflectARMaskMatrix, float4(i.body, 1)).xyz;
                    float2 uv = q.xy / max(q.z, 1e-4);
                    bool inside = q.z > 0 && all(uv > 0) && all(uv < 1);
                    float m = inside ? SAMPLE_TEXTURE2D_LOD(_ReflectARMask, sampler_ReflectARMask, uv, 0).r : 0;
                    clip(m - _ReflectARMaskThreshold);
                }
                half3 c = 0.2 + 0.5 * abs(normalize(i.n));
                return half4(_UseMask > 0.5 ? c * half3(1, 0.75, 0.45) : c, 1);   // debug view: trimmed parts warm
            }
            ENDHLSL
        }
    }
    SubShader
    {
        Tags { "RenderPipeline"="HDRenderPipeline" "RenderType"="Opaque" "Queue"="Geometry-10" }
        HLSLINCLUDE
        #pragma target 4.5
        #include "Packages/com.unity.render-pipelines.core/ShaderLibrary/Common.hlsl"
        #include "Packages/com.unity.render-pipelines.high-definition/Runtime/ShaderLibrary/ShaderVariables.hlsl"
        CBUFFER_START(UnityPerMaterial)
            float _ColorMask, _UseMask, _DepthPush;
            float4 _MaskOffset;
        CBUFFER_END
        TEXTURE2D(_ReflectARMask); SAMPLER(sampler_ReflectARMask);
        TEXTURE2D(_ReflectARFeed); SAMPLER(sampler_ReflectARFeed);
        float4x4 _ReflectARMaskMatrix;     // body coords -> (u / w, v / h, 1) * camera depth
        float4x4 _ReflectARWorldToBody;    // world -> body coords (MirrorWorld local: the unreflected body)
        float _ReflectARMaskThreshold;
        float _ReflectARFeedOn;            // CameraFeed mode: feed brightness (its _Dim), else 0

        struct A { float4 pos : POSITION; float3 n : NORMAL; };
        struct V { float4 pos : SV_POSITION; float3 n : TEXCOORD0; float3 body : TEXCOORD1; float3 bodyNow : TEXCOORD2; };

        V Vert (A i, float push)
        {
            V o;
            float3 w = TransformObjectToWorld(i.pos.xyz);                 // camera-relative in HDRP
            float3 vs = TransformWorldToView(w);
            vs.z -= push;
            o.pos = TransformWViewToHClip(vs);
            o.n = TransformObjectToWorldNormal(i.n);
            o.bodyNow = mul(_ReflectARWorldToBody, float4(GetAbsolutePositionWS(w), 1)).xyz;
            o.body = o.bodyNow - _MaskOffset.xyz;
            return o;
        }

        float2 ImageUV (float3 body)
        {
            float3 q = mul(_ReflectARMaskMatrix, float4(body, 1)).xyz;
            return q.xy / max(q.z, 1e-4);
        }

        void Trim (V i)
        {
            if (_UseMask > 0.5)
            {
                float3 q = mul(_ReflectARMaskMatrix, float4(i.body, 1)).xyz;
                float2 uv = q.xy / max(q.z, 1e-4);
                bool inside = q.z > 0 && all(uv > 0) && all(uv < 1);
                float m = inside ? SAMPLE_TEXTURE2D_LOD(_ReflectARMask, sampler_ReflectARMask, uv, 0).r : 0;
                clip(m - _ReflectARMaskThreshold);
            }
        }
        ENDHLSL

        Pass
        {
            Name "DepthForwardOnly"
            Tags { "LightMode"="DepthForwardOnly" }
            ColorMask 0
            ZWrite On
            ZTest LEqual
            Cull Off
            Offset 1, 1
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            V vert (A i) { return Vert(i, _DepthPush); }
            void frag (V i) { Trim(i); }
            ENDHLSL
        }

        Pass
        {
            Name "ForwardOnly"
            Tags { "LightMode"="ForwardOnly" }
            ZWrite On
            ZTest LEqual
            Cull Off
            Offset 1, 1
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            V vert (A i) { return Vert(i, _DepthPush); }
            float4 frag (V i) : SV_Target
            {
                Trim(i);
                float3 c;
                if (_ColorMask > 0.5)
                {
                    c = 0.2 + 0.5 * abs(normalize(i.n));
                    if (_UseMask > 0.5) c *= float3(1, 0.75, 0.45);           // debug view: trimmed parts warm
                }
                else
                {
                    // the feed pixel this point of the body projects to (feed texture v is up)
                    float2 uv = ImageUV(i.bodyNow);
                    c = _ReflectARFeedOn > 0 ? SAMPLE_TEXTURE2D_LOD(_ReflectARFeed, sampler_ReflectARFeed, float2(uv.x, 1 - uv.y), 0).rgb * _ReflectARFeedOn : 0;
                }
                return float4(c / GetCurrentExposureMultiplier(), 1);
            }
            ENDHLSL
        }

        Pass
        {
            Name "ShadowCaster"
            Tags { "LightMode"="ShadowCaster" }
            ColorMask 0
            ZWrite On
            ZTest LEqual
            Cull Off
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            V vert (A i) { return Vert(i, 0); }
            void frag (V i) { Trim(i); }
            ENDHLSL
        }
    }
}
