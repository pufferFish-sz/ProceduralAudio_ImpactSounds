using System.Collections.Generic;
using System.Text;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.InputSystem;
using UnityEngine.UI;

namespace ProcAudio
{
    /// <summary>
    /// The demo's overlay: floor-surface dropdown, level sliders, what the object
    /// under the crosshair is doing, and an event log.
    ///   Tab / Esc  free the cursor for the panel (click the scene to look again)
    ///   1..9       floor surface          R  reset objects
    /// </summary>
    public class DemoHUD : MonoBehaviour
    {
        public SurfaceSelector floor;
        public ObjectGrabber grabber;
        public Dropdown surfaceDropdown;
        public Slider impactSlider, rollSlider, slideSlider, masterSlider;
        public Text impactLabel, rollLabel, slideLabel, masterLabel;
        public Text statusText, logText, hintText;
        public GameObject panel;
        public Graphic crosshair;

        readonly StringBuilder sb = new StringBuilder();

        void Start()
        {
            var engine = ProcAudioEngine.Instance;
            if (surfaceDropdown != null && floor != null)
            {
                surfaceDropdown.ClearOptions();
                var names = new List<string>();
                foreach (var o in floor.options) names.Add(o.name);
                surfaceDropdown.AddOptions(names);
                surfaceDropdown.SetValueWithoutNotify(floor.current);
                surfaceDropdown.onValueChanged.AddListener(i => floor.Select(i));
                floor.Changed += i => surfaceDropdown.SetValueWithoutNotify(i);
            }
            if (engine != null)
            {
                Bind(impactSlider, impactLabel, "Impact", -30f, 40f, engine.impactLevelDb, v => engine.impactLevelDb = v);
                Bind(rollSlider, rollLabel, "Roll", 0f, 70f, engine.rollLevelDb, v => engine.rollLevelDb = v);
                Bind(slideSlider, slideLabel, "Slide", 0f, 80f, engine.slideLevelDb, v => engine.slideLevelDb = v);
                Bind(masterSlider, masterLabel, "Master", -40f, 12f, engine.masterDb, v => engine.masterDb = v);
                engine.LogChanged += RefreshLog;
            }
            SetLook(true);
            RefreshLog();
        }

        static void Bind(Slider s, Text label, string name, float min, float max, float value, System.Action<float> set)
        {
            if (s == null) return;
            s.minValue = min;
            s.maxValue = max;
            s.wholeNumbers = true;
            s.SetValueWithoutNotify(value);
            void Show(float v) { if (label != null) label.text = $"{name}  {v:+0;-0;0} dB"; }
            Show(value);
            s.onValueChanged.AddListener(v => { set(v); Show(v); });
        }

        void SetLook(bool look)
        {
            Cursor.lockState = look ? CursorLockMode.Locked : CursorLockMode.None;
            Cursor.visible = !look;
            if (crosshair != null) crosshair.enabled = look;
        }

        void Update()
        {
            var kb = Keyboard.current;
            var mouse = Mouse.current;
            if (kb != null)
            {
                if (kb.tabKey.wasPressedThisFrame || kb.escapeKey.wasPressedThisFrame)
                    SetLook(!FirstPersonController.LookEnabled);
                if (floor != null)
                {
                    for (int i = 0; i < Mathf.Min(9, floor.options.Count); ++i)
                        if (kb[Key.Digit1 + i].wasPressedThisFrame) floor.Select(i);
                }
                if (kb.rKey.wasPressedThisFrame)
                {
                    if (grabber != null) grabber.Release(Vector3.zero);
                    foreach (var b in ProcAudioEngine.Bodies) b.ResetPose();
                }
            }
            // back to mouse look on a click into the scene (not on the panel)
            if (!FirstPersonController.LookEnabled && mouse != null && mouse.leftButton.wasPressedThisFrame &&
                (EventSystem.current == null || !EventSystem.current.IsPointerOverGameObject()))
                SetLook(true);

            UpdateStatus();
        }

        void UpdateStatus()
        {
            if (statusText == null) return;
            var engine = ProcAudioEngine.Instance;
            sb.Clear();
            if (engine == null || !engine.Available)
            {
                sb.Append("<color=#ff8080>Audio engine not available: ")
                  .Append(engine != null ? engine.Error : "no ProcAudioEngine in the scene").Append("</color>\n");
            }
            else
            {
                int pending = engine.PendingBuilds;
                if (pending > 0) sb.Append("<color=#ffd060>Building voices… ").Append(pending).Append(" left</color>\n");
            }
            if (floor != null && floor.options.Count > 0)
                sb.Append("Floor: <b>").Append(floor.options[floor.current].name).Append("</b>\n");

            var b = grabber != null ? (grabber.Held != null ? grabber.Held : grabber.Hover) : null;
            if (b != null)
            {
                sb.Append(grabber.Held != null ? "Holding: " : "Looking at: ")
                  .Append("<b>").Append(b.displayName).Append("</b>  <i>").Append(b.Describe()).Append("</i>\n")
                  .Append(b.Status);
                if (b.LastImpact.Length > 0) sb.Append("\nlast: ").Append(b.LastImpact);
            }
            statusText.text = sb.ToString();
        }

        void RefreshLog()
        {
            if (logText == null || ProcAudioEngine.Instance == null) return;
            logText.text = string.Join("\n", ProcAudioEngine.Instance.Log);
        }
    }
}
