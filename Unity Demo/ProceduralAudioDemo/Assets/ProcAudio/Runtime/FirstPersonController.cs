using UnityEngine;
using UnityEngine.InputSystem;

namespace ProcAudio
{
    /// <summary>
    /// WASD + mouse look first-person controller (CharacterController). Walking into
    /// a rigidbody pushes it, so boxes can be shoved across the floor.
    /// Mouse look only runs while the cursor is locked (see DemoHUD).
    /// </summary>
    [RequireComponent(typeof(CharacterController))]
    public class FirstPersonController : MonoBehaviour
    {
        public Transform cameraPivot;
        public float walkSpeed = 3.5f;
        public float sprintSpeed = 6f;
        public float jumpSpeed = 4.5f;
        public float gravity = 15f;
        [Tooltip("degrees per mouse count")]
        public float lookSensitivity = 0.12f;

        [Header("Pushing")]
        [Tooltip("How hard walking into a rigidbody pushes it")]
        public float pushStrength = 2.5f;
        public float maxPushMass = 40f;

        CharacterController cc;
        float pitch, vy;
        Vector3 moveVel;

        public static bool LookEnabled => Cursor.lockState == CursorLockMode.Locked;

        void Awake()
        {
            cc = GetComponent<CharacterController>();
            if (cameraPivot == null && Camera.main != null) cameraPivot = Camera.main.transform;
        }

        void Update()
        {
            var kb = Keyboard.current;
            var mouse = Mouse.current;

            if (LookEnabled && mouse != null)
            {
                Vector2 d = mouse.delta.ReadValue() * lookSensitivity;
                transform.Rotate(0f, d.x, 0f, Space.World);
                pitch = Mathf.Clamp(pitch - d.y, -85f, 85f);
                if (cameraPivot != null) cameraPivot.localRotation = Quaternion.Euler(pitch, 0f, 0f);
            }

            Vector2 input = Vector2.zero;
            bool sprint = false, jump = false;
            if (kb != null)
            {
                if (kb.wKey.isPressed) input.y += 1f;
                if (kb.sKey.isPressed) input.y -= 1f;
                if (kb.dKey.isPressed) input.x += 1f;
                if (kb.aKey.isPressed) input.x -= 1f;
                sprint = kb.leftShiftKey.isPressed;
                jump = kb.spaceKey.wasPressedThisFrame;
            }
            input = Vector2.ClampMagnitude(input, 1f);
            float speed = sprint ? sprintSpeed : walkSpeed;
            moveVel = (transform.right * input.x + transform.forward * input.y) * speed;

            if (cc.isGrounded)
            {
                vy = -1f;
                if (jump) vy = jumpSpeed;
            }
            else
            {
                vy -= gravity * Time.deltaTime;
            }
            cc.Move((moveVel + Vector3.up * vy) * Time.deltaTime);
        }

        void OnControllerColliderHit(ControllerColliderHit hit)
        {
            var rb = hit.collider.attachedRigidbody;
            if (rb == null || rb.isKinematic || rb.mass > maxPushMass) return;
            if (hit.moveDirection.y < -0.3f) return;   // standing on it
            Vector3 dir = new Vector3(hit.moveDirection.x, 0f, hit.moveDirection.z);
            if (dir.sqrMagnitude < 1e-4f) return;
            dir.Normalize();
            // move it along at walking pace; lighter things are easier to shove
            float target = moveVel.magnitude;
            Vector3 v = rb.linearVelocity;
            float along = Vector3.Dot(v, dir);
            if (along < target)
            {
                float dv = Mathf.Min(target - along, pushStrength * Time.deltaTime * 60f / Mathf.Max(1f, rb.mass));
                rb.AddForceAtPosition(dir * dv * rb.mass, hit.point, ForceMode.Impulse);
            }
        }
    }
}
