using UnityEngine;

namespace ProcAudio
{
    /// <summary>
    /// A static thing that objects hit, roll and slide on (floor, rug, table, wall).
    /// Colliders on this GameObject or its children count as this surface.
    /// </summary>
    public class ProcAudioSurface : MonoBehaviour
    {
        public string displayName = "Surface";
        public BodyDescription description =
            BodyDescription.Make(PaShape.Ground, PaMaterial.Concrete, 4f, PaMount.BuiltIn);

        public string Key => description.Key;

        void OnEnable() => ProcAudioEngine.Surfaces.Add(this);
        void OnDisable() => ProcAudioEngine.Surfaces.Remove(this);

        /// <summary>Changes what the surface is made of; bodies build voices for it right away.</summary>
        public void SetDescription(BodyDescription d, string newName)
        {
            description = d;
            displayName = newName;
            if (ProcAudioEngine.Instance != null) ProcAudioEngine.Instance.NotifySurfaceChanged(this);
        }
    }
}
