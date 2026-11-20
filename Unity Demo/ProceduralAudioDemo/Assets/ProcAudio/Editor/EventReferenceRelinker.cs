using FMODUnity;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace ProcAudio.EditorTools
{
    /// <summary>
    /// ProcAudio ▸ Re-link Object Events: looks every SoundBody's FMOD event up
    /// again by its path and stores the current GUID. The game finds events by
    /// GUID, so a scene keeps working after events were deleted and recreated in
    /// FMOD Studio (new GUIDs, same paths). Changes nothing else in the scene.
    /// </summary>
    public static class EventReferenceRelinker
    {
        [MenuItem("ProcAudio/Re-link Object Events")]
        public static void RelinkMenu() => Debug.Log("[ProcAudio] " + Relink());

        /// <summary>Re-links the open scenes; returns a one-line summary.</summary>
        public static string Relink()
        {
            int changed = 0, ok = 0, missing = 0;
            foreach (var b in Object.FindObjectsByType<SoundBody>(FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                string path = b.soundEvent.Path;
                if (string.IsNullOrEmpty(path)) continue;   // surfaces have no event
                EventReference found;
                try { found = EventReference.Find(path); }
                catch (System.Exception e)
                {
                    Debug.LogError($"[ProcAudio] {b.name}: {path}: {e.Message}");
                    ++missing;
                    continue;
                }
                if (found.Guid.IsNull)
                {
                    Debug.LogError($"[ProcAudio] {b.name}: FMOD event {path} not found in the built banks");
                    ++missing;
                    continue;
                }
                if (found.Guid.Equals(b.soundEvent.Guid)) { ++ok; continue; }
                Undo.RecordObject(b, "Re-link FMOD event");
                b.soundEvent = found;
                EditorUtility.SetDirty(b);
                EditorSceneManager.MarkSceneDirty(b.gameObject.scene);
                ++changed;
            }
            for (int i = 0; i < SceneManager.sceneCount; ++i)
            {
                var sc = SceneManager.GetSceneAt(i);
                if (sc.isDirty && !string.IsNullOrEmpty(sc.path)) EditorSceneManager.SaveScene(sc);
            }
            return $"event references: {changed} re-linked, {ok} already current, {missing} not found";
        }
    }
}
