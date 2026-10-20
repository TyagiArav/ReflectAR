using UnityEngine;
using UnityEngine.UI;

namespace ReflectAR
{
    /// <summary>
    /// Runtime-built UGUI: range prompt (top), garment carousel (bottom strip, off the body),
    /// fist-hold progress ring at the hand, selection toast and a debug line.
    /// </summary>
    public class MirrorUI
    {
        readonly GarmentSpec[] specs;
        readonly RectTransform canvasRt, carousel, ringRt;
        readonly RectTransform[] cards;
        readonly Image[] cardBg;
        readonly Text[] cardTick;
        readonly Text prompt, promptSub, hint, debug, toast, ringLabel;
        readonly Image ring;
        readonly GameObject promptPanel;
        float toastT = -10f, carouselX;

        static Font font;
        static readonly Color Panel = new Color(0, 0, 0, 0.55f), Accent = new Color(1f, 0.82f, 0.35f);

        public MirrorUI(GarmentSpec[] specs)
        {
            this.specs = specs;
            font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");

            var cgo = new GameObject("MirrorUI", typeof(Canvas), typeof(CanvasScaler));
            var canvas = cgo.GetComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            var scaler = cgo.GetComponent<CanvasScaler>();
            scaler.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
            scaler.referenceResolution = new Vector2(1920, 1080);
            scaler.matchWidthOrHeight = 0.5f;
            canvasRt = cgo.GetComponent<RectTransform>();

            // ---- range prompt
            promptPanel = Box("Prompt", canvasRt, new Vector2(0.5f, 1f), new Vector2(0, -40), new Vector2(900, 150), Panel).gameObject;
            prompt = Label(promptPanel.transform, "", 64, FontStyle.Bold, new Vector2(0, 22), new Vector2(880, 80));
            promptSub = Label(promptPanel.transform, "", 28, FontStyle.Normal, new Vector2(0, -42), new Vector2(880, 40));

            // ---- carousel
            var strip = Box("Carousel", canvasRt, new Vector2(0.5f, 0f), new Vector2(0, 20), new Vector2(1920, 200), new Color(0, 0, 0, 0.35f));
            strip.anchorMin = new Vector2(0, 0); strip.anchorMax = new Vector2(1, 0); strip.sizeDelta = new Vector2(0, 200);
            hint = Label(canvasRt, "Swipe your hand  ←  →  to browse     ·     Raise a fist for 5 s to select",
                26, FontStyle.Normal, Vector2.zero, new Vector2(1400, 40));
            var hrt = hint.rectTransform; hrt.anchorMin = hrt.anchorMax = new Vector2(0.5f, 0f); hrt.anchoredPosition = new Vector2(0, 245);
            carousel = new GameObject("Cards", typeof(RectTransform)).GetComponent<RectTransform>();
            carousel.SetParent(strip, false);
            carousel.anchorMin = carousel.anchorMax = new Vector2(0.5f, 0.5f);
            cards = new RectTransform[specs.Length];
            cardBg = new Image[specs.Length];
            cardTick = new Text[specs.Length];
            for (int i = 0; i < specs.Length; i++)
            {
                var card = Box("Card_" + i, carousel, new Vector2(0.5f, 0.5f), new Vector2(i * 200, 0), new Vector2(180, 160), new Color(0.12f, 0.12f, 0.12f, 0.85f));
                cardBg[i] = card.GetComponent<Image>();
                var sw = Box("Swatch", card, new Vector2(0.5f, 1f), new Vector2(0, -12), new Vector2(150, 80), specs[i].colorA);
                if (specs[i].pattern != Pattern.Solid)
                {
                    var tex = PatternTex.Make(specs[i]);
                    var raw = new GameObject("Pattern", typeof(RectTransform), typeof(RawImage)).GetComponent<RawImage>();
                    raw.transform.SetParent(sw, false);
                    var rr = raw.rectTransform; rr.anchorMin = Vector2.zero; rr.anchorMax = Vector2.one; rr.offsetMin = rr.offsetMax = Vector2.zero;
                    raw.texture = tex; raw.uvRect = new Rect(0, 0, 1.5f, 0.8f);
                }
                Label(card, specs[i].name, 20, FontStyle.Bold, new Vector2(0, -40), new Vector2(170, 50));
                cardTick[i] = Label(card, "✓", 34, FontStyle.Bold, new Vector2(66, 54), new Vector2(40, 40));
                cardTick[i].color = Accent;
                cards[i] = card;
            }

            // ---- fist ring
            ringRt = new GameObject("FistRing", typeof(RectTransform), typeof(Image)).GetComponent<RectTransform>();
            ringRt.SetParent(canvasRt, false);
            ringRt.sizeDelta = new Vector2(150, 150);
            ring = ringRt.GetComponent<Image>();
            ring.sprite = RingSprite();
            ring.type = Image.Type.Filled;
            ring.fillMethod = Image.FillMethod.Radial360;
            ring.fillOrigin = (int)Image.Origin360.Top;
            ring.fillClockwise = true;
            ring.color = Accent;
            ringLabel = Label(ringRt, "", 46, FontStyle.Bold, Vector2.zero, new Vector2(140, 60));

            toast = Label(canvasRt, "", 54, FontStyle.Bold, new Vector2(0, 120), new Vector2(1200, 80));
            toast.color = Accent;
            debug = Label(canvasRt, "", 20, FontStyle.Normal, Vector2.zero, new Vector2(1100, 30));
            debug.alignment = TextAnchor.UpperLeft;
            var drt = debug.rectTransform; drt.anchorMin = drt.anchorMax = drt.pivot = new Vector2(0, 1); drt.anchoredPosition = new Vector2(16, -12);
        }

