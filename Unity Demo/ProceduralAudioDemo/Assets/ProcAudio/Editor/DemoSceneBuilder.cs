using System.Collections.Generic;
using System.IO;
using FMODUnity;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.InputSystem.UI;
using UnityEngine.SceneManagement;
using UnityEngine.UI;

namespace ProcAudio.EditorTools
{
    /// <summary>
    /// ProcAudio ▸ Build Impact Demo Scene: generates Assets/Scenes/ImpactDemo.unity
    /// (room, floor with selectable surface, rug / steel plate / table / ramp,
    /// objects, first-person player, HUD). Every object plays its FMOD event
    /// event:/Objects/&lt;Name&gt; (made by FMOD Test/scripts/procaudio_demo_events.js:
    /// keep the object list there in step with this one). Re-running it overwrites the scene, so
    /// tweak the generated scene directly, or change this script and rebuild.
    /// </summary>
    public static class DemoSceneBuilder
    {
        const string ScenePath = "Assets/Scenes/ImpactDemo.unity";
        const string MatDir = "Assets/ProcAudio/Materials";
        const string PhysDir = "Assets/ProcAudio/Physics";

        [MenuItem("ProcAudio/Build Impact Demo Scene")]
        public static void Build()
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;
            BuildScene();
        }

        /// <summary>Builds and saves the scene without prompting (for scripts / CLI).</summary>
        public static string BuildScene()
        {
            Directory.CreateDirectory(MatDir);
            Directory.CreateDirectory(PhysDir);
            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

            // ---------------------------------------------------------- engine + light
            new GameObject("Contact Audio Manager").AddComponent<ContactAudioManager>();

            var sun = new GameObject("Sun").AddComponent<Light>();
            sun.type = LightType.Directional;
            sun.intensity = 1.3f;
            sun.shadows = LightShadows.Soft;
            sun.transform.rotation = Quaternion.Euler(55f, -35f, 0f);
            var fill = new GameObject("Fill light").AddComponent<Light>();
            fill.type = LightType.Point;
            fill.range = 18f;
            fill.intensity = 1.5f;
            fill.transform.position = new Vector3(0f, 2.8f, 0f);

            // ---------------------------------------------------------- room
            var room = new GameObject("Room").transform;
            var floorGo = Box("Floor (selectable surface)", room, new Vector3(0f, -0.1f, 0f), new Vector3(16f, 0.2f, 12f),
                              Mat("Floor", new Color(0.62f, 0.62f, 0.6f), 0.2f));
            var options = SurfaceSelector.DefaultOptions();
            var floorSurf = floorGo.AddComponent<SoundBody>();
            floorSurf.displayName = options[0].name;
            floorSurf.description = options[0].description;
            var selector = floorGo.AddComponent<SurfaceSelector>();
            selector.options = options;
            selector.current = 0;
            selector.renderers = new[] { floorGo.GetComponent<Renderer>() };
            selector.colliders = new[] { floorGo.GetComponent<Collider>() };
            selector.faceSize = new Vector2(16f, 12f);

            var walls = new GameObject("Walls (concrete)").transform;
            walls.SetParent(room);
            var wallSurf = walls.gameObject.AddComponent<SoundBody>();
            wallSurf.displayName = "Concrete wall";
            wallSurf.description = BodyDescription.Make(PaShape.Ground, PaMaterial.Concrete, 4f, PaMount.BuiltIn);
            var wallMat = Mat("Wall", new Color(0.82f, 0.8f, 0.76f), 0.1f);
            Box("Wall N", walls, new Vector3(0f, 1.5f, 6.1f), new Vector3(16.4f, 3f, 0.2f), wallMat);
            Box("Wall S", walls, new Vector3(0f, 1.5f, -6.1f), new Vector3(16.4f, 3f, 0.2f), wallMat);
            Box("Wall E", walls, new Vector3(8.1f, 1.5f, 0f), new Vector3(0.2f, 3f, 12f), wallMat);
            Box("Wall W", walls, new Vector3(-8.1f, 1.5f, 0f), new Vector3(0.2f, 3f, 12f), wallMat);

            // ---------------------------------------------------------- fixed surfaces
            var fixtures = new GameObject("Surfaces").transform;
            Patch("Rug", fixtures, new Vector3(-4.5f, 0.006f, 1.5f), new Vector3(3f, 0.012f, 2f),
                  BodyDescription.Make(PaShape.Panel, PaMaterial.Wood, 2.4f, PaMount.BuiltIn, PaFinish.Carpeted),
                  new LookSpec { pattern = SurfacePattern.Carpet, color = new Color(0.62f, 0.2f, 0.18f), smoothness = 0f, metersPerTile = 0.5f },
                  Phys("Rug", 0.95f, 0f));
            Patch("Steel plate", fixtures, new Vector3(4.5f, 0.004f, 1.5f), new Vector3(2f, 0.008f, 2f),
                  BodyDescription.Make(PaShape.Panel, PaMaterial.Metal, 2f, PaMount.BuiltIn, PaFinish.Bare, 0.008f),
                  new LookSpec { pattern = SurfacePattern.Brushed, color = new Color(0.72f, 0.74f, 0.78f), smoothness = 0.65f, metallic = 0.9f, metersPerTile = 1f },
                  Phys("Steel plate", 0.35f, 0.25f));

            // table: one surface (the top rings; the legs count as the table too)
            var table = new GameObject("Wooden table").transform;
            table.SetParent(fixtures);
            var tSurf = table.gameObject.AddComponent<SoundBody>();
            tSurf.displayName = "Wooden table";
            tSurf.description = BodyDescription.Make(PaShape.Panel, PaMaterial.Wood, 1.8f, PaMount.BuiltIn, PaFinish.Polished, 0.025f);
            var tableMat = Mat("Table", new Color(0.5f, 0.32f, 0.18f), 0.55f);
            var top = Box("Top", table, new Vector3(0f, 0.74f, 3.5f), new Vector3(1.8f, 0.04f, 0.9f), tableMat);
            top.AddComponent<SurfaceLook>().look = new LookSpec { pattern = SurfacePattern.Planks, color = new Color(0.5f, 0.32f, 0.18f), smoothness = 0.6f, metersPerTile = 0.9f };
            top.GetComponent<Collider>().sharedMaterial = Phys("Table", 0.35f, 0.25f);
            foreach (var (x, z) in new[] { (-0.83f, 3.12f), (0.83f, 3.12f), (-0.83f, 3.88f), (0.83f, 3.88f) })
                Box("Leg", table, new Vector3(x, 0.36f, z), new Vector3(0.06f, 0.72f, 0.06f), tableMat);

            // ramp: varnished plank on a block, for rolling balls
            var ramp = new GameObject("Varnished ramp").transform;
            ramp.SetParent(fixtures);
            var rSurf = ramp.gameObject.AddComponent<SoundBody>();
            rSurf.displayName = "Varnished ramp";
            rSurf.description = BodyDescription.Make(PaShape.Panel, PaMaterial.Wood, 3f, PaMount.BuiltIn, PaFinish.Polished, 0.03f);
            const float angle = 12f;
            float rise = 3f * Mathf.Sin(angle * Mathf.Deg2Rad);
            var plank = Box("Plank", ramp, new Vector3(-5f, rise * 0.5f + 0.02f, -3f), new Vector3(1f, 0.04f, 3f), tableMat);
            plank.transform.rotation = Quaternion.Euler(-angle, 0f, 0f);
            plank.AddComponent<SurfaceLook>().look = new LookSpec { pattern = SurfacePattern.Planks, color = new Color(0.62f, 0.4f, 0.22f), smoothness = 0.8f, metersPerTile = 1f };
            plank.GetComponent<Collider>().sharedMaterial = Phys("Ramp", 0.3f, 0.2f);
            Box("Support", ramp, new Vector3(-5f, (rise - 0.02f) * 0.5f, -1.62f), new Vector3(1f, rise - 0.02f, 0.2f), tableMat);
            // side rails so balls stay on
            Box("Rail L", ramp, new Vector3(-5.52f, rise * 0.5f + 0.06f, -3f), new Vector3(0.04f, 0.08f, 3f), tableMat)
                .transform.rotation = Quaternion.Euler(-angle, 0f, 0f);
            Box("Rail R", ramp, new Vector3(-4.48f, rise * 0.5f + 0.06f, -3f), new Vector3(0.04f, 0.08f, 3f), tableMat)
                .transform.rotation = Quaternion.Euler(-angle, 0f, 0f);

            // ---------------------------------------------------------- objects
            var objects = new GameObject("Objects").transform;
            const float T = 0.76f;   // table top
            Ball("Steel Ball", objects, PaMaterial.Metal, 0.06f, 7850f, new Vector3(-0.6f, T, 3.35f), Mat("Steel", new Color(0.75f, 0.76f, 0.78f), 0.85f, 1f), Phys("Steel", 0.3f, 0.45f));
            Ball("Glass Ball", objects, PaMaterial.Glass, 0.05f, 2500f, new Vector3(-0.42f, T, 3.65f), Mat("Glass", new Color(0.65f, 0.85f, 0.9f), 0.95f), Phys("Glass", 0.3f, 0.5f));
            Ball("Wooden Ball", objects, PaMaterial.Wood, 0.1f, 700f, new Vector3(-0.15f, T, 3.4f), Mat("Wood", new Color(0.72f, 0.52f, 0.3f), 0.3f), Phys("Wood", 0.4f, 0.35f));
            BoxBody("Tin Box", objects, PaMaterial.Metal, 0.15f, 0f, 0.63f, new Vector3(0.25f, T, 3.5f), Mat("Tin", new Color(0.6f, 0.62f, 0.66f), 0.6f, 0.8f), Phys("Tin", 0.3f, 0.3f));
            BoxBody("Wooden Box", objects, PaMaterial.Wood, 0.2f, 0f, 0.6f, new Vector3(0.6f, T, 3.45f), Mat("Wood box", new Color(0.66f, 0.46f, 0.26f), 0.25f), Phys("Wood", 0.4f, 0.35f));

            Ball("Rubber Ball", objects, PaMaterial.Rubber, 0.12f, 1100f, new Vector3(1.5f, 0f, 0f), Mat("Rubber", new Color(0.85f, 0.2f, 0.15f), 0.35f), Phys("Rubber", 0.85f, 0.75f, PhysicsMaterialCombine.Maximum));
            Ball("Stone Ball", objects, PaMaterial.Stone, 0.14f, 2600f, new Vector3(-3.8f, 0f, -5f), Mat("Stone", new Color(0.5f, 0.5f, 0.48f), 0.3f), Phys("Stone", 0.5f, 0.2f));
            Ball("Plastic Ball", objects, PaMaterial.Plastic, 0.08f, 1050f, new Vector3(-4.3f, 0f, -5.2f), Mat("Plastic", new Color(0.2f, 0.45f, 0.85f), 0.6f), Phys("Plastic", 0.35f, 0.45f));
            Stick("Wooden Stick", objects, PaMaterial.Wood, 0.6f, 0.03f, 0.35f, new Vector3(-1.5f, 0f, 1f), Mat("Stick", new Color(0.78f, 0.6f, 0.38f), 0.3f), Phys("Wood", 0.4f, 0.35f));
            BoxBody("Plastic Crate", objects, PaMaterial.Plastic, 0.35f, 0f, 1.2f, new Vector3(2.5f, 0f, 2.5f), Mat("Crate", new Color(0.25f, 0.6f, 0.35f), 0.5f), Phys("Plastic", 0.35f, 0.4f));
            var card = Mat("Cardboard", new Color(0.72f, 0.55f, 0.36f), 0.05f);
            var cardPhys = Phys("Cardboard", 0.45f, 0.05f);
            BoxBody("Cardboard Box S", objects, PaMaterial.Cardboard, 0.4f, 0f, 0.32f, new Vector3(-2f, 0f, -1f), card, cardPhys);
            BoxBody("Cardboard Box M", objects, PaMaterial.Cardboard, 0.5f, 0f, 0.5f, new Vector3(-2.8f, 0f, 0.2f), card, cardPhys);
            BoxBody("Cardboard Box L", objects, PaMaterial.Cardboard, 0.6f, 0f, 0.72f, new Vector3(2.8f, 0f, -1.5f), card, cardPhys);

            // ---------------------------------------------------------- player
            var player = new GameObject("Player");
            player.transform.position = new Vector3(0f, 0.02f, -3.5f);
            var cc = player.AddComponent<CharacterController>();
            cc.height = 1.8f;
            cc.radius = 0.3f;
            cc.center = new Vector3(0f, 0.9f, 0f);
            cc.stepOffset = 0.25f;
            var fps = player.AddComponent<FirstPersonController>();
            var camGo = new GameObject("Main Camera");
            camGo.tag = "MainCamera";
            camGo.transform.SetParent(player.transform, false);
            camGo.transform.localPosition = new Vector3(0f, 1.6f, 0f);
            var cam = camGo.AddComponent<Camera>();
            cam.nearClipPlane = 0.03f;
            cam.fieldOfView = 70f;
            camGo.AddComponent<StudioListener>();   // the FMOD listener (no Unity AudioListener)
            fps.cameraPivot = camGo.transform;
            var grabber = camGo.AddComponent<ObjectGrabber>();
            grabber.cam = cam;
            grabber.playerCollider = cc;

            // ---------------------------------------------------------- HUD
            BuildHud(selector, grabber);

            EditorSceneManager.SaveScene(scene, ScenePath);
            AddToBuildSettings(ScenePath);
            AssetDatabase.SaveAssets();
            Debug.Log("[ProcAudio] Built " + ScenePath);
            return ScenePath;
        }

