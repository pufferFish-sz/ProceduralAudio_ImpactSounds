using System;
using System.Collections.Generic;
using FMOD.Studio;
using FMODUnity;
using UnityEngine;

namespace ProcAudio
{
    /// <summary>
    /// Routes physics contacts to the objects' FMOD events (one per scene).
    ///
    /// One sound per contact: Unity reports a collision to both rigidbodies, so
    /// every contact pair has one OWNER, and only the owner's report is used:
    ///   - an object against a static surface: the object;
    ///   - two objects: the ball (it rolls on the other), else the held one,
    ///     else the lighter one, else the lower instance id.
    /// The owner's event plays the pair (Render = Both): its own body is baked
    /// into the event, the other body is sent through the Target parameters.
    ///
    ///   OnCollisionEnter  normal relative speed  -> one-shot instance, Motion = Impact
    ///   OnCollisionStay   tangential speed       -> one sustained instance per touching pair,
    ///                                               Motion = Roll (free balls) or Slide, Speed
    ///                                               updated every physics step, stopped when
    ///                                               the contact ends
    /// Load gate: a roll / slide sounds only while the contact carries a real part
    /// of the object's weight (Collision.impulse / dt / m g >= minLoadFraction, for
    /// at least minSlideContactTime), and its volume follows that load. Lifting
    /// or brushing an object over a surface stays silent.
    ///
    /// Debounce: minimum impact speed, minimum time between impacts of a pair,
    /// contact chatter of a pair already touching (it must be a real re-hit), a cap
    /// on new impacts per physics step and on sustained instances, and no restart
    /// of a pair's roll / slide more often than restartInterval.
    ///
    /// Warm-up: the plug-in builds rolling / sliding tables for a new pair in the
    /// background (~0.5 s each, then cached for the process). At start, silent
    /// Speed-0 instances of every object against every static surface build them,
    /// so the first slide does not start late.
    /// </summary>
    [DefaultExecutionOrder(-50)]
    public class ContactAudioManager : MonoBehaviour
    {
        public static ContactAudioManager Instance { get; private set; }
        public static readonly List<SoundBody> Bodies = new List<SoundBody>();

        [Header("Impacts")]
        [Tooltip("Normal speeds below this make no impact (m/s)")]
        public float minImpactSpeed = 0.06f;
        [Tooltip("Shortest time between two impacts of the same pair (s)")]
        public float impactCooldown = 0.04f;
        [Tooltip("At most this many new impacts per physics step")]
        public int maxImpactsPerStep = 6;
        [Tooltip("While a pair is already in contact (sliding, pressed down), contact chatter " +
                 "re-reports it as a new collision: only a re-hit at least this fast counts (m/s)")]
        public float chatterImpactSpeed = 0.35f;

        [Header("Rolling / sliding")]
        [Tooltip("Roll / slide speeds below this are silent (m/s)")]
        public float minMotionSpeed = 0.02f;
        [Tooltip("A contact may drop out this long (s) before its roll / slide stops")]
        public float contactGrace = 0.08f;
        [Tooltip("At most this many rolling / sliding instances at once")]
        public int maxSustained = 16;
        [Tooltip("A moving target slower than this counts as Resting (it can carry a slide)")]
        public float restingTargetSpeed = 0.3f;
        [Tooltip("A new roll / slide instance is not judged finished before this age (s): " +
                 "FMOD reports it STOPPED until its first update")]
        public float minInstanceAge = 0.3f;
        [Tooltip("Shortest time between two restarts of the same pair's roll / slide (s)")]
        public float restartInterval = 0.25f;

        [Header("Load gate (lifting, brushing)")]
        [Tooltip("A roll / slide only sounds while the contact carries at least this fraction of the " +
                 "object's weight. Lifting an object keeps it touching for a few steps with almost no " +
                 "load (and its contact point can move fast as it tips): that made a loud whoosh.")]
        [Range(0f, 1f)] public float minLoadFraction = 0.3f;
        [Tooltip("...and only once the contact has lasted this long (s)")]
        public float minSlideContactTime = 0.05f;
        [Tooltip("Contact points moving apart along the contact normal faster than this (m/s, " +
                 "smoothed like the load) while carrying less than the full weight are being lifted " +
                 "off: silent, and the contact time starts over. A held object pressed down jitters " +
                 "by millimetres at full load: that is not a lift.")]
        public float liftSpeed = 0.05f;
        [Tooltip("Smoothing of the measured load (s): the solver's per-step impulse jitters")]
        public float loadSmoothing = 0.03f;
        [Tooltip("Scale a roll / slide's volume by its load (capped at the resting weight): light, " +
                 "brushing contacts are quieter and a lift fades out instead of cutting")]
        public bool volumeFollowsLoad = true;

