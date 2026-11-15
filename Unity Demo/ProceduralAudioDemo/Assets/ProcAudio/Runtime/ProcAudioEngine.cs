using System;
using System.Collections.Generic;
using UnityEngine;

namespace ProcAudio
{
    /// <summary>
    /// Starts the native engine at the output sample rate, owns the levels that
    /// every ProcAudioBody renders with, and keeps a short event log for the HUD.
    /// One per scene.
    /// </summary>
    [DefaultExecutionOrder(-100)]
    public class ProcAudioEngine : MonoBehaviour
    {
        public static ProcAudioEngine Instance { get; private set; }

        /// <summary>False until the native engine started, and again once it is being torn down.
        /// Read on the audio thread.</summary>
        public static volatile bool AudioActive;

        [Header("Levels, dB (0 dB: 63 Pa at 1 m = full scale, as in the FMOD plug-in)")]
        [Tooltip("Impacts (the FMOD plug-in's Impact/Drop preset is 0 dB)")]
        [Range(-30f, 40f)] public float impactLevelDb = 0f;
        [Tooltip("Rolling is physically much quieter than impacts (FMOD Roll preset: +35 dB)")]
        [Range(0f, 70f)] public float rollLevelDb = 35f;
        [Tooltip("Sliding (FMOD Slide preset: +50 dB)")]
        [Range(0f, 80f)] public float slideLevelDb = 50f;
        [Range(-40f, 12f)] public float masterDb = 0f;

        [Header("Engine")]
        [Tooltip("Threads that build voices (cold roll/slide tables take ~0.5 s each)")]
        public int workerThreads = 3;

        [Header("Physics (applied at start)")]
        public float fixedTimestep = 0.01f;
        [Tooltip("Unity kills bounces below this relative speed (default 2 m/s)")]
        public float bounceThreshold = 0.4f;

        // audio-thread copies of the levels, as linear gains
        public static volatile float ImpactGain, RollGain, SlideGain, MasterGain = 1f;

        public bool Available { get; private set; }
        public int SampleRate { get; private set; }
        public string Error { get; private set; } = "";

        public static readonly List<ProcAudioBody> Bodies = new List<ProcAudioBody>();
        public static readonly List<ProcAudioSurface> Surfaces = new List<ProcAudioSurface>();

        readonly List<string> log = new List<string>();
        public IReadOnlyList<string> Log => log;
        public event Action LogChanged;

        const float FullScalePa = 63f;

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
            UpdateGains();

            SampleRate = AudioSettings.outputSampleRate;
            try
            {
                ProcAudioNative.pa_init(SampleRate, workerThreads);
                Available = true;
                AudioActive = true;
            }
            catch (Exception e)   // DllNotFoundException, EntryPointNotFoundException
            {
                Available = false;
                Error = "procaudio_unity.dll not loaded: " + e.Message;
                Debug.LogError("[ProcAudio] " + Error +
                               "\nBuild it: cmake --build cpp/build --config Release --target procaudio_unity");
            }
        }

        void Update() => UpdateGains();

        void UpdateGains()
        {
            ImpactGain = Db(impactLevelDb) / FullScalePa;
            RollGain = Db(rollLevelDb) / FullScalePa;
            SlideGain = Db(slideLevelDb) / FullScalePa;
            MasterGain = Db(masterDb);
        }

        static float Db(float db) => Mathf.Pow(10f, db / 20f);

        public static float GainFor(int kind)
        {
            switch (kind)
            {
                case ProcAudioNative.MotionRoll: return RollGain;
                case ProcAudioNative.MotionSlide: return SlideGain;
                default: return ImpactGain;
            }
        }

        public int PendingBuilds => Available ? ProcAudioNative.pa_pending_builds() : 0;

        /// <summary>Creates a voice (built asynchronously). 0 if the engine is unavailable.</summary>
        public int CreateVoice(BodyDescription obj, BodyDescription target, bool motion)
        {
            if (!Available) return 0;
            var o = obj.ToNative();
            var t = target.ToNative();
            return ProcAudioNative.pa_voice_create(ref o, ref t, motion ? 1 : 0);
        }

        public static float BodyMass(BodyDescription d)
        {
            try
            {
                var n = d.ToNative();
                return ProcAudioNative.pa_body_mass(ref n);
            }
            catch (Exception) { return 0f; }
        }

        /// <summary>A surface's description changed: every body gets its voices ready.</summary>
        public void NotifySurfaceChanged(ProcAudioSurface s)
        {
            foreach (var b in Bodies) b.Prewarm(s);
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
            AudioActive = false;
            Instance = null;
            // The Editor keeps the DLL loaded between Play sessions: drop the voices
            // but keep the engine (and its table cache) for the next session.
            if (Available) ProcAudioNative.pa_reset();
        }
    }
}
