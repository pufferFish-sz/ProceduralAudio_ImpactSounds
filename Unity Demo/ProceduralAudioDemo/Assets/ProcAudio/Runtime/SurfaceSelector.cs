using System;
using System.Collections.Generic;
using UnityEngine;

namespace ProcAudio
{
    [Serializable]
    public class SurfaceOption
    {
        public string name = "Concrete";
        public BodyDescription description = BodyDescription.Make(PaShape.Ground, PaMaterial.Concrete, 4f, PaMount.BuiltIn);
        public LookSpec look = new LookSpec();
        [Header("Physics")]
        [Range(0f, 1.5f)] public float friction = 0.6f;
        [Range(0f, 1f)] public float bounciness = 0.1f;
    }

    /// <summary>
    /// Switches what a surface is (the body it describes to the objects' FMOD
    /// events, its look, its physics material) between a list of options. The HUD
    /// dropdown and the number keys drive it.
    /// </summary>
    [RequireComponent(typeof(SoundBody))]
    public class SurfaceSelector : MonoBehaviour
    {
        public List<SurfaceOption> options = new List<SurfaceOption>();
        public int current;
        public Renderer[] renderers;
        public Collider[] colliders;
        public Vector2 faceSize;

        public event Action<int> Changed;

        SoundBody surface;
        readonly Dictionary<int, PhysicsMaterial> physics = new Dictionary<int, PhysicsMaterial>();

        void Awake() => surface = GetComponent<SoundBody>();

        void Start() => Select(current, force: true);

        public void Select(int i, bool force = false)
        {
            if (options.Count == 0) return;
            i = Mathf.Clamp(i, 0, options.Count - 1);
            if (i == current && !force) return;
            current = i;
            var o = options[i];
            surface.description = o.description;
            surface.displayName = o.name;
            foreach (var r in renderers) SurfaceLook.Apply(r, o.look, faceSize);
            if (!physics.TryGetValue(i, out var pm))
            {
                pm = new PhysicsMaterial(o.name)
                {
                    dynamicFriction = o.friction,
                    staticFriction = o.friction * 1.1f,
                    bounciness = o.bounciness,
                    frictionCombine = PhysicsMaterialCombine.Average,
                    bounceCombine = PhysicsMaterialCombine.Average,
                };
                physics[i] = pm;
            }
            foreach (var c in colliders) c.sharedMaterial = pm;
            var mgr = ContactAudioManager.Instance;
            if (mgr != null && !force)
            {
                mgr.AddLog($"Floor is now: {o.name}");
                mgr.WarmUp(surface);   // build the objects' roll / slide tables for it
            }
            Changed?.Invoke(i);
        }

        /// <summary>The demo's floors: what the plug-in offers for a floor.</summary>
        public static List<SurfaceOption> DefaultOptions()
        {
            SurfaceOption O(string n, BodyDescription d, SurfacePattern p, Color c, float smooth, float mPerTile,
                            float friction, float bounce, float metallic = 0f) =>
                new SurfaceOption
                {
                    name = n, description = d, friction = friction, bounciness = bounce,
                    look = new LookSpec { pattern = p, color = c, smoothness = smooth, metallic = metallic, metersPerTile = mPerTile }
                };
            const PaMount B = PaMount.BuiltIn;
            return new List<SurfaceOption>
            {
                O("Concrete slab", BodyDescription.Make(PaShape.Ground, PaMaterial.Concrete, 4f, B),
                  SurfacePattern.Concrete, new Color(0.62f, 0.62f, 0.60f), 0.15f, 3f, 0.65f, 0.15f),
                O("Wood floor", BodyDescription.Make(PaShape.Panel, PaMaterial.Wood, 2.4f, B),
                  SurfacePattern.Planks, new Color(0.66f, 0.47f, 0.30f), 0.25f, 1.6f, 0.45f, 0.2f),
                O("Varnished wood", BodyDescription.Make(PaShape.Panel, PaMaterial.Wood, 2.4f, B, PaFinish.Polished),
                  SurfacePattern.Planks, new Color(0.55f, 0.33f, 0.18f), 0.8f, 1.6f, 0.3f, 0.2f),
                O("Tiled floor", BodyDescription.Make(PaShape.Ground, PaMaterial.Concrete, 4f, B, PaFinish.Tiled),
                  SurfacePattern.Tiles, new Color(0.88f, 0.88f, 0.84f), 0.75f, 1.2f, 0.4f, 0.15f),
                O("Carpet (on wood)", BodyDescription.Make(PaShape.Panel, PaMaterial.Wood, 2.4f, B, PaFinish.Carpeted),
                  SurfacePattern.Carpet, new Color(0.35f, 0.42f, 0.55f), 0.0f, 0.5f, 0.95f, 0.0f),
                O("Stone slab", BodyDescription.Make(PaShape.Ground, PaMaterial.Stone, 4f, B),
                  SurfacePattern.Stone, new Color(0.55f, 0.53f, 0.50f), 0.35f, 3f, 0.55f, 0.15f),
                O("Steel deck", BodyDescription.Make(PaShape.Panel, PaMaterial.Metal, 2.4f, B, PaFinish.Bare, 0.008f),
                  SurfacePattern.Brushed, new Color(0.70f, 0.72f, 0.75f), 0.6f, 1f, 0.35f, 0.2f, 0.9f),
            };
        }
    }
}
