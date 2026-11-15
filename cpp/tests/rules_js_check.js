// The ProcAudio panel's motion rules (procaudio_studio_menu.js, JavaScript)
// must match the plug-in's (pa::check_action, C++). This runs both over
// every object x target x motion and compares what plays and why.
//
//   node cpp/tests/rules_js_check.js
//
// Needs cpp/build/Release/procaudio_render.exe (its "rules" mode).
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");
const { execFileSync } = require("child_process");

const root = path.resolve(__dirname, "..", "..");
const exe = path.join(root, "cpp", "build", "Release", "procaudio_render.exe");
const csv = execFileSync(exe, ["rules"], { encoding: "utf8" }).trim().split(/\r?\n/);

// the menu script runs outside Studio: no `studio`, so it only defines ProcAudio
const ctx = { console };
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(root, "cpp", "plugins", "fmod", "procaudio_studio_menu.js"), "utf8")
                + "\nthis.ProcAudio = ProcAudio;", ctx);
const PA = ctx.ProcAudio;

const SHAPES = ["Ball", "Stick", "Box", "Panel", "Ground"];
const MOUNTS = ["Free", "Held", "Resting", "Built-in"];
const MOTIONS = ["Impact", "Drop", "Roll", "Slide"];
let checked = 0, bad = 0;
for (const line of csv.slice(1)) {
    const [os, om, ts, tm, wanted, plays, reason = ""] = line.split(",");
    const base = { oShape: SHAPES.indexOf(os), oMount: MOUNTS.indexOf(om) };
    const states = [Object.assign({ target: SHAPES.indexOf(ts) + 1, tMount: MOUNTS.indexOf(tm) }, base)];
    if (os === ts && om === tm)   // "Same as object" must give the same answer
        states.push(Object.assign({ target: 0, tMount: 0 }, base));
    for (const st of states) {
        const r = PA.checkMotion(st, MOTIONS.indexOf(wanted));
        const jsPlays = r.use < 0 ? "None" : MOTIONS[r.use];
        ++checked;
        if (jsPlays !== plays || r.reason !== reason) {
            ++bad;
            if (bad <= 20)
                console.log(`MISMATCH ${os}/${om} on ${st.target === 0 ? "same" : ts + "/" + tm}, ${wanted}: ` +
                            `C++ ${plays} "${reason}", JS ${jsPlays} "${r.reason}"`);
        }
    }
}
console.log(`rules: ${checked} cases, ${bad} mismatches between the panel (JS) and the plug-in (C++)`);

// The panel's adapting lists (Object shape / mount, Target, Target mount):
// over every state, an offered choice leaves some motion possible, a
// hidden one leaves none, and the current value always stays.
const FIELDS = { oShape: 5, oMount: 4, target: 6, tMount: 4 };
let states = 0, listBad = 0;
const fail = (msg) => { if (++listBad <= 20) console.log("LIST " + msg); };
for (let oShape = 0; oShape < 5; ++oShape)
    for (let oMount = 0; oMount < 4; ++oMount)
        for (let target = 0; target < 6; ++target)
            for (let tMount = 0; tMount < 4; ++tMount) {
                const st = { oShape, oMount, target, tMount, motion: 0 };
                ++states;
                for (const [f, n] of Object.entries(FIELDS)) {
                    const opts = PA.listOptions(st, f);
                    if (!opts.includes(st[f])) fail(`${JSON.stringify(st)} ${f}: current value missing`);
                    for (let v = 0; v < n; ++v) {
                        if (v === st[f]) continue;
                        const can = PA.possible(Object.assign({}, st, { [f]: v }));
                        if (can !== opts.includes(v))
                            fail(`${JSON.stringify(st)} ${f}=${v}: ${can ? "possible but hidden" : "impossible but offered"}`);
                    }
                }
            }
// the example: a Built-in object leaves the target only Free or Held, and
// no Ground or Same as object (both would be fixed too)
const fixedBall = { oShape: 0, oMount: 3, target: 2, tMount: 0, motion: 0 };   // built-in ball, free stick
const tm = PA.listOptions(fixedBall, "tMount"), tg = PA.listOptions(fixedBall, "target");
if (tm.join() !== "0,1") fail(`built-in object: target mounts ${tm} (want Free, Held)`);
if (tg.includes(0) || tg.includes(5)) fail(`built-in object: targets ${tg} still offer Same as object / Ground`);
const onGround = { oShape: 2, oMount: 0, target: 5, tMount: 3, motion: 0 };     // free box on the ground
if (PA.listOptions(onGround, "oMount").join() !== "0,1,2")
    fail(`object on the ground: mounts ${PA.listOptions(onGround, "oMount")} (want Free, Held, Resting)`);
console.log(`lists: ${states} states x 4 lists, ${listBad} problems`);
process.exit(bad || listBad ? 1 : 0);
