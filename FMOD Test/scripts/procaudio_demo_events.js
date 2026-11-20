// Creates the Unity demo's object events: one event per object,
// event:/Objects/<Name>, each a single ProcAudio Contact instrument with the
// object's own body baked in (shape, material, size). What the object meets
// is NOT baked in: the game describes the other body through the shared
// parameter presets below, so any object can hit, roll or slide on any other
// object or surface without authoring the combinations.
//
// Parameters (presets in "ProcAudio Contact"), set by the Unity contact manager:
//   Motion            Impact | Roll | Slide   -> plug-in Motion (0 / 2 / 3) and Level
//   Speed             m/s: hit speed (Impact) or rolling / slip speed
//   Held              Free | Held             -> Obj Mount (a held ball slides)
//   Target Shape      Ball | Stick | Box | Panel | Ground        -> Target (1..5)
//   Target Material   metal | glass | wood | plastic | rubber | stone |
//                     cardboard | concrete | ceramic             -> Tgt Material
//   Target Finish     Bare | Polished | Tiled | Carpeted         -> Tgt Finish
//   Target Mount      Free | Held | Resting | Built-in           -> Tgt Mount
//   Target Size       m (0.005..20)                              -> Tgt Size
//   Target Thickness  m (0 = auto, ..0.3)                        -> Tgt Thickness
// Level follows Motion with the plug-in's presets: Impact 0 dB, Roll +35,
// Slide +50 (rolling and sliding are physically much quieter).
// Timeline: the instrument fills 0..0.1 s (Async + Cut, 2.5 s AHDSR release),
// with a sustain point at 0.05 s that holds only while Motion is Roll / Slide:
// an impact plays and rings out on its own; a roll / slide plays until the game
// stops the instance.
//
// Run headless with FMOD Studio closed:
//   fmodstudiocl -script procaudio_demo_events.js FMOD_Project_Test.fspro
// then build:  fmodstudiocl -build FMOD_Project_Test.fspro
// Re-running refills the events in the Objects folder (edits to their tracks and
// parameters are lost; their GUIDs, which Unity references, are kept);
// the presets are reused. Keep OBJECTS in step with the Unity scene builder
// (Assets/ProcAudio/Editor/DemoSceneBuilder.cs).

var BALL = 0, STICK = 1, BOX = 2;
var METAL = 0, GLASS = 1, WOOD = 2, PLASTIC = 3, RUBBER = 4, STONE = 5, CARDBOARD = 6;

var OBJECTS = [
    // name,             shape, material,  size (m), thickness (m, 0 auto), hit
    ["Steel Ball",        BALL,  METAL,     0.06,  0,    0.3],
    ["Glass Ball",        BALL,  GLASS,     0.05,  0,    0.3],
    ["Wooden Ball",       BALL,  WOOD,      0.10,  0,    0.3],
    ["Rubber Ball",       BALL,  RUBBER,    0.12,  0,    0.3],
    ["Stone Ball",        BALL,  STONE,     0.14,  0,    0.3],
    ["Plastic Ball",      BALL,  PLASTIC,   0.08,  0,    0.3],
    ["Tin Box",           BOX,   METAL,     0.15,  0,    0.3],
    ["Wooden Box",        BOX,   WOOD,      0.20,  0,    0.3],
    ["Plastic Crate",     BOX,   PLASTIC,   0.35,  0,    0.3],
    ["Cardboard Box S",   BOX,   CARDBOARD, 0.40,  0,    0.3],
    ["Cardboard Box M",   BOX,   CARDBOARD, 0.50,  0,    0.3],
    ["Cardboard Box L",   BOX,   CARDBOARD, 0.60,  0,    0.3],
    ["Wooden Stick",      STICK, WOOD,      0.60,  0.03, 0.8],
];

var EVENT_FOLDER = "Objects";
var PRESET_FOLDER = "ProcAudio Contact";
var BANK = "Impacts";
var RING_MS = 2500;            // AHDSR release: an impact rings out at most this long
                               // after its region; a stopped roll / slide fades with it

var ENUM = studio.project.parameterType.UserEnumeration;
var USER = studio.project.parameterType.User;
var PRESETS = [
    { name: "Motion", type: ENUM, min: 0, max: 2, enumerationLabels: ["Impact", "Roll", "Slide"] },
    { name: "Speed", type: USER, min: 0, max: 10, initial: 0 },
    { name: "Held", type: ENUM, min: 0, max: 1, enumerationLabels: ["Free", "Held"] },
    { name: "Target Shape", type: ENUM, min: 0, max: 4, enumerationLabels: ["Ball", "Stick", "Box", "Panel", "Ground"], initial: 4 },
    { name: "Target Material", type: ENUM, min: 0, max: 8,
      enumerationLabels: ["metal", "glass", "wood", "plastic", "rubber", "stone", "cardboard", "concrete", "ceramic"], initial: 7 },
    { name: "Target Finish", type: ENUM, min: 0, max: 3, enumerationLabels: ["Bare", "Polished", "Tiled", "Carpeted"] },
    { name: "Target Mount", type: ENUM, min: 0, max: 3, enumerationLabels: ["Free", "Held", "Resting", "Built-in"], initial: 3 },
    { name: "Target Size", type: USER, min: 0.005, max: 20, initial: 4 },
    { name: "Target Thickness", type: USER, min: 0, max: 0.3, initial: 0 },
];

