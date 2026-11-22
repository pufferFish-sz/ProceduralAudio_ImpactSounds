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
    /// of the load it should (normal force / (m g cos slope) >= minLoadFraction, for
    /// at least minSlideContactTime), and its volume follows that load. Brushing an
    /// object over a surface stays silent. A HELD object slides audibly only while
    /// the grabber presses it onto that very surface (SoundBody.pressedOn): picking
    /// it up, carrying it, or an end dragging along while it is lifted is silent.
    ///
    /// Free objects (tossed, dropped, tumbling) start a roll / slide only once they
    /// really slide: freeSlideMinTime of calm contact (no load spike, no bounce),
    /// not within settleAfterImpact of a hit on the pair, and not while a non-ball
    /// tumbles (spin > maxTumbleRate). A started one carries on through small bumps.
    /// Every roll / slide fades in over attackTime, so a short one stays quiet.
    ///
    /// Contacts: a body made of several colliders (a ramp's plank and rails, a
    /// table's top and legs) reports several contacts per step; the one carrying
    /// the most normal impulse is the contact (point, normal), and the loads add.
    ///
    /// Debounce: minimum impact speed, minimum time between impacts of a pair,
    /// contact chatter of a pair already touching (it must be a real re-hit, and at
    /// most one per touchingImpactInterval), a cap on new impacts per physics step
    /// and on sustained instances, and no restart of a pair's roll / slide more often
    /// than restartInterval. Interpenetrating contacts (an object wedged into
    /// geometry, pushed out by the solver every step) are not hits; a pair that keeps
    /// "hitting" while it never separates is jammed and stays quiet until it parts.
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
        [Tooltip("While a pair is already in contact, at most one re-hit per this time (s): rough or " +
                 "grippy floors make boxes judder, which re-reports the contact many times a second")]
        public float touchingImpactInterval = 0.25f;
        [Tooltip("A contact penetrating deeper than this (m) is the solver pushing a wedged object out, " +
                 "not a hit")]
        public float wedgeDepth = 0.01f;
        [Tooltip("More than this many impacts of one pair within jamWindow while it never separates: " +
                 "the pair is jammed (stuck in geometry) and stays silent until it parts")]
        public int jamImpacts = 4;
        public float jamWindow = 1f;

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
        [Tooltip("When a roll / slide goes silent (lifted, carried off, contact ended, too slow) it " +
                 "fades out over this time (s) at its last speed, instead of the speed dropping to 0 " +
                 "at once (an 80 ms cut). Touching again during the fade picks it up again.")]
        public float releaseTime = 0.4f;

        [Header("Load gate (lifting, brushing)")]
        [Tooltip("A roll / slide only sounds while the contact carries at least this fraction of the " +
                 "object's weight. Lifting an object keeps it touching for a few steps with almost no " +
                 "load (and its contact point can move fast as it tips): that made a loud whoosh.")]
        [Range(0f, 1f)] public float minLoadFraction = 0.3f;
        [Tooltip("...and only once the contact has lasted this long (s)")]
        public float minSlideContactTime = 0.05f;
        [Tooltip("A HELD object whose contact points move apart along the contact normal faster than " +
                 "this (m/s, smoothed like the load) while carrying less than the full load is being " +
                 "lifted off: silent, and the contact time starts over. A held object pressed down " +
                 "jitters by millimetres at full load: that is not a lift. (A free object that leaves " +
                 "a surface loses the contact; on a slope it never carries its full weight, so the " +
                 "test is not applied to it: it muted balls rolling down the ramp.)")]
        public float liftSpeed = 0.05f;
        [Tooltip("Smoothing of the measured load (s): the solver's per-step impulse jitters")]
        public float loadSmoothing = 0.03f;
        [Tooltip("Scale a roll / slide's volume by its load (capped at the resting weight): light, " +
                 "brushing contacts are quieter and a lift fades out instead of cutting")]
        public bool volumeFollowsLoad = true;

        [Header("Free objects (tossed, dropped, tumbling)")]
        [Tooltip("A free object's roll / slide starts only after this long (s) of calm contact: " +
                 "a bounce or a tumble touches for a few steps and must stay silent")]
        public float freeSlideMinTime = 0.2f;
        [Tooltip("...for a ball's roll (s): a ball that keeps touching is rolling")]
        public float freeRollMinTime = 0.1f;
        [Tooltip("Log once when a free object keeps moving on a surface this long (s) without a roll / " +
                 "slide sound, with the reason (0 = off)")]
        public float reportSilentAfter = 0.6f;
        [Tooltip("...and not within this time (s) after an impact on the same pair")]
        public float settleAfterImpact = 0.25f;
        [Tooltip("Moving along the contact normal faster than this (m/s, either way) is bouncing: " +
                 "the calm-contact clock starts over")]
        public float maxBounceSpeed = 0.15f;
        [Tooltip("A step whose contact carries more than this many times the expected load is a hit, " +
                 "not a slide: the calm-contact clock starts over")]
        public float loadSpike = 3f;
        [Tooltip("A free object other than a ball spinning faster than this (rad/s) is tumbling: " +
                 "it makes impacts, not a slide")]
        public float maxTumbleRate = 4f;
        [Tooltip("Every roll / slide fades in over this time (s): a short one cannot reach the level " +
                 "of an impact")]
        public float attackTime = 0.15f;

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
            public float best = -1f;         // strongest contact's impulse this step (picks point / normal)
            public float load;               // smoothed normal force / weight
            public float separating;         // smoothed speed apart along the normal (m/s)
            public float since = -1f;        // start of this continuous contact
            public float lastSeen = -1f;
            public float silentSince = -1f;  // moving but silent since (diagnostic)
            public bool reported;
        }

        class Sustained
        {
            public SoundBody owner, other;
            public EventInstance inst;
            public int motion;
            public bool held;
            public float lastTouch, started, volume = 1f;
            public float releaseStart = -1f;   // < 0: sounding; else fading out since then
            public float releaseGain = 1f;
            public float applied;              // volume last sent (load x attack)
        }

        struct WarmJob { public SoundBody obj, target; public int motion; public bool held; }

        readonly Dictionary<PairKey, Touch> touches = new Dictionary<PairKey, Touch>();
        readonly Dictionary<PairKey, Sustained> sustained = new Dictionary<PairKey, Sustained>();
        readonly Dictionary<PairKey, float> lastImpact = new Dictionary<PairKey, float>();
        readonly Dictionary<PairKey, float> lastContact = new Dictionary<PairKey, float>();
        readonly Dictionary<PairKey, float> lastStart = new Dictionary<PairKey, float>();
        readonly Dictionary<PairKey, (float start, int count)> impactBurst = new Dictionary<PairKey, (float, int)>();
        readonly HashSet<PairKey> jammed = new HashSet<PairKey>();
        readonly List<Sustained> fadingOut = new List<Sustained>();   // replaced instances, releasing
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
            // the contact carrying the most normal impulse is THE contact; averaging
            // points across colliders (plank + rail) put it inside the body
            // the SUPPORTING contact wins (impulse, weighted toward normals facing up): a
            // ball brushing the ramp's side rail must not become "the contact", or its
            // sideways normal reads as a load spike and a bounce and the roll never starts
            int best = 0;
            float bestImp = -1f, sumImp = 0f, minSep = float.MaxValue;
            Vector3 up = -Physics.gravity.normalized;
            for (int i = 0; i < n; ++i)
            {
                float imp = Mathf.Abs(Vector3.Dot(cbuf[i].impulse, cbuf[i].normal));
                sumImp += imp;
                float score = imp * (0.25f + Mathf.Max(0f, Vector3.Dot(cbuf[i].normal, up)));
                if (score > bestImp) { bestImp = score; best = i; }
                minSep = Mathf.Min(minSep, cbuf[i].separation);
            }
            Vector3 point = cbuf[best].point;
            Vector3 normal = cbuf[best].normal.sqrMagnitude > 1e-8f ? cbuf[best].normal.normalized : Vector3.up;

            var key = new PairKey(self, other);
            if (enter && minSep > -wedgeDepth)   // deeper: the solver pushing a wedged body out
                TryImpact(key, self, other, point, Mathf.Abs(Vector3.Dot(c.relativeVelocity, normal)));

            if (!touches.TryGetValue(key, out var t)) touches[key] = t = new Touch();
            t.owner = self;
            t.other = other;
            if (bestImp > t.best)
            {
                t.best = bestImp;
                t.point = point;
                t.normal = normal;
            }
            t.n += 1;
            t.impulse += sumImp;
        }

        void TryImpact(PairKey key, SoundBody owner, SoundBody other, Vector3 point, float vn)
        {
            if (vn < minImpactSpeed || impactsThisStep >= maxImpactsPerStep) return;
            float now = Time.fixedTime;
            bool hadImpact = lastImpact.TryGetValue(key, out float t);
            if (hadImpact && now - t < impactCooldown) return;
            // still touching a moment ago: contact chatter, not a new hit, unless it is
            // a real re-hit, and then at most one per touchingImpactInterval
            bool touching = lastContact.TryGetValue(key, out float c) && now - c <= contactGrace;
            if (touching)
            {
                if (vn < chatterImpactSpeed || jammed.Contains(key)) return;
                if (hadImpact && now - t < touchingImpactInterval) return;
                // a pair that keeps "hitting" without ever separating is stuck
                if (!impactBurst.TryGetValue(key, out var b) || now - b.start > jamWindow) b = (now, 0);
                b.count += 1;
                impactBurst[key] = b;
                if (b.count > jamImpacts)
                {
                    jammed.Add(key);
                    AddLog($"{owner.displayName} stuck against {other.displayName}: silent until it parts");
                    return;
                }
            }
            else impactBurst.Remove(key);
            lastImpact[key] = now;
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
                Vector3 p = t.point;
                Vector3 nrm = t.normal.sqrMagnitude > 1e-8f ? t.normal.normalized : Vector3.up;
                float impulse = t.impulse;
                t.point = t.normal = Vector3.zero;
                t.n = 0;
                t.impulse = 0f;
                t.best = -1f;
                if (t.owner == null || t.other == null) continue;

                // how much of the load it should carry the contact carries: normal force
                // over m g cos(slope) for a free body (a ball on the 12-degree ramp carries
                // 98 % of its weight: that is full load); held bodies have no gravity, the
                // hand's push is their load, so over m g
                float weight = t.owner.Body.mass * Physics.gravity.magnitude;
                float expect = t.owner.held ? 1f
                    : Mathf.Max(0.25f, Mathf.Abs(Vector3.Dot(nrm, Physics.gravity.normalized)));
                float load = weight > 0f ? impulse / Time.fixedDeltaTime / (weight * expect) : 1f;
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
                bool held = t.owner.held;
                bool lifting = held && t.separating > liftSpeed && t.load < 1f;
                if (lifting) t.since = now;
                // a held object sounds only where the hand presses it down
                bool carried = held && t.owner.pressedOn != t.other;
                bool stuck = jammed.Contains(kv.Key);
                bool pressed = !lifting && !carried && !stuck && t.load >= minLoadFraction &&
                               now - t.since >= minSlideContactTime;
                // free objects: a toss, a bounce or a tumble is not a slide
                bool tumbling = false, settling = false;
                if (!held)
                {
                    bool sounding = sustained.TryGetValue(kv.Key, out var cur) && cur.releaseStart < 0f;
                    tumbling = t.owner.description.shape != PaShape.Ball &&
                               ob.angularVelocity.magnitude > maxTumbleRate;
                    if (sounding)
                        pressed &= !tumbling;   // carries on through small bumps
                    else
                    {
                        // solver jitter along the normal grows with speed: a ball rolling
                        // down the ramp at 2 m/s is not bouncing at 0.15 m/s; a hit shows
                        // in the SMOOTHED load (a one-step solver spike does not)
                        float bounceLimit = maxBounceSpeed + 0.1f * relC.magnitude;
                        bool bouncing = Mathf.Abs(t.separating) > bounceLimit || t.load > loadSpike;
                        if (bouncing || tumbling) t.since = now;
                        settling = lastImpact.TryGetValue(kv.Key, out float li) && now - li < settleAfterImpact;
                        float minTime = t.owner.description.shape == PaShape.Ball ? freeRollMinTime : freeSlideMinTime;
                        pressed &= !settling && !tumbling && now - t.since >= minTime;
                    }
                }
                float slide = (rel - nrm * Vector3.Dot(rel, nrm)).magnitude;
                float roll = (relC - nrm * Vector3.Dot(relC, nrm)).magnitude;

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
                    : tumbling ? $"tumbling on {t.other.displayName} (silent)"
                    : settling ? $"settling after a hit on {t.other.displayName} (silent)"
                    : stuck ? $"stuck against {t.other.displayName} (silent)"
                    : carried ? $"carried, touching {t.other.displayName} (silent)"
                    : lifting ? $"lifting off {t.other.displayName} (silent)"
                    : !pressed && moving >= minMotionSpeed
                        ? $"brushing {t.other.displayName} (load {t.load * 100f:0}%, silent)"
                        : $"resting on {t.other.displayName}";
                t.other.lastTouchTime = Time.time;
                // diagnostic: a free object moving on a surface that stays silent
                if (speed > 0f || held || moving < 0.1f) { t.silentSince = -1f; t.reported = false; }
                else if (t.silentSince < 0f) t.silentSince = now;
                else if (!t.reported && reportSilentAfter > 0f && now - t.silentSince > reportSilentAfter)
                {
                    t.reported = true;
                    AddLog($"{t.owner.displayName}: {t.owner.status} at {moving:0.00} m/s");
                }
            }

            // contact ended: fade out; a finished fade stops the instance
            scratchKeys.Clear();
            foreach (var kv in sustained)
            {
                var s = kv.Value;
                if (now - s.lastTouch > contactGrace) BeginRelease(s, now);
                if (s.releaseStart >= 0f && !Fade(s, now)) scratchKeys.Add(kv.Key);
            }
            foreach (var k in scratchKeys)
            {
                Stop(sustained[k]);
                sustained.Remove(k);
            }
            for (int i = fadingOut.Count - 1; i >= 0; --i)
            {
                if (Fade(fadingOut[i], now)) continue;
                Stop(fadingOut[i]);
                fadingOut.RemoveAt(i);
            }

            // a jammed pair is free again once it has been apart for a moment
            scratchKeys.Clear();
            foreach (var k in jammed)
                if (!lastContact.TryGetValue(k, out float lc) || now - lc > 0.3f) scratchKeys.Add(k);
            foreach (var k in scratchKeys)
            {
                jammed.Remove(k);
                impactBurst.Remove(k);
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
                    // a changed one (let go, roll <-> slide) fades out beside its successor
                    if (changed && !ended)
                    {
                        BeginRelease(s, now);
                        fadingOut.Add(s);
                    }
                    else Stop(s);
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
                s.volume = volume;
                s.applied = volume * Attack(s, now);
                s.inst.setVolume(s.applied);
                s.inst.set3DAttributes(point.To3DAttributes());
                var res = s.inst.start();
                if (res != FMOD.RESULT.OK) AddLog($"{owner.displayName}: FMOD could not start its {(motion == ROLL ? "roll" : "slide")} ({res})");
                sustained[key] = s;
            }
            else if (speed <= 0f)
            {
                // silent now (lifted, carried, too slow): fade at the last speed
                BeginRelease(s, now);
                s.inst.set3DAttributes(point.To3DAttributes());
            }
            else
            {
                if (s.releaseStart >= 0f)   // touching again: pick it up
                {
                    s.releaseStart = -1f;
                    s.releaseGain = 1f;
                    s.applied = -1f;         // force the volume below
                }
                s.inst.setParameterByName(P_SPEED, Mathf.Min(speed, 10f));
                s.volume = volume;
                float eff = volume * Attack(s, now);
                if (Mathf.Abs(eff - s.applied) > 0.01f)   // FMOD ramps volume changes
                {
                    s.inst.setVolume(eff);
                    s.applied = eff;
                }
                s.inst.set3DAttributes(point.To3DAttributes());
            }
            s.lastTouch = now;
        }

        /// <summary>Fade-in gain of a roll / slide since its start (smoothstep over attackTime).</summary>
        float Attack(Sustained s, float now)
        {
            if (attackTime <= 0f) return 1f;
            float x = Mathf.Clamp01((now - s.started) / attackTime);
            return x * x * (3f - 2f * x);
        }

        static void BeginRelease(Sustained s, float now)
        {
            if (s.releaseStart < 0f) s.releaseStart = now;
        }

        /// <summary>Applies the release fade; false when it has finished.</summary>
        bool Fade(Sustained s, float now)
        {
            float x = releaseTime > 0f ? (now - s.releaseStart) / releaseTime : 1f;
            if (x >= 1f) return false;
            float g = (1f - x) * (1f - x);   // quadratic: quick at first, a soft tail
            if (Mathf.Abs(g - s.releaseGain) > 0.005f)
            {
                s.releaseGain = g;
                s.inst.setVolume(Mathf.Max(0f, s.applied) * g);
            }
            return true;
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
            foreach (var s in fadingOut) Stop(s);
            fadingOut.Clear();
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
