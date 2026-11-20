using System.Collections.Generic;
using System.Text;
using FMODUnity;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.InputSystem;
using UnityEngine.UI;

namespace ProcAudio
{
    /// <summary>
    /// The demo's overlay: floor-surface dropdown, FMOD master volume, what the
    /// object under the crosshair is doing, and an event log.
    ///   Tab / Esc  free the cursor for the panel (click the scene to look again)
    ///   1..9       floor surface          R  reset objects
    /// Levels per motion (Impact 0 dB, Roll +35, Slide +50) live in the FMOD
    /// events (Level automated by the Motion parameter).
    /// </summary>
    public class DemoHUD : MonoBehaviour
    {
        public SurfaceSelector floor;
        public ObjectGrabber grabber;
        public Dropdown surfaceDropdown;
        public Slider masterSlider;
        public Text masterLabel;
        public Text statusText, logText, hintText;
        public GameObject panel;
        public Graphic crosshair;
        [Tooltip("FMOD bus the master slider drives")]
        public string masterBus = "bus:/";

        readonly StringBuilder sb = new StringBuilder();

        void Start()
        {
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
            if (masterSlider != null)
            {
                masterSlider.minValue = -40f;
                masterSlider.maxValue = 12f;
                masterSlider.wholeNumbers = true;
                masterSlider.SetValueWithoutNotify(0f);
                ShowMaster(0f);
                masterSlider.onValueChanged.AddListener(v =>
                {
                    try { RuntimeManager.GetBus(masterBus).setVolume(Mathf.Pow(10f, v / 20f)); }
                    catch (System.Exception e) { Debug.LogWarning("[ProcAudio] " + e.Message); }
                    ShowMaster(v);
                });
            }
            var mgr = ContactAudioManager.Instance;
            if (mgr != null) mgr.LogChanged += RefreshLog;
            SetLook(true);
            RefreshLog();
        }

        void ShowMaster(float v)
        {
            if (masterLabel != null) masterLabel.text = $"FMOD master  {v:+0;-0;0} dB";
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
                    foreach (var b in ContactAudioManager.Bodies) b.ResetPose();
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
            var mgr = ContactAudioManager.Instance;
            sb.Clear();
            if (mgr == null)
                sb.Append("<color=#ff8080>No ContactAudioManager in the scene</color>\n");
            else if (!RuntimeManager.HaveAllBanksLoaded)
                sb.Append("<color=#ffd060>Loading FMOD banks…</color>\n");
            else if (mgr.WarmUpRemaining > 0)
                sb.Append("<color=#ffd060>Warming up roll / slide tables… ").Append(mgr.WarmUpRemaining).Append(" left</color>\n");
            if (floor != null && floor.options.Count > 0)
                sb.Append("Floor: <b>").Append(floor.options[floor.current].name).Append("</b>");
            if (mgr != null) sb.Append("   ·   rolling/sliding: ").Append(mgr.SustainedCount)
                               .Append("   ·   instances made: ").Append(mgr.InstancesCreated);
            sb.Append('\n');

            var b = grabber != null ? (grabber.Held != null ? grabber.Held : grabber.Hover) : null;
            if (b != null)
            {
                sb.Append(grabber.Held != null ? "Holding: " : "Looking at: ")
                  .Append("<b>").Append(b.displayName).Append("</b>  <i>").Append(b.Describe()).Append("</i>\n")
                  .Append(b.Status());
                if (b.lastImpact.Length > 0) sb.Append("\nlast: ").Append(b.lastImpact);
            }
            statusText.text = sb.ToString();
        }

        void RefreshLog()
        {
            var mgr = ContactAudioManager.Instance;
            if (logText == null || mgr == null) return;
            logText.text = string.Join("\n", mgr.Log);
        }
    }
}