        // ------------------------------------------------------------------ helpers

        static GameObject Box(string name, Transform parent, Vector3 pos, Vector3 scale, Material mat)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
            go.name = name;
            go.transform.SetParent(parent, false);
            go.transform.position = pos;
            go.transform.localScale = scale;
            go.GetComponent<Renderer>().sharedMaterial = mat;
            go.isStatic = parent == null || parent.name != "Objects";
            return go;
        }

        static void Patch(string name, Transform parent, Vector3 pos, Vector3 scale, BodyDescription d, LookSpec look, PhysicsMaterial pm)
        {
            var go = Box(name, parent, pos, scale, Mat(name, look.color, look.smoothness, look.metallic));
            var s = go.AddComponent<SoundBody>();
            s.displayName = name;
            s.description = d;
            go.AddComponent<SurfaceLook>().look = look;
            go.GetComponent<Collider>().sharedMaterial = pm;
        }

        const string EventFolder = "event:/Objects/";

        static SoundBody Body(GameObject go, string name, BodyDescription d, float mass, Material mat, PhysicsMaterial pm)
        {
            go.name = name;
            go.isStatic = false;
            go.GetComponent<Renderer>().sharedMaterial = mat;
            go.GetComponent<Collider>().sharedMaterial = pm;
            var rb = go.AddComponent<Rigidbody>();
            rb.mass = mass;
            rb.interpolation = RigidbodyInterpolation.Interpolate;
            rb.collisionDetectionMode = CollisionDetectionMode.ContinuousDynamic;
            rb.angularDamping = d.shape == PaShape.Ball ? 0.3f : 0.05f;   // balls: some rolling resistance
            var b = go.AddComponent<SoundBody>();
            b.displayName = name;
            b.description = d;
            string path = EventFolder + name;
            try { b.soundEvent = EventReference.Find(path); }
            catch (System.Exception e) { Debug.LogError($"[ProcAudio] {path}: {e.Message}"); }
            if (b.soundEvent.Guid.IsNull)
                Debug.LogError($"[ProcAudio] FMOD event {path} not found: run procaudio_demo_events.js and build the banks");
            return b;
        }

        // a solid ball of this density (kg/m^3)
        static void Ball(string name, Transform parent, PaMaterial m, float diameter, float density, Vector3 restOn, Material mat, PhysicsMaterial pm)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            go.transform.SetParent(parent, false);
            go.transform.position = restOn + Vector3.up * (diameter * 0.5f + 0.002f);
            go.transform.localScale = Vector3.one * diameter;
            float r = diameter * 0.5f;
            Body(go, name, BodyDescription.Make(PaShape.Ball, m, diameter), density * 4f / 3f * Mathf.PI * r * r * r, mat, pm);
        }

        // Box: size x 2/3 size x 2/3 size (the model's box), lying on its big face
        static void BoxBody(string name, Transform parent, PaMaterial m, float size, float wall, float mass, Vector3 restOn, Material mat, PhysicsMaterial pm)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
            go.transform.SetParent(parent, false);
            float h = size * 2f / 3f;
            go.transform.position = restOn + Vector3.up * (h * 0.5f + 0.002f);
            go.transform.localScale = new Vector3(size, h, h);
            Body(go, name, BodyDescription.Make(PaShape.Box, m, size, PaMount.Free, PaFinish.Bare, wall), mass, mat, pm);
        }

        static void Stick(string name, Transform parent, PaMaterial m, float length, float section, float mass, Vector3 restOn, Material mat, PhysicsMaterial pm)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
            go.transform.SetParent(parent, false);
            go.transform.position = restOn + Vector3.up * (section * 0.5f + 0.002f);
            go.transform.localScale = new Vector3(length, section, section);
            go.transform.rotation = Quaternion.Euler(0f, 30f, 0f);
            Body(go, name, BodyDescription.Make(PaShape.Stick, m, length, PaMount.Free, PaFinish.Bare, section, 0.8f), mass, mat, pm);
        }

        static Material Mat(string name, Color c, float smoothness, float metallic = 0f)
        {
            string path = $"{MatDir}/{name}.mat";
            var mat = AssetDatabase.LoadAssetAtPath<Material>(path);
            if (mat == null)
            {
                var shader = Shader.Find("Universal Render Pipeline/Lit");
                if (shader == null) shader = Shader.Find("Standard");
                mat = new Material(shader);
                AssetDatabase.CreateAsset(mat, path);
            }
            if (mat.HasProperty("_BaseColor")) mat.SetColor("_BaseColor", c);
            else mat.color = c;
            if (mat.HasProperty("_Smoothness")) mat.SetFloat("_Smoothness", smoothness);
            if (mat.HasProperty("_Metallic")) mat.SetFloat("_Metallic", metallic);
            EditorUtility.SetDirty(mat);
            return mat;
        }

        static PhysicsMaterial Phys(string name, float friction, float bounce,
                                    PhysicsMaterialCombine bounceCombine = PhysicsMaterialCombine.Average)
        {
            string path = $"{PhysDir}/{name}.asset";
            var pm = AssetDatabase.LoadAssetAtPath<PhysicsMaterial>(path);
            if (pm == null)
            {
                pm = new PhysicsMaterial(name);
                AssetDatabase.CreateAsset(pm, path);
            }
            pm.dynamicFriction = friction;
            pm.staticFriction = friction * 1.1f;
            pm.bounciness = bounce;
            pm.frictionCombine = PhysicsMaterialCombine.Average;
            pm.bounceCombine = bounceCombine;
            EditorUtility.SetDirty(pm);
            return pm;
        }

        static void AddToBuildSettings(string path)
        {
            var list = new List<EditorBuildSettingsScene>(EditorBuildSettings.scenes);
            list.RemoveAll(s => s.path == path);
            list.Insert(0, new EditorBuildSettingsScene(path, true));
            EditorBuildSettings.scenes = list.ToArray();
        }

        // ------------------------------------------------------------------ HUD

        static Font font;

        static void BuildHud(SurfaceSelector selector, ObjectGrabber grabber)
        {
            font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
            var res = new DefaultControls.Resources
            {
                standard = AssetDatabase.GetBuiltinExtraResource<Sprite>("UI/Skin/UISprite.psd"),
                background = AssetDatabase.GetBuiltinExtraResource<Sprite>("UI/Skin/Background.psd"),
                inputField = AssetDatabase.GetBuiltinExtraResource<Sprite>("UI/Skin/InputFieldBackground.psd"),
                knob = AssetDatabase.GetBuiltinExtraResource<Sprite>("UI/Skin/Knob.psd"),
                checkmark = AssetDatabase.GetBuiltinExtraResource<Sprite>("UI/Skin/Checkmark.psd"),
                dropdown = AssetDatabase.GetBuiltinExtraResource<Sprite>("UI/Skin/DropdownArrow.psd"),
                mask = AssetDatabase.GetBuiltinExtraResource<Sprite>("UI/Skin/UIMask.psd"),
            };

            var es = new GameObject("EventSystem", typeof(EventSystem));
            var module = es.AddComponent<InputSystemUIInputModule>();
            module.AssignDefaultActions();

            var canvasGo = new GameObject("HUD", typeof(Canvas), typeof(CanvasScaler), typeof(GraphicRaycaster));
            var canvas = canvasGo.GetComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            var scaler = canvasGo.GetComponent<CanvasScaler>();
            scaler.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
            scaler.referenceResolution = new Vector2(1600f, 900f);
            scaler.matchWidthOrHeight = 0.5f;
            var hud = canvasGo.AddComponent<DemoHUD>();
            hud.floor = selector;
            hud.grabber = grabber;

            // panel (top left), stacked by a layout group
            var panel = new GameObject("Panel", typeof(RectTransform), typeof(Image), typeof(VerticalLayoutGroup), typeof(ContentSizeFitter));
            panel.transform.SetParent(canvasGo.transform, false);
            var prt = (RectTransform)panel.transform;
            prt.anchorMin = prt.anchorMax = prt.pivot = new Vector2(0f, 1f);
            prt.anchoredPosition = new Vector2(16f, -16f);
            prt.sizeDelta = new Vector2(380f, 0f);
            panel.GetComponent<Image>().color = new Color(0.05f, 0.06f, 0.08f, 0.72f);
            var vlg = panel.GetComponent<VerticalLayoutGroup>();
            vlg.padding = new RectOffset(14, 14, 12, 12);
            vlg.spacing = 4f;
            vlg.childControlHeight = true;
            vlg.childControlWidth = true;
            vlg.childForceExpandHeight = false;
            panel.GetComponent<ContentSizeFitter>().verticalFit = ContentSizeFitter.FitMode.PreferredSize;
            hud.panel = panel;

            Label(panel.transform, "Procedural impact audio", 20, FontStyle.Bold, 28f);
            Label(panel.transform, "Ground surface  (keys 1–7)", 14, FontStyle.Normal, 20f);
            var dd = DefaultControls.CreateDropdown(res);
            dd.name = "Surface dropdown";
            dd.transform.SetParent(panel.transform, false);
            Layout(dd, 32f);
            hud.surfaceDropdown = dd.GetComponent<Dropdown>();
            var template = (RectTransform)hud.surfaceDropdown.template;
            template.sizeDelta = new Vector2(template.sizeDelta.x, 230f);

            Spacer(panel.transform, 6f);
            hud.masterSlider = SliderRow(panel.transform, res, "FMOD master", out hud.masterLabel);
            Spacer(panel.transform, 6f);
            hud.hintText = Label(panel.transform,
                "WASD move · Shift run · Space jump\n" +
                "Left click: pick up / drop · Right click: throw\n" +
                "Wheel: hold distance · aim at the floor while holding to slide\n" +
                "Walk into boxes to push them · R: reset objects\n" +
                "Tab / Esc: cursor for this panel · click the scene to look",
                13, FontStyle.Normal, 96f);
            hud.hintText.color = new Color(0.8f, 0.85f, 0.9f);

            // status (bottom left)
            hud.statusText = Anchored(canvasGo.transform, "Status", new Vector2(0f, 0f), new Vector2(16f, 16f), new Vector2(760f, 130f),
                                      16, TextAnchor.LowerLeft);
            // event log (top right)
            hud.logText = Anchored(canvasGo.transform, "Log", new Vector2(1f, 1f), new Vector2(-16f, -16f), new Vector2(520f, 200f),
                                   14, TextAnchor.UpperRight);
            hud.logText.color = new Color(0.9f, 0.92f, 0.95f, 0.9f);

            // crosshair
            var ch = new GameObject("Crosshair", typeof(RectTransform), typeof(Image));
            ch.transform.SetParent(canvasGo.transform, false);
            var crt = (RectTransform)ch.transform;
            crt.anchorMin = crt.anchorMax = crt.pivot = new Vector2(0.5f, 0.5f);
            crt.sizeDelta = new Vector2(6f, 6f);
            var chImg = ch.GetComponent<Image>();
            chImg.sprite = res.knob;
            chImg.color = new Color(1f, 1f, 1f, 0.85f);
            chImg.raycastTarget = false;
            hud.crosshair = chImg;

            foreach (var t in canvasGo.GetComponentsInChildren<Text>(true)) t.font = font;
        }

        static void Layout(GameObject go, float h)
        {
            var le = go.GetComponent<LayoutElement>();
            if (le == null) le = go.AddComponent<LayoutElement>();
            le.preferredHeight = h;
            le.minHeight = h;
        }

        static void Spacer(Transform parent, float h)
        {
            var go = new GameObject("Spacer", typeof(RectTransform));
            go.transform.SetParent(parent, false);
            Layout(go, h);
        }

        static Text Label(Transform parent, string text, int size, FontStyle style, float h)
        {
            var go = new GameObject("Label", typeof(RectTransform), typeof(Text));
            go.transform.SetParent(parent, false);
            var t = go.GetComponent<Text>();
            t.text = text;
            t.fontSize = size;
            t.fontStyle = style;
            t.color = Color.white;
            t.raycastTarget = false;
            t.horizontalOverflow = HorizontalWrapMode.Wrap;
            t.verticalOverflow = VerticalWrapMode.Overflow;
            Layout(go, h);
            return t;
        }

        static Slider SliderRow(Transform parent, DefaultControls.Resources res, string name, out Text label)
        {
            label = Label(parent, name, 14, FontStyle.Normal, 18f);
            var s = DefaultControls.CreateSlider(res);
            s.name = name + " slider";
            s.transform.SetParent(parent, false);
            Layout(s, 18f);
            return s.GetComponent<Slider>();
        }

        static Text Anchored(Transform parent, string name, Vector2 corner, Vector2 offset, Vector2 size, int fontSize, TextAnchor align)
        {
            var go = new GameObject(name, typeof(RectTransform), typeof(Text), typeof(Shadow));
            go.transform.SetParent(parent, false);
            var rt = (RectTransform)go.transform;
            rt.anchorMin = rt.anchorMax = rt.pivot = corner;
            rt.anchoredPosition = offset;
            rt.sizeDelta = size;
            var t = go.GetComponent<Text>();
            t.fontSize = fontSize;
            t.alignment = align;
            t.color = Color.white;
            t.supportRichText = true;
            t.raycastTarget = false;
            t.horizontalOverflow = HorizontalWrapMode.Wrap;
            t.verticalOverflow = VerticalWrapMode.Overflow;
            go.GetComponent<Shadow>().effectColor = new Color(0f, 0f, 0f, 0.8f);
            return t;
        }
    }
}
