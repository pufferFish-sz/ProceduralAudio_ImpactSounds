/* ProcAudio Contact: FMOD Studio menu ("ProcAudio"). Install next to Studio's
   own user scripts: %LOCALAPPDATA%/FMOD Studio/Scripts (Scripts > Reload).

   The plug-in deck (procaudio_fmod.plugin.js) is static: Studio 2.02 decks
   cannot hide options or grey controls out. Studio SCRIPTS can, so this
   menu adds what the deck cannot do:

   How a ProcAudio instrument ends. Studio stops a synchronous instrument
   dead at its end, so a short instrument cuts the ring. Studio's own
   envelopes (timeline fade-out, AHDSR release) change the volume once per
   mix block and still end in a cut on short instruments. Pick one ending:
   ProcAudio > Ending: Fit Selected to Their Length
       The sound fades to silence exactly at the instrument's end. This
       writes each selected instrument's length into the plug-in's Fit
       Length (the plug-in fades sample by sample) and makes it synchronous.
       A script cannot see an instrument being resized: after resizing, run
       Ending: Update Fitted Lengths.
   ProcAudio > Ending: Ring Out Selected (Async + Release)
       The sound rings past the instrument's end: Async + Cut with an AHDSR
       release on the volume (long enough to ring out: seconds), Fit off.
   ProcAudio > Ending: Update Fitted Lengths (Whole Project)
       Every fitted ProcAudio instrument in the project gets its current
       length again (ring-out instruments are left alone).
   ProcAudio > Open Panel (follows the selection)
       A window beside the deck that adapts, which the deck cannot (Studio
       fixes a deck when the plug-in loads). It follows the selected ProcAudio
       instrument, hides rows that do not apply (target rows for "Same as
       object", Floor finish except Panel / Ground, Thickness and Hit
       position for balls), offers only the motions the pair can do and says
       why, and writes every change straight to the instrument. It shows the
       last Play's peak ("Make audible" for a pair too quiet to hear) and the
       ending (Fit to length / Ring out). Same rules as the plug-in
       (pa::check_action in cpp/core/src/body_desc.cpp).
   ProcAudio > Show Last Render
       The plug-in saves every Play inside Studio as a WAV
       (%LOCALAPPDATA%/FMOD Studio/ProcAudio). This imports the newest one
       into the project and shows it in the Assets browser (waveform).

   Design: proto/docs/plugin_body_design.md, Parts 8-10. */

