using System;
using System.Globalization;
using UnityEngine;

namespace ProcAudio
{
    // Same order as the C++ enums (procaudio/body_desc.h) and pa::body_material().
    public enum PaShape { Ball, Stick, Box, Panel, Ground }
    public enum PaMaterial { Metal, Glass, Wood, Plastic, Rubber, Stone, Cardboard, Concrete, Ceramic }
    public enum PaMount { Free, Held, Resting, BuiltIn }
    public enum PaFinish { Bare, Polished, Tiled, Carpeted }

    /// <summary>
    /// One sounding body, as the C++ model sees it (pa::BodyDesc).
    /// Size means: Ball diameter, Stick length, Box longest side (the box is
    /// size x 2/3 size x 2/3 size), Panel longer span, Ground slab span.
    /// Thickness 0 = the shape/material default (stick section, box wall, plate).
    /// Finish applies to floors (Panel, Ground) only.
    /// </summary>
    [Serializable]
    public struct BodyDescription
    {
        public PaShape shape;
        public PaMaterial material;
        [Tooltip("m: ball diameter, stick length, box longest side, panel/slab span")]
        public float size;
        [Tooltip("m, 0 = auto: stick section, box wall, plate thickness")]
        public float thickness;
        public PaMount mount;
        [Range(0f, 1f), Tooltip("where it is struck: 0 center, 1 edge / end")]
        public float hit;
        [Tooltip("floors only: what the object touches")]
        public PaFinish finish;

        public static BodyDescription Make(PaShape shape, PaMaterial material, float size,
            PaMount mount = PaMount.Free, PaFinish finish = PaFinish.Bare, float thickness = 0f, float hit = 0.3f)
        {
            return new BodyDescription
            {
                shape = shape, material = material, size = size, thickness = thickness,
                mount = mount, hit = hit, finish = finish
            };
        }

        public BodyDescription WithMount(PaMount m)
        {
            var d = this;
            d.mount = m;
            return d;
        }

        public ProcAudioNative.PaDesc ToNative()
        {
            return new ProcAudioNative.PaDesc
            {
                shape = (int)shape, material = (int)material, size = size, thickness = thickness,
                mount = (int)mount, hit = hit, finish = (int)finish
            };
        }

        /// <summary>Identity of the description (voices are cached by it).</summary>
        public string Key => string.Format(CultureInfo.InvariantCulture, "{0}|{1}|{2:R}|{3:R}|{4}|{5:R}|{6}",
            (int)shape, (int)material, size, thickness, (int)mount, hit, (int)finish);

        public string Pretty()
        {
            string mat = material.ToString().ToLowerInvariant();
            string sz = size >= 1f ? $"{size:0.#} m" : $"{size * 100f:0.#} cm";
            string fin = finish == PaFinish.Bare ? "" : $", {finish.ToString().ToLowerInvariant()}";
            return $"{mat} {shape.ToString().ToLowerInvariant()} {sz}{fin}";
        }
    }
}