// How Studio evaluates automation (measured with a traced plug-in):
//  - between two points it interpolates in the plug-in dial's POSITION space
//    (0..1), each point's value being given in real units. For a dial with a
//    breakpoint curve (Speed, Tgt Size, Tgt Thickness: evenly spaced positions
//    -> the g_*_vals tables in procaudio_fmod.cpp) a point at every breakpoint
//    with value = the breakpoint gives exactly value = parameter in between.
//  - a labeled (enumeration) parameter HOLDS the value of the point at or
//    below it: every label needs its own point.
function dialPoints(vals) {
    return vals.map(function (v) { return [v, v]; });
}
function labelPoints(n, f) {   // labels 0 .. n - 1 -> plug-in value f(label)
    var pts = [];
    for (var i = 0; i < n; ++i) pts.push([i, f(i)]);
    return pts;
}
function same(i) { return i; }
var SIZE_VALS = [0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20];
var THICK_VALS = [0, 0.0005, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.3];
var SPEED_VALS = [0, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10];

// plug-in parameter <- preset, as automation points (preset value, plug-in value).
// Studio applies automation to FLOAT plug-in parameters only: the dropdowns
// (Motion, Target, Tgt Material, ...) are driven through the plug-in's "Game"
// floats (v2.5), which override them (-1 = off, else the dropdown index).
var AUTOMATION = [
    ["Game Target", "Target Shape", labelPoints(5, function (i) { return i + 1; })],   // 0 = "Same as object"
    ["Game Tgt Mat", "Target Material", labelPoints(9, same)],
    ["Game Tgt Finish", "Target Finish", labelPoints(4, same)],
    ["Game Tgt Mount", "Target Mount", labelPoints(4, same)],
    ["Game Obj Mount", "Held", labelPoints(2, same)],
    ["Game Motion", "Motion", labelPoints(3, function (i) { return [0, 2, 3][i]; })],   // Impact, Roll, Slide
    ["Tgt Size", "Target Size", dialPoints(SIZE_VALS)],
    ["Tgt Thickness", "Target Thickness", dialPoints(THICK_VALS)],
    ["Speed", "Speed", dialPoints(SPEED_VALS)],
    ["Level", "Motion", labelPoints(3, function (i) { return [0, 35, 50][i]; })],
];

var LOG = [];
function log(m) { LOG.push(String(m)); }

// Instruments saved by an older plug-in (e.g. a hand-made test event) lack the
// v2.5 Game parameters, and a bank build then rejects them ("not compatible with
// their placed instances"). Add what is missing to every ProcAudio instrument,
// as the ProcAudio Studio menu script does when a project opens.
var GAME_PARAMS = [["Game Motion", 3], ["Game Target", 5], ["Game Tgt Mat", 8],
                   ["Game Tgt Finish", 3], ["Game Tgt Mount", 3], ["Game Obj Mount", 3]];
function upgradeInstruments() {
    var n = 0;
    studio.project.model.PluginSound.findInstances().forEach(function (snd) {
        if (!snd.plugin || snd.plugin.identifier !== "ProcAudio Contact") return;
        var have = pluginParams(snd), added = false;
        GAME_PARAMS.forEach(function (g) {
            if (have[g[0]] !== undefined) return;
            var p = studio.project.create("FloatPluginParameter");
            p.name = g[0];
            p.value = -1;
            p.curve = "{(0.000000,-1.000000)(1.000000," + g[1].toFixed(6) + ")}";
            snd.plugin.relationships.pluginParameters.add(p);
            added = true;
        });
        if (added) ++n;
    });
    if (n) log("upgraded " + n + " older ProcAudio instrument(s) with the v2.5 Game parameters");
}

function childNamed(folder, name, entity) {
    var found = null;
    folder.items.forEach(function (it) { if (it.name === name && it.isOfType(entity)) found = it; });
    return found;
}

function ensureFolder(parent, name, entity) {
    var f = childNamed(parent, name, entity);
    if (f) return f;
    f = studio.project.create(entity);
    f.name = name;
    f.folder = parent;
    return f;
}