var ProcAudio = (function () {
    var PLUGIN = "ProcAudio Contact";
    var SHAPES = ["Ball", "Stick", "Box", "Panel", "Ground"];
    var MATERIALS = ["metal", "glass", "wood", "plastic", "rubber", "stone", "cardboard",
                     "concrete", "ceramic"];
    var FINISHES = ["Bare", "Polished", "Tiled", "Carpeted"];
    var MOUNTS = ["Free", "Held", "Resting", "Built-in"];
    var MOTIONS = ["Impact", "Drop", "Roll", "Slide"];
    var RENDERS = ["Both", "Object only", "Target only"];
    var BALL = 0, BOX = 2, PANEL = 3, GROUND = 4;
    var FREE = 0, RESTING = 2, BUILTIN = 3;
    var IMPACT = 0, DROP = 1, ROLL = 2, SLIDE = 3, NONE = -1;

    // ------------------------------------------------ the instrument

    function isProcAudio(obj) {
        return obj && obj.entity === "PluginSound" && obj.plugin &&
               obj.plugin.identifier === PLUGIN;
    }

    function selectedSounds() {
        var sel = [];
        try { sel = studio.window.editorSelection(); } catch (e) {}
        if (!sel || !sel.length) {
            try { var d = studio.window.deckCurrent(); if (d) sel = [d]; } catch (e) {}
        }
        return (sel || []).filter(isProcAudio);
    }

    function params(sound) {
        var map = {};
        sound.plugin.pluginParameters.forEach(function (p) { map[p.name] = p; });
        return map;
    }

    // the plug-in's parameters as one plain object (sizes in m)
    function read(sound) {
        var p = params(sound), v = function (n, d) { return p[n] ? p[n].value : d; };
        return {
            oShape: v("Obj Shape", 0), oMaterial: v("Obj Material", 0), oMount: v("Obj Mount", 0),
            oSize: v("Obj Size", 0.06), oThick: v("Obj Thickness", 0), oHit: v("Obj Hit Pos", 0.3),
            target: v("Target", 4), tMaterial: v("Tgt Material", 2), tFinish: v("Tgt Finish", 1),
            tMount: v("Tgt Mount", 3), tSize: v("Tgt Size", 2.4), tThick: v("Tgt Thickness", 0),
            tHit: v("Tgt Hit Pos", 0.3), motion: v("Motion", 0), speed: v("Speed", 2.0),
            render: v("Render", 0), level: v("Level", 0), fit: v("Fit Length", 0)
        };
    }

    function write(sound, st) {
        var p = params(sound);
        var set = function (n, x) { if (p[n] !== undefined && x !== undefined && !isNaN(x)) p[n].value = x; };
        set("Obj Shape", st.oShape); set("Obj Material", st.oMaterial); set("Obj Mount", st.oMount);
        set("Obj Size", st.oSize); set("Obj Thickness", st.oThick); set("Obj Hit Pos", st.oHit);
        set("Target", st.target); set("Tgt Material", st.tMaterial); set("Tgt Finish", st.tFinish);
        set("Tgt Mount", st.tMount); set("Tgt Size", st.tSize); set("Tgt Thickness", st.tThick);
        set("Tgt Hit Pos", st.tHit); set("Motion", st.motion); set("Speed", st.speed);
        set("Render", st.render); set("Level", st.level); set("Fit Length", st.fit);
    }

    // ------------------------------------------------ what makes sense
    // Mirrors pa::check_action / pa::target_supports (body_desc.cpp).

    function targetShape(st) { return st.target === 0 ? st.oShape : st.target - 1; }
    function targetMount(st) { return st.target === 0 ? st.oMount : st.tMount; }

    // Ground is always Built-in
    function mountOf(shape, mount) { return shape === GROUND ? BUILTIN : mount; }
    function objMount(st) { return mountOf(st.oShape, st.oMount); }
    function tgtMount(st) { return mountOf(targetShape(st), targetMount(st)); }
    var HELD = 1;
    function moving(m) { return m === FREE || m === HELD; }
    function turns(shape, m) { return shape === BALL && (m === FREE || m === RESTING); }

    // a flat top (Box, Panel, Ground) that holds still (not Free)
    function supports(st) {
        var s = targetShape(st);
        return (s === BOX || s === PANEL || s === GROUND) && tgtMount(st) !== FREE;
    }

    // each motion's own test: "" when it can happen, else why not
    function whyNot(st, a) {
        var om = objMount(st), tm = tgtMount(st), sup = supports(st);
        if (a === IMPACT) return !moving(om) && !moving(tm) ? "nothing moves: both at rest" : "";
        if (a === DROP) {
            if (om === BUILTIN) return "fixed objects can't fall";
            if (om === HELD) return "held objects don't fall";
            if (om === RESTING) return "already resting: can't fall";
            return sup ? "" : "nothing flat to land on";
        }
        if (a === ROLL) {
            if (st.oShape !== BALL) return "only balls roll";
            if (om === BUILTIN) return "fixed objects can't move";
            if (!turns(st.oShape, om)) return "a held ball can't turn";
            return sup ? "" : "nothing flat to roll on";
        }
        if (a === SLIDE) {
            if (om === BUILTIN) return "fixed objects can't move";
            if (turns(st.oShape, om)) return "a free ball rolls instead";
            if (om === HELD) return tm !== FREE ? "" : "the target would fly off";
            return sup ? "" : "nothing flat to slide on";
        }
        return "";
    }

    // { use: motion index or NONE, reason: "" when it plays as asked }.
    // Mirrors pa::check_action: Drop -> Impact, Roll -> Slide, Slide -> Roll
    // when the closest motion can happen, else NONE.
    function checkMotion(st, wanted) {
        var why = whyNot(st, wanted);
        if (!why) return { use: wanted, reason: "" };
        var alt = wanted === DROP ? IMPACT : wanted === ROLL ? SLIDE : wanted === SLIDE ? ROLL : NONE;
        if (alt !== NONE && whyNot(st, alt)) alt = NONE;
        return { use: alt, reason: why };
    }

    function validMotions(st) {
        var out = [];
        for (var m = 0; m < MOTIONS.length; ++m)
            if (checkMotion(st, m).use === m) out.push(m);
        return out;
    }

    // ------------------------------------------------ lists that adapt
    // The Shape, Mount and Target lists offer a choice only if, with
    // everything else as it is, some motion could still happen: a Built-in
    // object leaves the target only Free or Held (something has to move),
    // and so on. The value set now always stays in its list, so the panel
    // shows an instrument as it is even when nothing can happen.
    var TARGETS = ["Same as object"].concat(SHAPES);
    var FILTERED = { oShape: SHAPES, oMount: MOUNTS, target: TARGETS, tMount: MOUNTS };
    var FILTERED_NAMES = { oShape: "Object shape", oMount: "Object mount", target: "Target",
                           tMount: "Target mount" };

    function possible(st) { return validMotions(st).length > 0; }

    // the values of `field` (oShape, oMount, target, tMount) to offer
    function listOptions(st, field) {
        var out = [];
        for (var v = 0; v < FILTERED[field].length; ++v) {
            var st2 = {};
            for (var k in st) st2[k] = st[k];
            st2[field] = v;
            if (v === st[field] || possible(st2)) out.push(v);
        }
        return out;
    }

    // ------------------------------------------------ endings

    function volumeAdsr(sound) {
        var adsr = null;
        sound.modulators.forEach(function (m) {
            if (m.entity === "ADSRModulator" && m.nameOfPropertyBeingModulated === "volume") adsr = m;
        });
        return adsr;
    }

    // Plug-in v2.4 added the Fit Length parameter. Studio will not build a
    // bank whose ProcAudio instruments lack a parameter the plug-in has ("not
    // compatible with their placed instances"), and it does not add it by
    // itself. So this script adds it to every older instrument, with the
    // dial's mapping curve exactly as Studio stores it (g_fit_vals /
    // g_fit_pos in procaudio_fmod.cpp: keep the two in step).
    var FIT_CURVE = "{(0.000000,0.000000)(0.111111,0.050000)(0.222222,0.100000)" +
                    "(0.333333,0.200000)(0.444444,0.500000)(0.555556,1.000000)" +
                    "(0.666667,2.000000)(0.777778,5.000000)(0.888889,10.000000)" +
                    "(1.000000,30.000000)}";

    function addFitParameter(sound) {
        if (params(sound)["Fit Length"] !== undefined) return false;
        var p = studio.project.create("FloatPluginParameter");
        p.name = "Fit Length";
        p.value = 0;
        p.curve = FIT_CURVE;
        sound.plugin.relationships.pluginParameters.add(p);
        return true;
    }

    // v2.5 game control: float parameters that override the dropdowns (-1 = off),
    // so automation / game parameters can set them. [name, maximum]
    var GAME_PARAMS = [["Game Motion", 3], ["Game Target", 5], ["Game Tgt Mat", 8],
                       ["Game Tgt Finish", 3], ["Game Tgt Mount", 3], ["Game Obj Mount", 3]];

    function addGameParameters(sound) {
        var have = params(sound), added = false;
        GAME_PARAMS.forEach(function (g) {
            if (have[g[0]] !== undefined) return;
            var p = studio.project.create("FloatPluginParameter");
            p.name = g[0];
            p.value = -1;
            p.curve = "{(0.000000,-1.000000)(1.000000," + g[1].toFixed(6) + ")}";
            sound.plugin.relationships.pluginParameters.add(p);
            added = true;
        });
        return added;
    }

    // every ProcAudio instrument in the project: run when the project loads.
    // Instruments saved by an older plug-in lack the newer parameters, and a
    // bank build rejects them ("not compatible with their placed instances").
    function upgradeOne(sound) {
        var fit = addFitParameter(sound), game = addGameParameters(sound);
        return fit || game;
    }
    function upgradeAll() {
        return studio.project.model.PluginSound.findInstances().filter(isProcAudio)
            .filter(upgradeOne).length;
    }

    function hasFit(sound) {
        addFitParameter(sound);
        return params(sound)["Fit Length"] !== undefined;
    }

    // Fit: synchronous, the plug-in fades to silence at the instrument's end.
    // Returns false for a plug-in older than v2.4 (no Fit Length).
    function fit(sound) {
        if (!hasFit(sound)) return false;
        sound.isAsync = false;
        sound.isCutoff = false;
        var adsr = volumeAdsr(sound);   // Ring Out's release: no use when synchronous
        if (adsr && adsr.attackTime === 0 && adsr.holdTime === 0 && adsr.decayTime === 0 &&
            adsr.initialValue === 0 && adsr.peakValue === 0 && adsr.sustainValue === 0)
            studio.project.deleteObject(adsr);
        write(sound, { fit: sound.length });
        return true;
    }

    // every fitted instrument in the project gets its current length again
    function updateFitted() {
        var n = 0;
        studio.project.model.PluginSound.findInstances().filter(isProcAudio).forEach(function (s) {
            var f = read(s).fit;
            if (f > 0 && !s.isAsync && Math.abs(f - s.length) > 1e-6) {
                write(s, { fit: s.length });
                ++n;
            }
        });
        return n;
    }

    // Ring Out: Async + Cut + an AHDSR release, Fit off
    function setUp(sound, releaseMs) {
        if (hasFit(sound)) write(sound, { fit: 0 });
        sound.isAsync = true;    // untriggered, not cut by the end of the region ...
        sound.isCutoff = true;   // ... and stopped through the AHDSR release
        var adsr = volumeAdsr(sound);
        if (!adsr) {
            adsr = studio.project.create("ADSRModulator");
            adsr.nameOfPropertyBeingModulated = "volume";
            adsr.objectBeingModulated = sound;
        }
        adsr.initialValue = 0;   // dB: no fade-in, the hit is at the start
        adsr.peakValue = 0;
        adsr.sustainValue = 0;
        adsr.attackTime = 0;     // ms
        adsr.holdTime = 0;
        adsr.decayTime = 0;
        adsr.releaseTime = releaseMs;
        return adsr;
    }

    // ------------------------------------------------ last render

    function captureDir() { return __dirname + "/../ProcAudio"; }

    function lastRenderPath() {
        var f = studio.system.getFile(captureDir() + "/last_render.txt");
        if (!f.exists()) return "";
        f.open(studio.system.openMode.ReadOnly);
        var path = String(f.readText(4096)).trim();
        f.close();
        return path;
    }

    function importLastRender() {
        var path = lastRenderPath();
        if (!path) return null;
        return studio.project.importAudioFile(path);
    }

    // ------------------------------------------------ the last Play

    // { peak: dBFS, file } of the newest Play the plug-in saved, or null
    function lastPlay() {
        var f = studio.system.getFile(captureDir() + "/last_play.txt");
        if (!f.exists()) return null;
        f.open(studio.system.openMode.ReadOnly);
        var lines = String(f.readText(4096)).split("\n");
        f.close();
        var pk = parseFloat(lines[0]);
        return isNaN(pk) ? null : { peak: pk, file: String(lines[1] || "").trim() };
    }

    // the newest Play made with exactly this instrument's settings (an event
    // plays all its instruments at once, so "the last Play" is ambiguous):
    // { peak: dBFS } or null. plays.txt lines (procaudio_fmod.cpp): peak,
    // then the parameter values in this order.
    var PLAY_FIELDS = ["oShape", "oMaterial", "oMount", "oSize", "oThick", "oHit", "target",
                       "tMaterial", "tFinish", "tMount", "tSize", "tThick", "tHit", "motion",
                       "speed", "render", "level"];
    function playOf(sound) {
        var f = studio.system.getFile(captureDir() + "/plays.txt");
        if (!f.exists()) return null;
        f.open(studio.system.openMode.ReadOnly);
        var lines = String(f.readText(200000)).split("\n");
        f.close();
        var st = read(sound);
        var same = function (a, b) { return Math.abs(a - b) <= 1e-4 * Math.max(1, Math.abs(b)); };
        for (var i = lines.length - 1; i >= 0; --i) {
            var v = lines[i].trim().split(/\s+/).map(parseFloat);
            if (v.length !== PLAY_FIELDS.length + 1) continue;
            var ok = PLAY_FIELDS.every(function (k, j) { return same(v[j + 1], st[k]); });
            if (ok) return { peak: v[0] };
        }
        return null;
    }

    // ------------------------------------------------ the live panel
    // A modeless window beside the deck. Studio's deck is fixed when the
    // plug-in loads (a second registerPluginDescription is ignored, deck
    // scripts have no timers, and scripts cannot trigger Scripts > Reload),
    // so this panel is the deck that adapts: it follows the selected
    // ProcAudio instrument (polling every 250 ms; Studio has no change events
    // for scripts), HIDES what does not apply, offers only the motions the
    // pair can do, writes every change straight to the instrument (the deck
    // follows), and shows the last Play's peak with a fix for a pair that is
    // too quiet to hear.

    var panel = { open: false, sound: null, shown: "", peak: "" };
    var busy = false;   // set while the panel writes its own widgets

    function items(names, indices) {
        return indices.map(function (i) { return { text: names[i], userData: i }; });
    }
    function range(n) { var a = []; for (var i = 0; i < n; ++i) a.push(i); return a; }
    function combo(id, names) {
        return { widgetType: studio.ui.widgetType.ComboBox, widgetId: id,
                 items: items(names, range(names.length)),
                 onCurrentIndexChanged: function () { changed(this, id); } };
    }
    function edit(id) {
        return { widgetType: studio.ui.widgetType.LineEdit, widgetId: id, text: "",
                 onEditingFinished: function () { changed(this, id); } };
    }
    function label(text, id) {
        var l = { widgetType: studio.ui.widgetType.Label, text: text, wordWrap: true };
        if (id) l.widgetId = id;
        return l;
    }
    function button(id, text, onClicked) {
        return { widgetType: studio.ui.widgetType.PushButton, widgetId: id, text: text,
                 onClicked: onClicked };
    }
    // a grid row: its label gets the widget's id + "L", so the row can hide
    function row(r, id, text, widget) {
        widget.row = r; widget.column = 1;
        var l = label(text, id + "L"); l.row = r; l.column = 0;
        return [l, widget];
    }
    function grid(rows) {
        var all = [];
        rows.forEach(function (r) { all = all.concat(r); });
        return { widgetType: studio.ui.widgetType.Layout, layout: studio.ui.layoutType.GridLayout,
                 items: all };
    }

    var COMBOS = ["oShape", "oMaterial", "oMount", "target", "tMaterial", "tFinish", "tMount",
                  "render"];
    var EDITS = [["oSize", "oSize", 100, 1], ["oThick", "oThick", 1000, 1], ["oHit", "oHit", 1, 2],
                 ["tSize", "tSize", 100, 1], ["tThick", "tThick", 1000, 1], ["tHit", "tHit", 1, 2],
                 ["speed", "speed", 1, 2], ["level", "level", 1, 1]];   // id, field, scale, decimals
    var ROWS = ["oShape", "oMaterial", "oMount", "oSize", "oThick", "oHit", "target", "tMaterial",
                "tFinish", "tMount", "tSize", "tThick", "tHit", "motion", "speed", "render", "level"];

    function stateFrom(w) {
        var st = {};
        COMBOS.forEach(function (id) { st[id] = w.findWidget(id).currentUserData(); });
        st.motion = w.findWidget("motion").currentUserData();
        EDITS.forEach(function (e) { st[e[1]] = parseFloat(w.findWidget(e[0]).text()) / e[2]; });
        return st;
    }

    function showRow(w, id, on) {
        w.findWidget(id).setVisible(on);
        w.findWidget(id + "L").setVisible(on);
    }

    // what applies to this state: rows, the Motion list, the notes
    function layout(w, st) {
        var same = st.target === 0, tShape = targetShape(st);
        ["tMaterial", "tMount", "tSize", "tThick", "tHit"].forEach(function (id) {
            showRow(w, id, !same);
        });
        showRow(w, "tFinish", !same && (tShape === PANEL || tShape === GROUND));
        if (!same && tShape === GROUND) showRow(w, "tMount", false);
        showRow(w, "oThick", st.oShape !== BALL);
        showRow(w, "oHit", st.oShape !== BALL);
        showRow(w, "tThick", !same && tShape !== BALL);
        showRow(w, "tHit", !same && tShape !== BALL);
        // Shape, Mount, Target: only choices that leave something to happen
        var hiddenChoices = [];
        Object.keys(FILTERED).forEach(function (f) {
            var opts = listOptions(st, f), c = w.findWidget(f);
            c.setItems(items(FILTERED[f], opts));
            c.setCurrentIndex(Math.max(0, opts.indexOf(st[f])));
            var rowShown = f !== "tMount" || (!same && tShape !== GROUND);
            var gone = range(FILTERED[f].length).filter(function (v) { return opts.indexOf(v) < 0; });
            if (rowShown && gone.length)
                hiddenChoices.push(FILTERED_NAMES[f] + " " + gone.map(function (v) {
                    return FILTERED[f][v]; }).join(", "));
        });
        // Motion: only what this pair can do; a stored motion it cannot do
        // is shown as what plays (the plug-in plays that too)
        var ok = validMotions(st), plays = checkMotion(st, st.motion);
        var mc = w.findWidget("motion");
        var notes = [];
        if (!ok.length) {
            // nothing can happen for this pair (e.g. two bodies at rest):
            // keep the stored Motion visible but greyed, and say why
            mc.setItems([{ text: MOTIONS[st.motion] + " (silent)", userData: st.motion }]);
            mc.setCurrentIndex(0);
            mc.setEnabled(false);
            notes.push("Nothing can happen for this pair: " + whyNot(st, IMPACT) +
                       ". Make the object or the target Free or Held.");
        } else {
            mc.setEnabled(true);
            if (plays.use >= 0) {
                mc.setItems(items(MOTIONS, ok));
                mc.setCurrentIndex(Math.max(0, ok.indexOf(plays.use)));
            } else {
                // the stored motion is silent for this pair: show it as such
                // (first item) rather than pretend another motion is set
                mc.setItems([{ text: MOTIONS[st.motion] + " (silent)", userData: st.motion }]
                            .concat(items(MOTIONS, ok)));
                mc.setCurrentIndex(0);
            }
            if (plays.reason) notes.push("Motion " + MOTIONS[st.motion] + " plays as " +
                                         (plays.use >= 0 ? MOTIONS[plays.use] : "nothing (silent)") +
                                         ": " + plays.reason + ".");
            var hidden = range(MOTIONS.length).filter(function (m) { return ok.indexOf(m) < 0; });
            if (hidden.length) notes.push("Not offered: " + hidden.map(function (m) {
                return MOTIONS[m] + " (" + checkMotion(st, m).reason + ")"; }).join(", ") + ".");
        }
        if (hiddenChoices.length)
            notes.push("Hidden because nothing could happen with them: " +
                       hiddenChoices.join("; ") + ".");
        w.findWidget("why").setText(notes.join(" "));
        // what Speed means for the motion that plays
        var m = plays.use, v = st.speed;   // the motion that plays
        w.findWidget("speedL").setText(
            m === DROP ? "Drop speed (m/s) = from " + (100 * v * v / 19.62).toFixed(0) + " cm" :
            m === ROLL ? "Rolling speed (m/s)" : m === SLIDE ? "Sliding speed (m/s)" :
            "Hit speed (m/s)");
    }

    function endingText(s, st) {
        if (!s.isAsync && st.fit > 0)
            return "Ending: fades out at " + st.fit.toFixed(3) + " s" +
                   (Math.abs(st.fit - s.length) > 1e-6 ? " (the instrument is " + s.length.toFixed(3) +
                    " s now: press Fit again)" : " (fits the instrument)") + ".";
        if (s.isAsync) {
            var a = volumeAdsr(s);
            return "Ending: rings out past the instrument" + (a ? " (" + a.releaseTime + " ms release)" : "") + ".";
        }
        return "Ending: cut dead at the instrument's end (" + s.length.toFixed(3) + " s). " +
               "Press Fit or Ring out.";
    }

    // the instrument -> the widgets
    function show(w, s, st) {
        busy = true;
        try {
            COMBOS.forEach(function (id) {   // the adapting lists are filled by layout()
                if (!FILTERED[id]) w.findWidget(id).setCurrentIndex(st[id]);
            });
            EDITS.forEach(function (e) {
                w.findWidget(e[0]).setText((st[e[1]] * e[2]).toFixed(e[3]));
            });
            ROWS.forEach(function (id) { w.findWidget(id).setEnabled(true); });
            layout(w, st);
            w.findWidget("title").setText("<b>" + (s.audioTrack && s.audioTrack.name ? s.audioTrack.name + ": " : "") +
                                          "ProcAudio instrument</b> (changes here go straight to the deck)");
            w.findWidget("ending").setText(endingText(s, st));
            ["fit", "ringout", "audible"].forEach(function (id) { w.findWidget(id).setEnabled(true); });
        } finally {
            busy = false;
        }
    }

    function showNothing(w) {
        busy = true;
        try {
            ROWS.forEach(function (id) { w.findWidget(id).setEnabled(false); });
            ["fit", "ringout", "audible"].forEach(function (id) { w.findWidget(id).setEnabled(false); });
            w.findWidget("title").setText("<b>Select a ProcAudio instrument</b> in the event editor.");
            w.findWidget("ending").setText("");
            w.findWidget("why").setText("");
        } finally {
            busy = false;
        }
    }

    function remember(s) {
        panel.shown = JSON.stringify(read(s)) + "|" + s.length + "|" + s.isAsync;
    }

    // a widget changed by the user -> the instrument. The stored Motion
    // changes only when the user picks one: a Roll that plays as a slide for
    // a held ball becomes a roll again once the ball is free.
    function changed(widget, id) {
        if (busy || !panel.sound) return;
        var st = stateFrom(widget);
        if (id !== "motion" || st.motion === undefined) st.motion = read(panel.sound).motion;
        var bad = EDITS.filter(function (e) { return isNaN(st[e[1]]); });
        if (bad.length) { show(widget, panel.sound, read(panel.sound)); return; }   // undo a bad entry
        write(panel.sound, st);
        busy = true;
        try { layout(widget, st); } finally { busy = false; }
        remember(panel.sound);
    }

    function showPeak(w) {
        var lp = panel.sound ? playOf(panel.sound) : null, text = "";
        if (lp) {
            text = "These settings peaked at " + lp.peak.toFixed(1) + " dBFS when last played";
            if (lp.peak < -40) text += ": very quiet. The plug-in is calibrated (0 dBFS = 130 dB " +
                                       "SPL at 1 m), so a quiet pair is quiet. Make audible " +
                                       "raises Level so it peaks at -12 dBFS.";
            else text += ".";
        } else if (panel.sound) {
            text = "Play the event to see how loud these settings are.";
        }
        if (text !== panel.peak) { panel.peak = text; w.findWidget("peak").setText(text); }
    }

    function tick(w) {
        var s = selectedSounds()[0] || null;
        if (!s) {
            if (panel.sound) { panel.sound = null; panel.shown = ""; showNothing(w); }
        } else {
            var st = read(s), key = JSON.stringify(st) + "|" + s.length + "|" + s.isAsync;
            if (!panel.sound || s.id !== panel.sound.id || key !== panel.shown) {
                panel.sound = s;
                panel.shown = key;
                show(w, s, st);
            }
        }
        showPeak(w);
    }

    function openPanel() {
        if (panel.open) return;
        panel.open = true;
        panel.sound = null; panel.shown = ""; panel.peak = "";
        var section = function (t) { return label("<b>" + t + "</b>"); };
        studio.ui.showModelessDialog({
            windowTitle: "ProcAudio Contact",
            windowWidth: 440,
            windowHeight: 880,
            widgetType: studio.ui.widgetType.Layout,
            layout: studio.ui.layoutType.VBoxLayout,
            onConstructed: function () { showNothing(this); tick(this); this.startTimer(250); },
            onTimerEvent: function () { try { tick(this); } catch (e) { console.log("ProcAudio panel: " + e); } },
            onClose: function () { panel.open = false; panel.sound = null; },
            items: [
                label("", "title"),
                section("OBJECT: what this event's object is"),
                grid([row(0, "oShape", "Shape", combo("oShape", SHAPES)),
                      row(1, "oMaterial", "Material", combo("oMaterial", MATERIALS)),
                      row(2, "oMount", "Mount", combo("oMount", MOUNTS)),
                      row(3, "oSize", "Size (cm)", edit("oSize")),
                      row(4, "oThick", "Thickness (mm, 0 = auto)", edit("oThick")),
                      row(5, "oHit", "Hit position (0 center, 1 edge)", edit("oHit"))]),
                section("TARGET: what it hits, lands, rolls or slides on"),
                grid([row(0, "target", "Target", combo("target", TARGETS)),
                      row(1, "tMaterial", "Material", combo("tMaterial", MATERIALS)),
                      row(2, "tFinish", "Floor finish", combo("tFinish", FINISHES)),
                      row(3, "tMount", "Mount", combo("tMount", MOUNTS)),
                      row(4, "tSize", "Size (cm)", edit("tSize")),
                      row(5, "tThick", "Thickness (mm, 0 = auto)", edit("tThick")),
                      row(6, "tHit", "Hit position (0 center, 1 edge)", edit("tHit"))]),
                section("MOTION: how they meet"),
                grid([row(0, "motion", "Motion", { widgetType: studio.ui.widgetType.ComboBox,
                          widgetId: "motion", items: items(MOTIONS, range(MOTIONS.length)),
                          onCurrentIndexChanged: function () { changed(this, "motion"); } }),
                      row(1, "speed", "Hit speed (m/s)", edit("speed")),
                      row(2, "render", "Render", combo("render", RENDERS)),
                      row(3, "level", "Level (dB)", edit("level"))]),
                label("", "why"),
                label("", "peak"),
                label("", "ending"),
                { widgetType: studio.ui.widgetType.Layout, layout: studio.ui.layoutType.HBoxLayout,
                  items: [
                      button("audible", "Make audible", function () {
                          var lp = panel.sound ? playOf(panel.sound) : null;
                          if (!lp) return;
                          var lv = Math.max(-40, Math.min(80, read(panel.sound).level + (-12 - lp.peak)));
                          write(panel.sound, { level: Math.round(lv) });
                      }),
                      button("fit", "Fit to length", function () { if (panel.sound) fit(panel.sound); }),
                      button("ringout", "Ring out", function () { if (panel.sound) setUp(panel.sound, 3000); }),
                  ] },
            ],
        });
    }

    // ------------------------------------------------ menu

    // Studio evaluates this script whenever a project loads
    if (typeof studio !== "undefined" && studio.project && studio.project.model) {
        try {
            var upgraded = upgradeAll();
            if (upgraded)
                console.log("ProcAudio: added the newer plug-in parameters (v2.4 Fit Length, " +
                            "v2.5 Game control) to " + upgraded +
                            " instrument(s); save the project to keep it.");
        } catch (e) {
            console.log("ProcAudio: could not upgrade instruments: " + e);
        }
    }

    if (typeof studio !== "undefined" && studio.menu) {
        studio.menu.addMenuItem({
            name: "ProcAudio\\Ending: Fit Selected to Their Length",
            isEnabled: function () { return selectedSounds().length > 0; },
            execute: function () {
                var sounds = selectedSounds(), done = sounds.filter(fit);
                var msg = "Fitted " + done.length + " ProcAudio instrument(s): each fades to " +
                          "silence at its end (" + done.map(function (s) {
                              return s.length.toFixed(3) + " s"; }).join(", ") + ").\n" +
                          "After resizing an instrument, run Ending: Update Fitted Lengths.";
                if (done.length < sounds.length)
                    msg += "\n" + (sounds.length - done.length) + " instrument(s) skipped: " +
                           "install plug-in v2.4 (no Fit Length parameter).";
                studio.system.message(msg);
            },
        });
        studio.menu.addMenuItem({
            name: "ProcAudio\\Ending: Ring Out Selected (Async + Release)",
            isEnabled: function () { return selectedSounds().length > 0; },
            execute: function () {
                var sounds = selectedSounds();
                var ms = studio.system.getNumber(
                    "Release time (ms) when the playback position leaves the instrument:", 3000);
                if (ms === undefined || ms === null || isNaN(ms)) return;
                sounds.forEach(function (s) { setUp(s, ms); });
                studio.system.message("Set up " + sounds.length + " ProcAudio instrument(s) to " +
                                      "ring out: Async + Cut with a " + ms + " ms release.");
            },
        });
        studio.menu.addMenuItem({
            name: "ProcAudio\\Ending: Update Fitted Lengths (Whole Project)",
            execute: function () {
                var n = updateFitted();
                studio.system.message(n ? "Updated " + n + " fitted ProcAudio instrument(s) to " +
                                          "their current length."
                                        : "Every fitted ProcAudio instrument already matches its length.");
            },
        });
        studio.menu.addMenuItem({ name: "ProcAudio\\--------", execute: function () {} });
        studio.menu.addMenuItem({
            name: "ProcAudio\\Open Panel (follows the selection)",
            execute: function () { openPanel(); },
        });
        studio.menu.addMenuItem({
            name: "ProcAudio\\Show Last Render",
            execute: function () {
                var asset = importLastRender();
                if (!asset) {
                    studio.system.message("No ProcAudio render yet: play a ProcAudio event in " +
                                          "Studio first (renders go to " + captureDir() + ").");
                    return;
                }
                studio.window.navigateTo(asset);
            },
        });
    }

    return { read: read, write: write, checkMotion: checkMotion, validMotions: validMotions,
             listOptions: listOptions, possible: possible,
             setUp: setUp, fit: fit, updateFitted: updateFitted, upgradeAll: upgradeAll, addGameParameters: addGameParameters, lastRenderPath: lastRenderPath, importLastRender: importLastRender,
             isProcAudio: isProcAudio, openPanel: openPanel, lastPlay: lastPlay, playOf: playOf, MOTIONS: MOTIONS };
})();
