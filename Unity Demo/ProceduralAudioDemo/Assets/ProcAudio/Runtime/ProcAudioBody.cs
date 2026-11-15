using System;
using System.Collections.Generic;
using UnityEngine;

namespace ProcAudio
{
    /// <summary>
    /// A dynamic object that sounds through the procedural model.
    ///
    /// For every surface it meets it holds three native voices:
    ///   impact  - hits (object Free on the surface)
    ///   free    - rolling (balls) or sliding (everything else) when it moves on its own
    ///   held    - sliding while the player holds it (a held ball cannot turn, so it slides)
    /// plus one impact voice per other body it can hit. Voices are built on worker
    /// threads at start; idle voices sleep in the native engine and cost nothing.
    ///
    /// Physics -> sound:
    ///   OnCollisionEnter : normal relative speed -> impact
    ///   OnCollisionStay  : tangential speed at the contact -> slide speed; centre speed -> roll speed
    ///
    /// The sound is rendered in OnAudioFilterRead on a 3D AudioSource that loops a
    /// constant 1.0 clip: Unity's distance attenuation and panning act on that clip,
    /// and the filter multiplies the procedural signal in, so it is spatialized.
    /// </summary>
    [RequireComponent(typeof(Rigidbody))]
    [RequireComponent(typeof(AudioSource))]
    public class ProcAudioBody : MonoBehaviour
    {
        public string displayName = "Object";
        public BodyDescription description = BodyDescription.Make(PaShape.Ball, PaMaterial.Wood, 0.08f);

        [Tooltip("Set the Rigidbody mass from the model (material density x shape)")]
        public bool massFromModel = true;
        public float minMass = 0.05f;

        [Header("Contacts")]
        [Tooltip("Normal speeds below this make no impact (m/s)")]
        public float minImpactSpeed = 0.06f;
        [Tooltip("Shortest time between two impacts on the same surface (s)")]
        public float impactCooldown = 0.03f;
        [Tooltip("Roll / slide speeds below this are silent (m/s)")]
        public float minMotionSpeed = 0.02f;
        [Tooltip("Contact may drop out this long (s) before the roll / slide stops")]
        public float contactGrace = 0.06f;

        [Header("Audio source")]
        public float minDistance = 1f;
        public float maxDistance = 40f;

        /// <summary>Set by the grabber while the player holds this object.</summary>
        [NonSerialized] public bool held;

        public Rigidbody Body { get; private set; }
        public string Status { get; private set; } = "";
        public string LastImpact { get; private set; } = "";

        class VoiceRef
        {
            public int handle;
            public volatile int kind;   // gain category: ProcAudioNative.Motion*
        }

        class SurfaceVoices
        {
            public string name;
            public VoiceRef impact, free, heldV;
            public float lastTouch = -1f, lastImpact = -1f, speed;
            public int kind;
            public bool listed;
        }

        class Contact
        {
            public Vector3 point, normal;
            public int n;
        }

        readonly Dictionary<string, SurfaceVoices> surfaceVoices = new Dictionary<string, SurfaceVoices>();
        readonly Dictionary<string, VoiceRef> pairVoices = new Dictionary<string, VoiceRef>();
        readonly Dictionary<int, float> pairLastImpact = new Dictionary<int, float>();
        readonly Dictionary<ProcAudioSurface, Contact> touching = new Dictionary<ProcAudioSurface, Contact>();
        readonly List<SurfaceVoices> moving = new List<SurfaceVoices>();
        readonly ContactPoint[] cbuf = new ContactPoint[32];

        volatile VoiceRef[] refs = Array.Empty<VoiceRef>();   // what the audio thread renders
        float[] mono = new float[4096];
        volatile bool rendering;

        AudioSource src;
        Vector3 startPos;
        Quaternion startRot;

        static AudioClip dcClip;

        void Awake()
        {
            Body = GetComponent<Rigidbody>();
            src = GetComponent<AudioSource>();
            startPos = transform.position;
            startRot = transform.rotation;
        }

        void OnEnable() => ProcAudioEngine.Bodies.Add(this);
        void OnDisable() => ProcAudioEngine.Bodies.Remove(this);

        void Start()
        {
            var engine = ProcAudioEngine.Instance;
            if (massFromModel)
            {
                float m = ProcAudioEngine.BodyMass(description.WithMount(PaMount.Free));
                if (m > 0f) Body.mass = Mathf.Max(minMass, m);
            }
            if (engine == null || !engine.Available) return;

            SetupSource(engine.SampleRate);
            foreach (var s in ProcAudioEngine.Surfaces) Prewarm(s);
            foreach (var b in ProcAudioEngine.Bodies)
                if (b != this && GetInstanceID() < b.GetInstanceID()) GetPairVoice(b);
            rendering = true;
        }