function ensurePresets() {
    var ws = studio.project.workspace;
    var folder = ensureFolder(ws.masterParameterPresetFolder, PRESET_FOLDER, "ParameterPresetFolder");
    var out = {};
    PRESETS.forEach(function (def) {
        var owner = childNamed(folder, def.name, "ParameterPreset");
        var gp;
        if (owner) {
            gp = owner.parameter;
        } else {
            var d = { name: def.name, type: def.type, min: def.min, max: def.max };
            if (def.enumerationLabels) d.enumerationLabels = def.enumerationLabels;
            gp = ws.addGameParameter(d);
            gp.presetOwner.folder = folder;
            log("preset created: " + def.name);
        }
        if (def.initial !== undefined) gp.initialValue = def.initial;
        out[def.name] = gp;
    });
    return out;
}

function pluginParams(sound) {
    var map = {};
    sound.plugin.pluginParameters.forEach(function (p) { map[p.name] = p; });
    return map;
}

function makeEvent(obj, folder, bank, presets) {
    var ws = studio.project.workspace;
    var name = obj[0];
    // An existing event is emptied and refilled, not replaced: its GUID is what
    // Unity's EventReferences store, so it must survive a re-run.
    var ev = childNamed(folder, name, "Event");
    if (ev) {
        ev.groupTracks.forEach(function (t) { studio.project.deleteObject(t); });
        ev.parameters.forEach(function (p) { studio.project.deleteObject(p); });
        studio.project.model.SustainPoint.findInstances().forEach(function (sp) {
            if (sp.timeline && sp.timeline.id === ev.timeline.id) studio.project.deleteObject(sp);
        });
    } else {
        ev = ws.addEvent(name, true);             // with a spatializer on the master track
        ev.folder = folder;
    }
    if (ev.banks.indexOf(bank) < 0) ev.relationships.banks.add(bank);
    Object.keys(presets).forEach(function (k) { ev.addGameParameter(presets[k]); });

    var track = ev.addGroupTrack("Contact");
    var snd = ws.createPlugin("ProcAudio Contact");
    track.relationships.modules.add(snd);
    ev.timeline.relationships.modules.add(snd);
    snd.start = 0;
    snd.length = 0.1;
    // A plug-in instrument stops when the cursor leaves its region, Async or
    // not. Impacts: the cursor runs on, and Async + Cut lets the hit ring out
    // through the AHDSR release (the plug-in ends it sooner once it is quiet).
    // Roll / Slide: a sustain point inside the region holds the cursor while
    // Motion is Roll or Slide, so the instrument keeps playing until the game
    // stops the instance (which then fades through the same release).
    snd.isAsync = true;
    snd.isCutoff = true;
    var adsr = snd.addModulator("ADSRModulator", "volume");
    adsr.initialValue = 0; adsr.peakValue = 0; adsr.sustainValue = 0;
    adsr.attackTime = 0; adsr.holdTime = 0; adsr.decayTime = 0;
    adsr.releaseTime = RING_MS;

    var markers = ev.markerTracks.length ? ev.markerTracks[0] : ev.addMarkerTrack();
    var hold = markers.addSustainPoint(0.05, "Hold while rolling / sliding");
    var cond = studio.project.create("ParameterCondition");
    cond.parameter = presets["Motion"];
    cond.minimum = 1;        // Roll
    cond.maximum = 2;        // Slide
    hold.relationships.triggerConditions.add(cond);

    var p = pluginParams(snd);
    p["Obj Shape"].value = obj[1];
    p["Obj Material"].value = obj[2];
    p["Obj Size"].value = obj[3];
    p["Obj Thickness"].value = obj[4];
    p["Obj Hit Pos"].value = obj[5];
    p["Render"].value = 0;            // Both: the owner of a contact plays the pair
    if (p["Fit Length"]) p["Fit Length"].value = 0;

    AUTOMATION.forEach(function (a) {
        if (!p[a[0]]) throw new Error("plug-in parameter '" + a[0] + "' missing: FMOD Studio needs procaudio_fmod v2.5+");
        var curve = p[a[0]].addAutomator("value").addAutomationCurve(presets[a[1]]);
        a[2].forEach(function (pt) { curve.addAutomationPoint(pt[0], pt[1]); });
    });
    log("event: " + ev.getPath());
    return ev;
}

try {
    var ws = studio.project.workspace;
    var bank = studio.project.lookup("bank:/" + BANK);
    if (!bank) throw new Error("bank:/" + BANK + " not found");
    upgradeInstruments();
    var presets = ensurePresets();
    var folder = ensureFolder(ws.masterEventFolder, EVENT_FOLDER, "EventFolder");
    OBJECTS.forEach(function (o) { makeEvent(o, folder, bank, presets); });
    log("save: " + studio.project.save());
} catch (e) {
    log("ERROR: " + e);
}

var logFile = studio.system.getFile(__dirname + "/procaudio_demo_events.log");
logFile.open(studio.system.openMode.WriteOnly);
logFile.writeText(LOG.join("\n") + "\n");
logFile.close();
