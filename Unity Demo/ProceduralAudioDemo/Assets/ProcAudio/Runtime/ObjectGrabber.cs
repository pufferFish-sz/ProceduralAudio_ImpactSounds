using UnityEngine;
using UnityEngine.InputSystem;

namespace ProcAudio
{
    /// <summary>
    /// Pick up, carry, slide and throw rigidbodies.
    ///   Left click   pick up / let go
    ///   Right click  throw (while holding)
    ///   Mouse wheel  hold distance
    /// The held object follows the crosshair by velocity, so it still collides.
    /// Aim at the floor (or a table) while holding and the object is pressed onto
    /// it: move the mouse and it slides there (a held object sends Held = 1: a held
    /// ball slides instead of rolling).
    /// </summary>
    public class ObjectGrabber : MonoBehaviour
    {
        public Camera cam;
        public Collider playerCollider;
        public float reach = 3.5f;
        public float minHold = 0.6f, maxHold = 3f;
        public float holdDistance = 1.6f;
        public float maxCarryMass = 25f;
        public float throwSpeed = 9f;
        [Tooltip("How far into a surface under the crosshair the object is pressed (m)")]
        public float pressDepth = 0.015f;
        [Tooltip("On pick-up the hold point starts this much (m) closer than where you clicked, so " +
                 "the object comes up off the surface instead of being pressed into it and dragged")]
        public float pickUpLift = 0.25f;
        [Tooltip("Position gain: fraction of the gap closed per physics step")]
        public float follow = 0.5f;
        public float maxSpeed = 10f;
        [Tooltip("Let go when the object stays this far (m) from where it is held for jamTime: it is " +
                 "wedged in geometry (under a table, between legs) and the hold keeps forcing it in")]
        public float jamDistance = 0.3f;
        public float jamTime = 0.5f;
        public LayerMask mask = ~0;

        public SoundBody Held => heldBody;
        public SoundBody Hover { get; private set; }

        Rigidbody held;
        SoundBody heldBody;
        Quaternion rotOffset;
        float savedDamping, savedAngularDamping;
        bool savedGravity;
        RigidbodyInterpolation savedInterp;
        Vector3 target;
        float jamFor;

        void Awake()
        {
            if (cam == null) cam = GetComponentInChildren<Camera>();
            if (cam == null) cam = Camera.main;
        }

        void Update()
        {
            var mouse = Mouse.current;
            Hover = null;
            if (held == null && cam != null &&
                Physics.Raycast(cam.transform.position, cam.transform.forward, out var hit, reach, mask, QueryTriggerInteraction.Ignore))
            {
                Hover = hit.rigidbody != null ? hit.rigidbody.GetComponent<SoundBody>() : null;
            }
            if (mouse == null || !FirstPersonController.LookEnabled) return;

            if (mouse.leftButton.wasPressedThisFrame)
            {
                if (held != null) Release(Vector3.zero);
                else TryGrab();
            }
            if (held != null && mouse.rightButton.wasPressedThisFrame)
            {
                float s = throwSpeed / Mathf.Max(1f, Mathf.Sqrt(held.mass));
                Release(cam.transform.forward * s);
            }
            float wheel = mouse.scroll.ReadValue().y;
            if (Mathf.Abs(wheel) > 0.01f)
                holdDistance = Mathf.Clamp(holdDistance + Mathf.Sign(wheel) * 0.15f, minHold, maxHold);
        }

        void TryGrab()
        {
            if (!Physics.Raycast(cam.transform.position, cam.transform.forward, out var hit, reach, mask, QueryTriggerInteraction.Ignore))
                return;
            var rb = hit.rigidbody;
            if (rb == null || rb.isKinematic || rb.mass > maxCarryMass) return;
            held = rb;
            heldBody = rb.GetComponent<SoundBody>();
            if (heldBody != null)
            {
                heldBody.held = true;
                heldBody.pressedOn = null;   // picked up: carried, not pressed
            }
            jamFor = 0f;
            holdDistance = Mathf.Clamp(hit.distance - pickUpLift, minHold, maxHold);
            savedGravity = rb.useGravity;
            savedDamping = rb.linearDamping;
            savedAngularDamping = rb.angularDamping;
            savedInterp = rb.interpolation;
            rb.useGravity = false;
            rb.linearDamping = 0f;
            rb.angularDamping = 0f;
            rb.interpolation = RigidbodyInterpolation.Interpolate;
            rb.angularVelocity = Vector3.zero;
            // keep its orientation relative to where the player faces
            rotOffset = Quaternion.Inverse(YawRotation()) * rb.rotation;
            SetIgnore(true);
        }