        [Header("Warm-up")]
        public bool warmUp = true;
        public int warmUpPerFrame = 2;
        [Tooltip("At most this many silent warm-up instances at once (each runs a plug-in voice)")]
        public int warmUpConcurrent = 6;
        [Tooltip("How long each silent warm-up instance lives (s)")]
        public float warmUpHold = 1.5f;

        [Header("Physics (applied at start)")]
        public float fixedTimestep = 0.01f;
        [Tooltip("Unity kills bounces below this relative speed (default 2 m/s)")]
        public float bounceThreshold = 0.4f;

        // FMOD parameter names (presets "ProcAudio Contact" in the FMOD project)
        const string P_MOTION = "Motion", P_SPEED = "Speed", P_HELD = "Held";
        const string P_SHAPE = "Target Shape", P_MATERIAL = "Target Material", P_FINISH = "Target Finish",
                     P_MOUNT = "Target Mount", P_SIZE = "Target Size", P_THICK = "Target Thickness";
        const int IMPACT = 0, ROLL = 1, SLIDE = 2;

        readonly struct PairKey : IEquatable<PairKey>
        {
            readonly int a, b;
            public PairKey(SoundBody x, SoundBody y)
            {
                int i = x.GetInstanceID(), j = y.GetInstanceID();
                a = Mathf.Min(i, j);
                b = Mathf.Max(i, j);
            }
            public bool Equals(PairKey o) => a == o.a && b == o.b;
            public override bool Equals(object o) => o is PairKey k && Equals(k);
            public override int GetHashCode() => a * 486187739 + b;
        }

        class Touch
        {
            public SoundBody owner, other;
            public Vector3 point, normal;
            public int n;
            public float impulse;            // normal impulse this step (N s)
            public float load;               // smoothed normal force / weight
            public float separating;         // smoothed speed apart along the normal (m/s)
            public float since = -1f;        // start of this continuous contact
            public float lastSeen = -1f;
        }

        class Sustained
        {
            public SoundBody owner, other;
            public EventInstance inst;
            public int motion;
            public bool held;
            public float lastTouch, started, volume = 1f;
        }

        struct WarmJob { public SoundBody obj, target; public int motion; public bool held; }

        readonly Dictionary<PairKey, Touch> touches = new Dictionary<PairKey, Touch>();
        readonly Dictionary<PairKey, Sustained> sustained = new Dictionary<PairKey, Sustained>();
        readonly Dictionary<PairKey, float> lastImpact = new Dictionary<PairKey, float>();
        readonly Dictionary<PairKey, float> lastContact = new Dictionary<PairKey, float>();
        readonly Dictionary<PairKey, float> lastStart = new Dictionary<PairKey, float>();
        readonly List<PairKey> scratchKeys = new List<PairKey>();
        readonly ContactPoint[] cbuf = new ContactPoint[32];
        readonly Queue<WarmJob> warmQueue = new Queue<WarmJob>();
        readonly List<(EventInstance inst, float until)> warming = new List<(EventInstance, float)>();
        int impactsThisStep;

        readonly List<string> log = new List<string>();
        public IReadOnlyList<string> Log => log;
        public event Action LogChanged;

        public int WarmUpRemaining => warmQueue.Count + warming.Count;
        public int SustainedCount => sustained.Count;
        /// <summary>Event instances created so far (impacts + rolls / slides), for the HUD.</summary>
        public int InstancesCreated { get; private set; }
        public int ImpactInstances { get; private set; }
        public int MotionInstances { get; private set; }

        void Awake()
        {
            if (Instance != null && Instance != this)
            {
                Destroy(gameObject);
                return;
            }
            Instance = this;
            Time.fixedDeltaTime = fixedTimestep;
            Physics.bounceThreshold = bounceThreshold;
        }

        void Start()
        {
            if (!warmUp) return;
            foreach (var s in Bodies)
                if (!s.IsDynamic) WarmUp(s);
        }