        public void OnBrowse(int d) { }

        public void Toast(string s) { toast.text = s; toastT = Time.time; }

        public void Update(TrackingFrame f, bool connected, bool tracked, int current, int selected,
                           ViewMode mode, bool showDebug, Camera cam, BodyRig body)
        {
            // ---- prompt
            string p = null, sub = null;
            if (!connected) { p = "Starting…"; sub = "Waiting for the perception service (perception/server.py)"; }
            else if (f == null || !f.tracked || f.range == "none") { p = "Step into view"; sub = "Stand 1–3 m from the mirror, facing it"; }
            else if (f.range == "too_close") { p = "Please step back"; sub = $"You're {f.dist:0.0} m away — stand 1–3 m from the mirror"; }
            else if (f.range == "too_far") { p = "Come a little closer"; sub = $"You're {f.dist:0.0} m away — stand 1–3 m from the mirror"; }
            promptPanel.SetActive(p != null);
            if (p != null) { prompt.text = p; promptSub.text = sub; }

            // ---- carousel: slide so the current card is centred
            carouselX = Mathf.Lerp(carouselX, -current * 200f, 1f - Mathf.Exp(-12f * Time.deltaTime));
            carousel.anchoredPosition = new Vector2(carouselX, 0);
            for (int i = 0; i < cards.Length; i++)
            {
                bool cur = i == current;
                float s = cur ? 1.12f : 0.92f;
                cards[i].localScale = Vector3.Lerp(cards[i].localScale, Vector3.one * s, 1f - Mathf.Exp(-14f * Time.deltaTime));
                cardBg[i].color = cur ? new Color(Accent.r, Accent.g, Accent.b, 0.95f) : new Color(0.12f, 0.12f, 0.12f, 0.8f);
                cardTick[i].enabled = i == selected;
            }

            // ---- fist ring at the hand
            float prog = f != null && f.tracked ? f.fist_progress : 0f;
            bool showRing = prog > 0.01f && body.Valid;
            ring.enabled = showRing; ringLabel.enabled = showRing;
            if (showRing)
            {
                int wi = f.fist_hand == "left" ? LM.LWrist : LM.RWrist;
                Vector3 w = body.J[wi];
                if (mode == ViewMode.Mirror) w.z = -w.z;          // the reflected hand
                Vector3 sp = cam.WorldToScreenPoint(w);
                RectTransformUtility.ScreenPointToLocalPointInRectangle(canvasRt, sp, null, out var lp);
                Vector2 half = canvasRt.rect.size * 0.5f - new Vector2(90, 90);
                ringRt.anchoredPosition = new Vector2(Mathf.Clamp(lp.x, -half.x, half.x), Mathf.Clamp(lp.y, -half.y + 220, half.y));
                ring.fillAmount = prog;
                ringLabel.text = Mathf.CeilToInt((1f - prog) * 5f).ToString();
            }

            // ---- toast
            float ta = Mathf.Clamp01(2.5f - (Time.time - toastT));
            toast.color = new Color(Accent.r, Accent.g, Accent.b, ta);

            hint.enabled = connected && f != null && f.tracked && f.range == "ok";

            debug.enabled = showDebug;
            if (showDebug)
                debug.text = f == null
                    ? $"{(connected ? "connected" : "no perception")}  ·  [M] mode  [←/→] browse  [Space] select  [B] proxy  [C] cube  [D] debug"
                    : $"{mode}  ·  camera {f.cam_fps:0} fps  perception {f.proc_fps:0} fps ({f.lat_ms:0} ms)  depth {f.depth_hz:0} Hz  ·  dist {f.dist:0.00} m (skel {f.z_skel:0.00} / mono {f.z_da:0.00}, s={f.scale:0.00})  ·  render {1f / Mathf.Max(Time.smoothDeltaTime, 1e-3f):0} fps  ·  [M] mode [B] proxy [C] cube [D] debug";
        }

