using System.Collections.Generic;
using UnityEngine;

namespace ProcAudio
{
    public enum SurfacePattern { Plain, Planks, Tiles, Carpet, Concrete, Brushed, Stone }

    /// <summary>
    /// How a surface looks: a colour plus a small procedural texture, generated at
    /// runtime so the demo needs no texture assets. Also used by SurfaceSelector.
    /// </summary>
    [System.Serializable]
    public class LookSpec
    {
        public SurfacePattern pattern = SurfacePattern.Plain;
        public Color color = Color.gray;
        [Range(0f, 1f)] public float smoothness = 0.3f;
        [Range(0f, 1f)] public float metallic = 0f;
        [Tooltip("Size of one texture repeat in metres")]
        public float metersPerTile = 1f;
    }

    public class SurfaceLook : MonoBehaviour
    {
        public LookSpec look = new LookSpec();
        [Tooltip("World size of the textured face (x, z); 0 = from the renderer bounds")]
        public Vector2 faceSize;

        void Start() => Apply(GetComponent<Renderer>(), look, faceSize);

        static readonly Dictionary<(SurfacePattern, int), Texture2D> Cache = new Dictionary<(SurfacePattern, int), Texture2D>();

        public static void Apply(Renderer r, LookSpec look, Vector2 faceSize)
        {
            if (r == null || look == null) return;
            var mat = r.material;   // instance
            var tex = Texture(look.pattern);
            if (mat.HasProperty("_BaseColor")) mat.SetColor("_BaseColor", look.color);
            if (mat.HasProperty("_Smoothness")) mat.SetFloat("_Smoothness", look.smoothness);
            if (mat.HasProperty("_Metallic")) mat.SetFloat("_Metallic", look.metallic);
            if (mat.HasProperty("_BaseMap"))
            {
                mat.SetTexture("_BaseMap", tex);
                if (faceSize == Vector2.zero)
                {
                    var b = r.bounds.size;
                    faceSize = new Vector2(Mathf.Max(b.x, 0.01f), Mathf.Max(b.z, 0.01f));
                }
                float m = Mathf.Max(0.05f, look.metersPerTile);
                mat.SetTextureScale("_BaseMap", new Vector2(faceSize.x / m, faceSize.y / m));
            }
        }

        public static Texture2D Texture(SurfacePattern p)
        {
            const int N = 256;
            if (Cache.TryGetValue((p, N), out var t) && t != null) return t;
            t = new Texture2D(N, N, TextureFormat.RGBA32, true) { wrapMode = TextureWrapMode.Repeat, name = p.ToString() };
            var px = new Color[N * N];
            var rng = new System.Random((int)p * 7919 + 13);
            float Noise(float x, float y, float f) => Mathf.PerlinNoise(x * f + (int)p * 17.3f, y * f + 3.1f);
            for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x)
            {
                float u = x / (float)N, v = y / (float)N, g = 1f;
                switch (p)
                {
                    case SurfacePattern.Planks:
                    {
                        // 4 boards per tile, staggered joints, grain along u
                        int board = (int)(v * 4f);
                        float bv = v * 4f - board;
                        float offset = (board * 0.37f) % 1f;
                        float bu = (u + offset) % 1f;
                        float grain = Noise(u * 0.5f, v * 40f, 1f) * 0.25f + Noise(u, v, 12f) * 0.1f;
                        float tone = 0.85f + 0.15f * Mathf.Sin(board * 2.3f + 1f);
                        g = tone * (0.8f + grain);
                        if (bv < 0.03f || bu < 0.006f) g *= 0.45f;
                        break;
                    }
                    case SurfacePattern.Tiles:
                    {
                        // 4 x 4 tiles with grout
                        float tu = u * 4f % 1f, tv = v * 4f % 1f;
                        bool grout = tu < 0.04f || tv < 0.04f;
                        g = grout ? 0.55f : 0.95f + 0.05f * Noise(u, v, 30f);
                        break;
                    }
                    case SurfacePattern.Carpet:
                        g = 0.75f + 0.25f * (float)rng.NextDouble() * Noise(u, v, 60f) + 0.1f * Noise(u, v, 6f);
                        break;
                    case SurfacePattern.Concrete:
                        g = 0.8f + 0.12f * Noise(u, v, 8f) + 0.08f * (float)rng.NextDouble();
                        break;
                    case SurfacePattern.Brushed:
                        g = 0.85f + 0.1f * Noise(u * 0.2f, v * 90f, 1f) + 0.04f * (float)rng.NextDouble();
                        break;
                    case SurfacePattern.Stone:
                    {
                        // 2 x 2 large slabs, mottled
                        float tu = u * 2f % 1f, tv = v * 2f % 1f;
                        bool joint = tu < 0.015f || tv < 0.015f;
                        g = joint ? 0.5f : 0.7f + 0.3f * Noise(u, v, 5f) * Noise(u, v, 17f) + 0.05f * (float)rng.NextDouble();
                        break;
                    }
                    default:
                        g = 1f;
                        break;
                }
                px[y * N + x] = new Color(g, g, g, 1f);
            }
            t.SetPixels(px);
            t.Apply(true);
            Cache[(p, N)] = t;
            return t;
        }
    }
}