        /// <summary>Builds every object's roll / slide tables against this surface.</summary>
        public void WarmUp(SoundBody surface)
        {
            foreach (var o in Bodies)
            {
                if (!o.IsDynamic || !o.HasEvent || o == surface) continue;
                bool ball = o.description.shape == PaShape.Ball;
                warmQueue.Enqueue(new WarmJob { obj = o, target = surface, motion = ball ? ROLL : SLIDE, held = false });
                warmQueue.Enqueue(new WarmJob { obj = o, target = surface, motion = SLIDE, held = true });
            }
        }

        // ------------------------------------------------------------ ownership

        static bool Sounds(SoundBody b) => b.HasEvent && b.IsDynamic;

        static SoundBody Owner(SoundBody a, SoundBody b)
        {
            bool sa = Sounds(a), sb = Sounds(b);
            if (!sa && !sb) return null;
            if (sa != sb) return sa ? a : b;
            bool ba = a.description.shape == PaShape.Ball, bb = b.description.shape == PaShape.Ball;
            if (ba != bb) return ba ? a : b;
            if (a.held != b.held) return a.held ? a : b;
            float ma = a.Body.mass, mb = b.Body.mass;
            if (!Mathf.Approximately(ma, mb)) return ma < mb ? a : b;
            return a.GetInstanceID() < b.GetInstanceID() ? a : b;
        }

        // ------------------------------------------------------------ reports

        /// <summary>Called by SoundBody from OnCollisionEnter / OnCollisionStay.</summary>
        public void Report(SoundBody self, Collision c, bool enter)
        {
            var other = c.collider != null ? c.collider.GetComponentInParent<SoundBody>() : null;
            if (other == null || other == self) return;
            self.lastTouchTime = Time.time;
            if (Owner(self, other) != self) return;   // the owner's report is the one we use
            int n = c.GetContacts(cbuf);
            if (n == 0) return;
            Vector3 point = Vector3.zero, normal = Vector3.zero;
            for (int i = 0; i < n; ++i)
            {
                point += cbuf[i].point;
                normal += cbuf[i].normal;
            }
            point /= n;
            normal = normal.sqrMagnitude > 1e-8f ? normal.normalized : Vector3.up;

            var key = new PairKey(self, other);
            if (enter) TryImpact(key, self, other, point, Mathf.Abs(Vector3.Dot(c.relativeVelocity, normal)));

            if (!touches.TryGetValue(key, out var t)) touches[key] = t = new Touch();
            t.owner = self;
            t.other = other;
            t.point += point;
            t.normal += normal;
            t.n += 1;
            t.impulse += Mathf.Abs(Vector3.Dot(c.impulse, normal));
        }

        void TryImpact(PairKey key, SoundBody owner, SoundBody other, Vector3 point, float vn)
        {
            if (vn < minImpactSpeed || impactsThisStep >= maxImpactsPerStep) return;
            if (lastImpact.TryGetValue(key, out float t) && Time.fixedTime - t < impactCooldown) return;
            // still touching a moment ago: contact chatter, not a new hit
            if (lastContact.TryGetValue(key, out float c) && Time.fixedTime - c <= contactGrace &&
                vn < chatterImpactSpeed) return;
            lastImpact[key] = Time.fixedTime;
            ++impactsThisStep;

            var inst = RuntimeManager.CreateInstance(owner.soundEvent);
            ++InstancesCreated;
            ++ImpactInstances;
            SetTarget(inst, owner, other);
            inst.setParameterByName(P_MOTION, IMPACT);
            inst.setParameterByName(P_SPEED, Mathf.Min(vn, 10f));
            inst.set3DAttributes(point.To3DAttributes());
            inst.start();
            inst.release();

            owner.lastImpact = $"hit {other.displayName} at {vn:0.00} m/s";
            if (other.IsDynamic) other.lastImpact = $"hit by {owner.displayName} at {vn:0.00} m/s";
            if (vn > 0.4f) AddLog($"{owner.displayName} → {other.displayName}: impact {vn:0.0} m/s");
        }