        void SetupSource(int sampleRate)
        {
            if (dcClip == null || dcClip.frequency != sampleRate)
            {
                var ones = new float[sampleRate];
                for (int i = 0; i < ones.Length; ++i) ones[i] = 1f;
                dcClip = AudioClip.Create("ProcAudio DC", sampleRate, 1, sampleRate, false);
                dcClip.SetData(ones, 0);
            }
            src.clip = dcClip;
            src.loop = true;
            src.playOnAwake = false;
            src.spatialBlend = 1f;
            src.rolloffMode = AudioRolloffMode.Logarithmic;   // 1/r, like a point source
            src.minDistance = minDistance;
            src.maxDistance = maxDistance;
            src.dopplerLevel = 0f;
            src.volume = 1f;
            src.Play();
        }

        // ------------------------------------------------------------ voices

        /// <summary>Builds this body's voices for a surface (no-op if they exist).</summary>
        public void Prewarm(ProcAudioSurface s) => GetSurfaceVoices(s);

        SurfaceVoices GetSurfaceVoices(ProcAudioSurface s)
        {
            string key = s.Key;
            if (surfaceVoices.TryGetValue(key, out var sv))
            {
                sv.name = s.displayName;
                return sv;
            }
            var engine = ProcAudioEngine.Instance;
            if (engine == null) return null;
            var free = description.WithMount(PaMount.Free);
            var target = s.description;
            bool ball = description.shape == PaShape.Ball;
            sv = new SurfaceVoices
            {
                name = s.displayName,
                impact = AddRef(engine.CreateVoice(free, target, false), 0),
                free = AddRef(engine.CreateVoice(free, target, true),
                              ball ? ProcAudioNative.MotionRoll : ProcAudioNative.MotionSlide),
                heldV = AddRef(engine.CreateVoice(description.WithMount(PaMount.Held), target, true),
                               ProcAudioNative.MotionSlide),
            };
            surfaceVoices[key] = sv;
            return sv;
        }

        VoiceRef GetPairVoice(ProcAudioBody other)
        {
            var target = other.description.WithMount(PaMount.Free);
            string key = target.Key;
            if (pairVoices.TryGetValue(key, out var v)) return v;
            var engine = ProcAudioEngine.Instance;
            if (engine == null) return null;
            v = AddRef(engine.CreateVoice(description.WithMount(PaMount.Free), target, false), 0);
            pairVoices[key] = v;
            return v;
        }

        VoiceRef AddRef(int handle, int kind)
        {
            var r = new VoiceRef { handle = handle, kind = kind };
            if (handle <= 0) return r;
            var old = refs;
            var next = new VoiceRef[old.Length + 1];
            Array.Copy(old, next, old.Length);
            next[old.Length] = r;
            refs = next;
            return r;
        }

        // ------------------------------------------------------------ physics -> sound

        static ProcAudioSurface SurfaceOf(Collision c) =>
            c.collider != null ? c.collider.GetComponentInParent<ProcAudioSurface>() : null;

        void OnCollisionEnter(Collision c)
        {
            if (!rendering) return;
            int n = c.GetContacts(cbuf);
            if (n == 0) return;
            Vector3 nrm = Vector3.zero;
            for (int i = 0; i < n; ++i) nrm += cbuf[i].normal;
            nrm = nrm.sqrMagnitude > 1e-8f ? nrm.normalized : Vector3.up;
            float vn = Mathf.Abs(Vector3.Dot(c.relativeVelocity, nrm));

            var surf = SurfaceOf(c);
            if (surf != null)
            {
                Accumulate(surf, n);
                if (vn < minImpactSpeed) return;
                var sv = GetSurfaceVoices(surf);
                if (sv == null || Time.fixedTime - sv.lastImpact < impactCooldown) return;
                sv.lastImpact = Time.fixedTime;
                ProcAudioNative.pa_voice_impact(sv.impact.handle, vn);
                Report(sv.name, vn);
                return;
            }

            var other = c.rigidbody != null ? c.rigidbody.GetComponent<ProcAudioBody>() : null;
            if (other == null || other == this || vn < minImpactSpeed) return;
            if (GetInstanceID() > other.GetInstanceID()) return;   // the other one plays the pair
            int id = other.GetInstanceID();
            if (pairLastImpact.TryGetValue(id, out float t) && Time.fixedTime - t < impactCooldown) return;
            pairLastImpact[id] = Time.fixedTime;
            var pv = GetPairVoice(other);
            if (pv != null) ProcAudioNative.pa_voice_impact(pv.handle, vn);
            Report(other.displayName, vn);
            other.LastImpact = LastImpact;
        }

        void OnCollisionStay(Collision c)
        {
            if (!rendering) return;
            var surf = SurfaceOf(c);
            if (surf == null) return;
            Accumulate(surf, c.GetContacts(cbuf));
        }

        void Accumulate(ProcAudioSurface surf, int n)
        {
            if (n <= 0) return;
            if (!touching.TryGetValue(surf, out var ct)) touching[surf] = ct = new Contact();
            for (int i = 0; i < n; ++i)
            {
                ct.point += cbuf[i].point;
                ct.normal += cbuf[i].normal;
            }
            ct.n += n;
        }