        // ------------------------------------------------------------------ helpers
        static RectTransform Box(string name, Transform parent, Vector2 anchor, Vector2 pos, Vector2 size, Color c)
        {
            var go = new GameObject(name, typeof(RectTransform), typeof(Image));
            var rt = go.GetComponent<RectTransform>();
            rt.SetParent(parent, false);
            rt.anchorMin = rt.anchorMax = anchor;
            rt.pivot = new Vector2(0.5f, anchor.y);
            rt.anchoredPosition = pos;
            rt.sizeDelta = size;
            go.GetComponent<Image>().color = c;
            return rt;
        }

        static Text Label(Transform parent, string s, int size, FontStyle style, Vector2 pos, Vector2 box)
        {
            var go = new GameObject("Text", typeof(RectTransform), typeof(Text));
            var rt = go.GetComponent<RectTransform>();
            rt.SetParent(parent, false);
            rt.anchoredPosition = pos;
            rt.sizeDelta = box;
            var t = go.GetComponent<Text>();
            t.font = font; t.text = s; t.fontSize = size; t.fontStyle = style;
            t.alignment = TextAnchor.MiddleCenter; t.color = Color.white;
            t.horizontalOverflow = HorizontalWrapMode.Wrap; t.verticalOverflow = VerticalWrapMode.Overflow;
            var sh = go.AddComponent<Shadow>(); sh.effectColor = new Color(0, 0, 0, 0.8f); sh.effectDistance = new Vector2(2, -2);
            return t;
        }

        static Sprite RingSprite()
        {
            const int N = 128;
            var t = new Texture2D(N, N, TextureFormat.RGBA32, false);
            for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++)
            {
                float d = Vector2.Distance(new Vector2(x + 0.5f, y + 0.5f), new Vector2(N / 2f, N / 2f)) / (N / 2f);
                float a = Mathf.Clamp01((1f - d) * 40f) * Mathf.Clamp01((d - 0.78f) * 40f);
                t.SetPixel(x, y, new Color(1, 1, 1, a));
            }
            t.Apply();
            return Sprite.Create(t, new Rect(0, 0, N, N), new Vector2(0.5f, 0.5f));
        }
    }
}
