using System;
using FMODUnity;
using UnityEngine;

namespace ProcAudio
{
    /// <summary>
    /// Anything that takes part in a contact sound.
    ///
    /// Dynamic objects (with a Rigidbody) have an FMOD event, event:/Objects/&lt;Name&gt;,
    /// with their own body baked into its ProcAudio Contact instrument. They forward
    /// their collisions to the ContactAudioManager, which decides who plays.
    ///
    /// Static surfaces (floor, rug, table, walls) have no event: they are only ever
    /// the target of a contact, described to the object's event through its
    /// Target parameters. Colliders on child objects count as this body.
    /// </summary>
    public class SoundBody : MonoBehaviour
    {
        public string displayName = "Object";
        [Tooltip("What the plug-in models when this body is the TARGET of a contact. For objects, " +
                 "keep it in step with the body baked into the FMOD event.")]
        public BodyDescription description =
            BodyDescription.Make(PaShape.Ground, PaMaterial.Concrete, 4f, PaMount.BuiltIn);
        [Tooltip("This object's FMOD event (objects only; surfaces leave it empty)")]
        public EventReference soundEvent;

        /// <summary>Set by the grabber while the player holds this object.</summary>
        [NonSerialized] public bool held;
        /// <summary>While held: the surface the grabber is pressing it onto (null = carried in
        /// the air). Only a slide on this surface sounds.</summary>
        [NonSerialized] public SoundBody pressedOn;

        public Rigidbody Body { get; private set; }
        public bool IsDynamic => Body != null && !Body.isKinematic;
        public bool HasEvent => !soundEvent.IsNull;

        /// <summary>What it is doing (written by the ContactAudioManager, shown by the HUD).</summary>
        [NonSerialized] public string status = "";
        [NonSerialized] public string lastImpact = "";
        [NonSerialized] public float lastTouchTime = -1f;

        Vector3 startPos;
        Quaternion startRot;

        void Awake()
        {
            Body = GetComponent<Rigidbody>();
            startPos = transform.position;
            startRot = transform.rotation;
        }

        void OnEnable() => ContactAudioManager.Bodies.Add(this);
        void OnDisable() => ContactAudioManager.Bodies.Remove(this);

        // Only bodies with a Rigidbody receive these; static surfaces are found
        // through the collider of the other side.
        void OnCollisionEnter(Collision c) => ContactAudioManager.Instance?.Report(this, c, true);
        void OnCollisionStay(Collision c) => ContactAudioManager.Instance?.Report(this, c, false);

        public string Describe() =>
            Body != null ? $"{description.Pretty()} · {Body.mass:0.###} kg" : description.Pretty();

        public string Status()
        {
            if (Body != null && Body.IsSleeping()) return "at rest";
            if (Time.time - lastTouchTime > 0.1f) return "in the air";
            return status.Length > 0 ? status : "touching";
        }

        public void ResetPose()
        {
            if (Body == null) return;
            Body.linearVelocity = Vector3.zero;
            Body.angularVelocity = Vector3.zero;
            Body.position = startPos;
            Body.rotation = startRot;
            transform.SetPositionAndRotation(startPos, startRot);
        }
    }
}