        public void Release(Vector3 throwVelocity)
        {
            if (held == null) return;
            SetIgnore(false);
            held.useGravity = savedGravity;
            held.linearDamping = savedDamping;
            held.angularDamping = savedAngularDamping;
            held.interpolation = savedInterp;
            if (throwVelocity != Vector3.zero) held.linearVelocity += throwVelocity;
            if (heldBody != null)
            {
                heldBody.held = false;
                heldBody.pressedOn = null;
            }
            held = null;
            heldBody = null;
        }

        void SetIgnore(bool ignore)
        {
            if (playerCollider == null || held == null) return;
            foreach (var c in held.GetComponentsInChildren<Collider>()) Physics.IgnoreCollision(c, playerCollider, ignore);
        }

        Quaternion YawRotation() => Quaternion.Euler(0f, cam.transform.eulerAngles.y, 0f);

        void FixedUpdate()
        {
            if (held == null) return;
            Vector3 origin = cam.transform.position, fwd = cam.transform.forward;
            target = origin + fwd * holdDistance;

            // Something under the crosshair closer than the hold point: press the
            // object onto it (so it slides there) instead of pushing through.
            var b = HalfExtentAlong(held, Vector3.up);
            var hits = Physics.RaycastAll(origin, fwd, holdDistance + b, mask, QueryTriggerInteraction.Ignore);
            float best = float.MaxValue;
            RaycastHit bestHit = default;
            foreach (var h in hits)
            {
                if (h.rigidbody == held) continue;
                if (playerCollider != null && h.collider == playerCollider) continue;
                if (h.distance < best) { best = h.distance; bestHit = h; }
            }
            SoundBody pressing = null;
            if (best < float.MaxValue)
            {
                float ext = HalfExtentAlong(held, bestHit.normal);
                Vector3 onSurface = bestHit.point + bestHit.normal * (ext - pressDepth);
                if ((onSurface - origin).magnitude < holdDistance + 0.01f)
                {
                    target = onSurface;
                    pressing = bestHit.collider.GetComponentInParent<SoundBody>();
                }
            }
            if (heldBody != null) heldBody.pressedOn = pressing;

            Vector3 v = (target - held.position) * (follow / Time.fixedDeltaTime);
            held.linearVelocity = Vector3.ClampMagnitude(v, maxSpeed);

            Quaternion want = YawRotation() * rotOffset;
            Quaternion delta = want * Quaternion.Inverse(held.rotation);
            delta.ToAngleAxis(out float angle, out Vector3 axis);
            if (angle > 180f) angle -= 360f;
            held.angularVelocity = float.IsFinite(axis.x) && Mathf.Abs(angle) > 0.01f
                ? axis * (angle * Mathf.Deg2Rad * 0.3f / Time.fixedDeltaTime)
                : Vector3.zero;

            // snagged far away, or wedged: the hold keeps forcing it into geometry
            float gap = (held.position - target).magnitude;
            jamFor = gap > jamDistance ? jamFor + Time.fixedDeltaTime : 0f;
            if (gap > 1.5f || jamFor > jamTime) Release(Vector3.zero);
        }

        static float HalfExtentAlong(Rigidbody rb, Vector3 dir)
        {
            float best = 0f;
            foreach (var c in rb.GetComponentsInChildren<Collider>())
            {
                if (c is SphereCollider s)
                {
                    best = Mathf.Max(best, s.radius * MaxAbs(s.transform.lossyScale));
                    continue;
                }
                var e = c.bounds.extents;   // world AABB: fine for a rough press point
                best = Mathf.Max(best, Mathf.Abs(dir.x) * e.x + Mathf.Abs(dir.y) * e.y + Mathf.Abs(dir.z) * e.z);
            }
            return best;
        }

        static float MaxAbs(Vector3 v) => Mathf.Max(Mathf.Abs(v.x), Mathf.Max(Mathf.Abs(v.y), Mathf.Abs(v.z)));
    }
}
