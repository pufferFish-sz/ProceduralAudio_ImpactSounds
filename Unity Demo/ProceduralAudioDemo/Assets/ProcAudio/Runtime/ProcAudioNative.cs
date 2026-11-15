using System;
using System.Runtime.InteropServices;

namespace ProcAudio
{
    /// <summary>
    /// P/Invoke bindings for procaudio_unity.dll (cpp/plugins/unity/procaudio_unity.cpp).
    /// Rebuild the DLL with CMake (target procaudio_unity) with the Editor closed:
    /// the Editor keeps a loaded native plug-in locked.
    /// </summary>
    public static class ProcAudioNative
    {
        const string Dll = "procaudio_unity";

        [StructLayout(LayoutKind.Sequential)]
        public struct PaDesc
        {
            public int shape;
            public int material;
            public float size;
            public float thickness;
            public int mount;
            public float hit;
            public int finish;
        }

        public const int MotionNone = 0, MotionRoll = 1, MotionSlide = 2;

        [DllImport(Dll)] public static extern int pa_version();
        [DllImport(Dll)] public static extern int pa_init(int sampleRate, int workers);
        [DllImport(Dll)] public static extern void pa_reset();
        [DllImport(Dll)] public static extern void pa_shutdown();
        [DllImport(Dll)] public static extern int pa_sample_rate();
        [DllImport(Dll)] public static extern int pa_pending_builds();
        [DllImport(Dll)] public static extern int pa_last_error(byte[] buf, int cap);

        [DllImport(Dll)] public static extern float pa_body_mass(ref PaDesc d);
        [DllImport(Dll)] public static extern float pa_body_f0(ref PaDesc d);
        [DllImport(Dll)] public static extern float pa_default_thickness(ref PaDesc d);

        [DllImport(Dll)] public static extern int pa_voice_create(ref PaDesc obj, ref PaDesc tgt, int motion);
        [DllImport(Dll)] public static extern int pa_voice_state(int h);
        [DllImport(Dll)] public static extern void pa_voice_impact(int h, float v);
        [DllImport(Dll)] public static extern void pa_voice_motion(int h, int motion, float speed);
        [DllImport(Dll)] public static extern int pa_voice_process(int h, [In, Out] float[] output, int n, float gain);
        [DllImport(Dll)] public static extern void pa_voice_release(int h);

        public static string LastError()
        {
            var buf = new byte[512];
            int n = pa_last_error(buf, buf.Length);
            return System.Text.Encoding.UTF8.GetString(buf, 0, Math.Max(0, n));
        }
    }
}