        void Report(string what, float vn)
        {
            LastImpact = $"hit {what} at {vn:0.00} m/s";
            if (vn > 0.4f && ProcAudioEngine.Instance != null)
                ProcAudioEngine.Instance.AddLog($"{displayName} → {what}: impact {vn:0.0} m/s");
        }

        // Contacts gathered during the last physics step (callbacks run after the
        // step, before the next FixedUpdate) become roll / slide speeds.
        void FixedUpdate()
        {
            if (!rendering) return;
            float now = Time.fixedTime;
            bool ball = description.shape == PaShape.Ball;
            foreach (var kv in touching)
            {
                var ct = kv.Value;
                if (ct.n == 0) continue;
                var sv = GetSurfaceVoices(kv.Key);
                Vector3 p = ct.point / ct.n;
                Vector3 nrm = ct.normal.sqrMagnitude > 1e-8f ? ct.normal.normalized : Vector3.up;
                ct.point = ct.normal = Vector3.zero;
                ct.n = 0;
                if (sv == null) continue;

                Vector3 vp = Body.GetPointVelocity(p);              // contact point: slipping
                Vector3 vc = Body.linearVelocity;                    // centre: rolling
                float slide = (vp - nrm * Vector3.Dot(vp, nrm)).magnitude;
                float roll = (vc - nrm * Vector3.Dot(vc, nrm)).magnitude;

                int kind;
                float speed;
                if (held) { kind = ProcAudioNative.MotionSlide; speed = slide; }
                else if (ball) { kind = ProcAudioNative.MotionRoll; speed = roll; }
                else { kind = ProcAudioNative.MotionSlide; speed = slide; }
                if (speed < minMotionSpeed) speed = 0f;

                var use = held ? sv.heldV : sv.free;
                var idle = held ? sv.free : sv.heldV;
                use.kind = kind;
                ProcAudioNative.pa_voice_motion(use.handle, kind, speed);
                ProcAudioNative.pa_voice_motion(idle.handle, ProcAudioNative.MotionNone, 0f);
                sv.lastTouch = now;
                sv.speed = speed;
                sv.kind = kind;
                if (!sv.listed) { sv.listed = true; moving.Add(sv); }
            }

            string status = "";
            for (int i = moving.Count - 1; i >= 0; --i)
            {
                var sv = moving[i];
                if (now - sv.lastTouch > contactGrace)
                {
                    ProcAudioNative.pa_voice_motion(sv.free.handle, ProcAudioNative.MotionNone, 0f);
                    ProcAudioNative.pa_voice_motion(sv.heldV.handle, ProcAudioNative.MotionNone, 0f);
                    sv.listed = false;
                    moving.RemoveAt(i);
                    continue;
                }
                string verb = sv.speed <= 0f ? "resting on"
                            : sv.kind == ProcAudioNative.MotionRoll ? $"rolling {sv.speed:0.00} m/s on"
                            : $"sliding {sv.speed:0.00} m/s on";
                status = status.Length == 0 ? $"{verb} {sv.name}" : $"{status}, {verb} {sv.name}";
            }
            Status = status.Length == 0 ? "in the air / on another object" : status;
        }

        // ------------------------------------------------------------ audio thread

        void OnAudioFilterRead(float[] data, int channels)
        {
            if (!ProcAudioEngine.AudioActive || !rendering)
            {
                Array.Clear(data, 0, data.Length);
                return;
            }
            int n = data.Length / channels;
            var m = mono;
            if (m.Length < n) mono = m = new float[n];
            Array.Clear(m, 0, n);
            var r = refs;
            for (int i = 0; i < r.Length; ++i)
                ProcAudioNative.pa_voice_process(r[i].handle, m, n, ProcAudioEngine.GainFor(r[i].kind));
            float g = ProcAudioEngine.MasterGain;
            for (int i = 0, k = 0; i < n; ++i)
            {
                float s = Mathf.Clamp(m[i] * g, -1f, 1f);
                for (int ch = 0; ch < channels; ++ch, ++k) data[k] *= s;
            }
        }

        // ------------------------------------------------------------ misc

        public string Describe() => $"{description.Pretty()} · {Body.mass:0.###} kg";

        public void ResetPose()
        {
            Body.linearVelocity = Vector3.zero;
            Body.angularVelocity = Vector3.zero;
            Body.position = startPos;
            Body.rotation = startRot;
            transform.SetPositionAndRotation(startPos, startRot);
        }

        void OnDestroy()
        {
            rendering = false;
            var r = refs;
            refs = Array.Empty<VoiceRef>();
            if (!ProcAudioEngine.AudioActive) return;   // engine teardown resets all voices
            foreach (var v in r) ProcAudioNative.pa_voice_release(v.handle);
        }
    }
}
