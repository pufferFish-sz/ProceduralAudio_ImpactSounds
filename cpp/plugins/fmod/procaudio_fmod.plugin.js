/* FMOD Studio deck layout for the "ProcAudio Contact" plug-in instrument
   (v2.2: any object against any target). Must sit next to procaudio_fmod.dll
   (with procaudio_fmod_divider.png) in a Studio plug-in folder. Without it
   Studio shows every parameter as a dial (FMOD Studio 2.02 Plug-in Scripting
   API: registerPluginDescription; deck widgets Dropdown, Dial, Pixmap,
   Layout with isFramed).

   Three framed columns, a line between OBJECT and TARGET:
     OBJECT  what this event's object is
     TARGET  what it hits, lands, rolls or slides on. "Same as object" is
             the first Target choice (two equal bodies); the other target
             controls are then ignored and their dials say so. Floor finish
             applies to Panel and Ground targets only. (Studio 2.02 decks
             cannot grey controls out: deck widgets have no enabled state.)
     MOTION  how they meet: Motion + one Speed knob, Render, Level, and
             Fit length (v2.4): fade to silence at the instrument's end.
             ProcAudio > Ending: Fit writes it from the instrument's length.
   Studio titles each bound control with its displayName.
   Design: proto/docs/plugin_body_design.md. */

var SHAPES = ["Ball", "Stick", "Box", "Panel", "Ground"];
var TARGETS = ["Same as object"].concat(SHAPES);
var MATERIALS = ["metal", "glass", "wood", "plastic", "rubber", "stone", "cardboard",
                 "concrete", "ceramic"];
var FINISHES = ["Bare", "Polished", "Tiled", "Carpeted"];
var MOUNTS = ["Free", "Held", "Resting", "Built-in"];

function dropdown(binding) {
    return { deckWidgetType: studio.ui.deckWidgetType.Dropdown, binding: binding, minimumWidth: 110 };
}
function dial(binding, color) {
    var d = { deckWidgetType: studio.ui.deckWidgetType.Dial, binding: binding };
    if (color) d.color = color;
    return d;
}
function vbox(items, framed) {
    var l = { deckWidgetType: studio.ui.deckWidgetType.Layout,
              layout: studio.ui.layoutType.VBoxLayout, spacing: 4, items: items };
    if (framed) {
        l.isFramed = true;
        l.contentsMargins = { left: 8, top: 6, right: 8, bottom: 6 };
    }
    return l;
}
function hbox(items, spacing) {
    return { deckWidgetType: studio.ui.deckWidgetType.Layout,
             layout: studio.ui.layoutType.HBoxLayout, spacing: spacing || 6, items: items };
}
var divider = { deckWidgetType: studio.ui.deckWidgetType.Pixmap,
                filePath: __dirname + "/procaudio_fmod_divider.png" };

studio.plugins.registerPluginDescription("ProcAudio Contact", {
    companyName: "ProcAudio",
    productName: "Contact",
    parameters: {
        "Obj Shape": { displayName: "Object", enumeration: SHAPES },
        "Obj Material": { displayName: "Material", enumeration: MATERIALS },
        "Obj Mount": { displayName: "Mount", enumeration: MOUNTS },
        "Obj Size": { displayName: "Size" },
        "Obj Thickness": { displayName: "Thickness" },
        "Obj Hit Pos": { displayName: "Hit position" },
        "Target": { displayName: "Target", enumeration: TARGETS },
        "Tgt Material": { displayName: "Material", enumeration: MATERIALS },
        "Tgt Finish": { displayName: "Floor finish", enumeration: FINISHES },
        "Tgt Mount": { displayName: "Mount", enumeration: MOUNTS },
        "Tgt Size": { displayName: "Size" },
        "Tgt Thickness": { displayName: "Thickness" },
        "Tgt Hit Pos": { displayName: "Hit position" },
        "Motion": { displayName: "Motion", enumeration: ["Impact", "Drop", "Roll", "Slide"] },
        "Speed": { displayName: "Speed" },
        "Render": { displayName: "Render", enumeration: ["Both", "Object only", "Target only"] },
        "Level": { displayName: "Level" },
        "Fit Length": { displayName: "Fit length" },
    },
    // deck context menu. The physics spans ~75 dB (impacts ~130 dB SPL,
    // rolling / sliding 50-75 dB), so continuous motions start from a raised
    // Level and a walking-pace Speed. The rest are starting points that show
    // what the deck can do.
    defaultConfigurations: [
        { name: "Impact", parameterValues: { "Motion": 0, "Speed": 2.0, "Level": 0 } },
        { name: "Drop", parameterValues: { "Motion": 1, "Speed": 2.0, "Level": 0 } },
        { name: "Roll", parameterValues: { "Motion": 2, "Speed": 0.5, "Level": 35 } },
        { name: "Slide", parameterValues: { "Motion": 3, "Speed": 0.5, "Level": 50 } },
        { name: "Stick knocks a wooden ball",
          parameterValues: { "Obj Shape": 1, "Obj Material": 2, "Obj Mount": 1, "Obj Size": 0.6,
                             "Obj Thickness": 0, "Obj Hit Pos": 1.0, "Target": 1,
                             "Tgt Material": 2, "Tgt Finish": 0, "Tgt Mount": 0, "Tgt Size": 0.12,
                             "Motion": 0, "Speed": 3.0, "Level": 0 } },
        { name: "Two glass marbles clink",
          parameterValues: { "Obj Shape": 0, "Obj Material": 1, "Obj Mount": 0, "Obj Size": 0.025,
                             "Target": 0, "Motion": 0, "Speed": 0.5, "Level": 0 } },
        { name: "Cardboard box dropped on concrete",
          parameterValues: { "Obj Shape": 2, "Obj Material": 6, "Obj Mount": 0, "Obj Size": 0.3,
                             "Obj Thickness": 0, "Target": 5, "Tgt Material": 7,
                             "Tgt Finish": 0, "Tgt Mount": 3, "Tgt Size": 4.0, "Motion": 1,
                             "Speed": 2.5, "Level": 0 } },
        { name: "Steel ball rolls on a polished wood floor",
          parameterValues: { "Obj Shape": 0, "Obj Material": 0, "Obj Mount": 0, "Obj Size": 0.06,
                             "Target": 4, "Tgt Material": 2, "Tgt Finish": 1,
                             "Tgt Mount": 3, "Tgt Size": 2.4, "Motion": 2, "Speed": 0.5,
                             "Level": 35 } },
    ],
    deckUi: hbox([
        // OBJECT: what this event's object is
        vbox([
            hbox([vbox([dropdown("Obj Shape"), dropdown("Obj Material")]),
                  vbox([dropdown("Obj Mount")])]),
            hbox([dial("Obj Size", "#3a7bd5"), dial("Obj Thickness"), dial("Obj Hit Pos")]),
        ], true),
        divider,
        // TARGET: what it hits, lands, rolls or slides on
        vbox([
            hbox([vbox([dropdown("Target"), dropdown("Tgt Material")]),
                  vbox([dropdown("Tgt Finish"), dropdown("Tgt Mount")])]),
            hbox([dial("Tgt Size", "#3a9d5d"), dial("Tgt Thickness"), dial("Tgt Hit Pos")]),
        ], true),
        // MOTION: how they meet
        vbox([
            vbox([dropdown("Motion"), dropdown("Render")]),
            hbox([dial("Speed", "#d5793a"), dial("Level"), dial("Fit Length")]),
        ], true),
    ], 10),
});