        /// <summary>Describes `other` to the owner's event through the Target parameters.</summary>
        void SetTarget(EventInstance inst, SoundBody owner, SoundBody other)
        {
            var d = other.description;
            var mount = d.mount;
            if (other.IsDynamic)
                mount = other.held ? PaMount.Held
                      : other.Body.linearVelocity.magnitude < restingTargetSpeed ? PaMount.Resting : PaMount.Free;
            if (d.shape == PaShape.Ground) mount = PaMount.BuiltIn;
            inst.setParameterByName(P_SHAPE, (int)d.shape);
            inst.setParameterByName(P_MATERIAL, (int)d.material);
            inst.setParameterByName(P_FINISH, (int)d.finish);
            inst.setParameterByName(P_MOUNT, (int)mount);
            inst.setParameterByName(P_SIZE, Mathf.Clamp(d.size, 0.005f, 20f));
            inst.setParameterByName(P_THICK, Mathf.Clamp(d.thickness, 0f, 0.3f));
            inst.setParameterByName(P_HELD, owner.held ? 1 : 0);
        }

        // ------------------------------------------------------------ roll / slide

        // Contacts gathered during the last physics step (collision callbacks run
        // after the step, before the next FixedUpdate) become roll / slide speeds.
        void FixedUpdate()
        {
            impactsThisStep = 0;
            float now = Time.fixedTime;
            foreach (var kv in touches)
            {
                var t = kv.Value;
                if (t.n == 0) continue;
                Vector3 p = t.point / t.n;
                Vector3 nrm = t.normal.sqrMagnitude > 1e-8f ? t.normal.normalized : Vector3.up;
                float impulse = t.impulse;
                t.point = t.normal = Vector3.zero;
                t.n = 0;
                t.impulse = 0f;
                if (t.owner == null || t.other == null) continue;

                // how much of its weight the contact carries (normal force / m g)
                float weight = t.owner.Body.mass * Physics.gravity.magnitude;
                float load = weight > 0f ? impulse / Time.fixedDeltaTime / weight : 1f;
                var ob = t.owner.Body;
                bool otherMoves = t.other.IsDynamic;
                Vector3 rel = ob.GetPointVelocity(p) - (otherMoves ? t.other.Body.GetPointVelocity(p) : Vector3.zero);
                Vector3 relC = ob.linearVelocity - (otherMoves ? t.other.Body.linearVelocity : Vector3.zero);
                // the contact normal points from the other body toward the owner: > 0 = moving apart
                float sep = Vector3.Dot(rel, nrm);
                if (now - t.lastSeen > contactGrace)   // a new contact
                {
                    t.since = now;
                    t.load = load;
                    t.separating = sep;
                }
                else
                {
                    float a = 1f - Mathf.Exp(-Time.fixedDeltaTime / Mathf.Max(1e-4f, loadSmoothing));
                    t.load += a * (load - t.load);
                    t.separating += a * (sep - t.separating);
                }
                t.lastSeen = now;
                bool lifting = t.separating > liftSpeed && t.load < 1f;
                if (lifting) t.since = now;
                bool pressed = !lifting && t.load >= minLoadFraction && now - t.since >= minSlideContactTime;
                float slide = (rel - nrm * Vector3.Dot(rel, nrm)).magnitude;
                float roll = (relC - nrm * Vector3.Dot(relC, nrm)).magnitude;

                bool held = t.owner.held;
                int motion;
                float speed;
                if (held) { motion = SLIDE; speed = slide; }
                else if (t.owner.description.shape == PaShape.Ball) { motion = ROLL; speed = roll; }
                else { motion = SLIDE; speed = slide; }
                float moving = speed;
                if (speed < minMotionSpeed || !pressed) speed = 0f;
                float volume = volumeFollowsLoad ? Mathf.Clamp01(t.load) : 1f;

                lastContact[kv.Key] = now;
                UpdateSustained(kv.Key, t.owner, t.other, motion, held, speed, volume, p, now);
                t.owner.status = speed > 0f
                    ? $"{(motion == ROLL ? "rolling" : "sliding")} {speed:0.00} m/s on {t.other.displayName}, load {t.load * 100f:0}%"
                    : lifting ? $"lifting off {t.other.displayName} (silent)"
                    : !pressed && moving >= minMotionSpeed
                        ? $"brushing {t.other.displayName} (load {t.load * 100f:0}%, silent)"
                        : $"resting on {t.other.displayName}";
                t.other.lastTouchTime = Time.time;
            }

            scratchKeys.Clear();
            foreach (var kv in sustained)
                if (now - kv.Value.lastTouch > contactGrace) scratchKeys.Add(kv.Key);
            foreach (var k in scratchKeys)
            {
                Stop(sustained[k]);
                sustained.Remove(k);
            }
        }

        void UpdateSustained(PairKey key, SoundBody owner, SoundBody other, int motion, bool held, float speed,
                             float volume, Vector3 point, float now)
        {
            sustained.TryGetValue(key, out var s);
            if (s != null)
            {
                bool changed = s.motion != motion || s.held != held || s.owner != owner;
                // the plug-in ends an instance after 100 ms of silence (resting contact);
                // a fresh one reads STOPPED until FMOD's first update, so give it time
                bool ended = now - s.started > minInstanceAge && !Playing(s.inst);
                if (changed || ended)
                {
                    Stop(s);
                    sustained.Remove(key);
                    s = null;
                }
            }
            if (s == null)
            {
                if (speed <= 0f || sustained.Count >= maxSustained) return;
                if (lastStart.TryGetValue(key, out float ls) && now - ls < restartInterval) return;
                lastStart[key] = now;
                ++InstancesCreated;
                ++MotionInstances;
                s = new Sustained
                {
                    owner = owner, other = other, motion = motion, held = held, started = now,
                    inst = RuntimeManager.CreateInstance(owner.soundEvent),
                };
                SetTarget(s.inst, owner, other);
                s.inst.setParameterByName(P_MOTION, motion);
                s.inst.setParameterByName(P_SPEED, speed);
                s.inst.setVolume(volume);
                s.volume = volume;
                s.inst.set3DAttributes(point.To3DAttributes());
                s.inst.start();
                sustained[key] = s;
            }
            else
            {
                s.inst.setParameterByName(P_SPEED, Mathf.Min(speed, 10f));
                if (Mathf.Abs(volume - s.volume) > 0.01f)   // FMOD ramps volume changes
                {
                    s.inst.setVolume(volume);
                    s.volume = volume;
                }
                s.inst.set3DAttributes(point.To3DAttributes());
            }
            s.lastTouch = now;
        }

        static bool Playing(EventInstance inst)
        {
            if (!inst.isValid()) return false;
            inst.getPlaybackState(out var st);
            return st != PLAYBACK_STATE.STOPPED;
        }

        static void Stop(Sustained s)
        {
            if (!s.inst.isValid()) return;
            s.inst.setParameterByName(P_SPEED, 0f);
            s.inst.stop(FMOD.Studio.STOP_MODE.ALLOWFADEOUT);
            s.inst.release();
        }

        // ------------------------------------------------------------ warm-up, log

        void Update()
        {
            float now = Time.unscaledTime;
            for (int i = warming.Count - 1; i >= 0; --i)
            {
                if (now < warming[i].until) continue;
                warming[i].inst.stop(FMOD.Studio.STOP_MODE.IMMEDIATE);
                warming[i].inst.release();
                warming.RemoveAt(i);
            }
            for (int k = 0; k < warmUpPerFrame && warmQueue.Count > 0 && warming.Count < warmUpConcurrent; ++k)
            {
                var j = warmQueue.Dequeue();
                if (j.obj == null || j.target == null) continue;
                bool wasHeld = j.obj.held;
                j.obj.held = j.held;
                var inst = RuntimeManager.CreateInstance(j.obj.soundEvent);
                SetTarget(inst, j.obj, j.target);
                j.obj.held = wasHeld;
                inst.setParameterByName(P_MOTION, j.motion);
                inst.setParameterByName(P_SPEED, 0f);   // silent: only builds the tables
                inst.set3DAttributes(j.obj.transform.position.To3DAttributes());
                inst.start();
                warming.Add((inst, now + warmUpHold));
            }
        }

        public void AddLog(string line)
        {
            log.Insert(0, $"{Time.time,6:0.0}s  {line}");
            if (log.Count > 7) log.RemoveAt(log.Count - 1);
            LogChanged?.Invoke();
        }

        void OnDestroy()
        {
            if (Instance != this) return;
            foreach (var s in sustained.Values) Stop(s);
            sustained.Clear();
            foreach (var w in warming)
            {
                w.inst.stop(FMOD.Studio.STOP_MODE.IMMEDIATE);
                w.inst.release();
            }
            warming.Clear();
            Instance = null;
        }
    }
}
