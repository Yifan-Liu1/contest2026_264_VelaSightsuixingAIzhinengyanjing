#!/usr/bin/env python3
"""Generate the social-session expression graphics as an LVGL C source file.

Four 44x44 colour faces, one per emotion bucket, drawn procedurally rather than
converted from checked-in PNGs.  The shapes are code, so a review sees the
geometry that produced them instead of a byte blob, and the whole thing runs on
a stock python3 with no pillow, no pypng and no lz4 -- the same trade
app/social_cue/tools/md2c.py already makes for the skill document.

Usage, from the repository root:

    python3 app/velasight/tools/gen_faces.py

The check that the source and the generated file have not drifted is to re-run
it and see that git reports no change.

Why RGB565 with the panel background baked in
---------------------------------------------
The panels are 160x160 RGB565 framebuffers, so an RGB565 source is the one
format LVGL can put on them with no conversion at all: lv_draw_sw_img.c's
"simplest case just copy the pixels" branch fires only for an untransformed,
unmasked image with no recolor and no colorkey, and it hands the source buffer
straight to the RGB565 blender.  An alpha channel would cost a per-pixel blend
on a 26 ms-a-frame SPI panel for no visible gain, because the one thing behind
these faces is a background this script already knows: VS_PANEL_BG, the colour
vs_panel_init() paints both screens with.

So the alpha is resolved here instead.  Every edge is supersampled 4x4 and
composited against VS_PANEL_BG at generation time, which buys antialiased
curves at exactly zero runtime cost.  The faces are only ever drawn on that
background -- vs_panel_set_face() shows them on the status screen and nowhere
else -- so a pre-composited edge is not an approximation of the alpha, it is
the same result the alpha would have produced.

Why the disc carries the emotion colour
--------------------------------------
The four fills are the palette vs_app.c already assigns to the four buckets
(VS_COLOR_NEUTRAL/HAPPY/CONFUSED/TENSE in vs_app.c, the same values
cloud_classify_emotion() sends).  Baking them means the face is chosen by
snapshot->emotion alone and snapshot->emotion_color never has to reach the
status panel -- which keeps emotion_color out of vs_status_changed(), where its
absence is deliberate and documented.

The features are painted in VS_PANEL_BG, so they read as cut out of the disc.
That is also what makes the antialiasing exact: both the outer edge of the disc
and the inner edges of the features blend between the fill and the one colour
that is actually underneath.
"""

import math
import pathlib
import sys

# The panel background, from vs_panel_init()'s lv_obj_set_style_bg_color().
# Both the composite target for every antialiased edge and the ink the features
# are drawn in, which is why it is one constant and not two.

VS_PANEL_BG = (7, 12, 21)

# The emotion palette, verbatim from vs_app.c.  VS_EMOTION_NONE is absent on
# purpose: it shares the neutral face, because emotion_color gives it the same
# VS_COLOR_NEUTRAL a calm frame gets and a page reporting nothing is meant to
# look like a page reporting calm.

VS_COLOR_NEUTRAL = (0xE8, 0xEE, 0xF2)
VS_COLOR_HAPPY = (0x48, 0xC7, 0x8E)
VS_COLOR_CONFUSED = (0x5D, 0x8F, 0xE8)
VS_COLOR_TENSE = (0xE8, 0x5D, 0x5D)

# 44 px is what the status screen's middle layer has spare once the value label
# is pulled up to a single row: the layer runs y=35..106, the value takes
# y=38..54, and 44 px placed at y=60 lands on y=104 with the lower divider at
# y=108.  See VS_FACE_SIZE in vs_display.c, which must agree with this.

SIZE = 44

# 4x4 per pixel.  Sixteen samples resolve a 21 px radius smoothly enough that
# the disc edge shows no steps at this size, and the generator runs in well
# under a second, so there is nothing to gain from being cleverer.

SUPERSAMPLE = 4

# Everything below is in output pixels, measured from the top-left corner of
# the 44x44 image, y downwards.  The face is a disc of radius 21 centred at
# (22,22), so it clears the edge of the image by a pixel on all four sides;
# LVGL places the image on a whole-pixel boundary, so that margin is what keeps
# the outermost antialiased ring inside the bitmap.

CENTER = (22.0, 22.0)
DISC_RADIUS = 21.0

# The eyes are the same two dots on all four faces.  Only the brows and the
# mouth carry the expression, which is deliberate: at 44 px a changing eye
# shape reads as noise, while a brow angle and a mouth curve read at a glance.

# y=18.5 rather than the geometric middle, so the brows have somewhere to be.
# At r=2.9 the eyes reach up to y=15.6, and the lowest brow tip below is at
# y=14.7 -- just under a pixel of clearance, which is what keeps an angled brow
# from merging into the eye under it at this size.

EYE_Y = 18.5
EYE_X = (15.0, 29.0)
EYE_RADIUS = 2.9


def _clamp(value, low, high):
    return low if value < low else high if value > high else value


def disc(cx, cy, radius):
    """A filled circle."""

    def inside(x, y):
        return (x - cx) ** 2 + (y - cy) ** 2 <= radius * radius

    return inside


def segment(x0, y0, x1, y1, width):
    """A straight stroke with square-ish ends, as a distance-to-segment test."""

    dx = x1 - x0
    dy = y1 - y0
    length_sq = dx * dx + dy * dy
    half = width / 2.0

    def inside(x, y):
        if length_sq == 0.0:
            t = 0.0
        else:
            t = _clamp(((x - x0) * dx + (y - y0) * dy) / length_sq, 0.0, 1.0)

        nx = x0 + t * dx
        ny = y0 + t * dy
        return (x - nx) ** 2 + (y - ny) ** 2 <= half * half

    return inside


def arc(cx, cy, radius, start_deg, end_deg, width):
    """A stroked arc.

    Angles follow LVGL's own convention -- 0 at three o'clock, growing
    clockwise because y runs down -- so a smile is the bottom of a circle
    placed above the mouth (30..150) and a frown is the top of a circle placed
    below it (210..330).
    """

    half = width / 2.0

    def inside(x, y):
        dx = x - cx
        dy = y - cy
        distance = math.hypot(dx, dy)
        if abs(distance - radius) > half:
            return False

        angle = math.degrees(math.atan2(dy, dx)) % 360.0
        if start_deg <= end_deg:
            return start_deg <= angle <= end_deg

        return angle >= start_deg or angle <= end_deg

    return inside


def eyes():
    return [disc(EYE_X[0], EYE_Y, EYE_RADIUS),
            disc(EYE_X[1], EYE_Y, EYE_RADIUS)]


# The four faces.  "fill" is the disc; "features" are painted in VS_PANEL_BG
# over it, in list order, though none of them overlap.

FACES = [
    {
        "name": "velasight_face_calm_44",
        "label": "neutral / calm -- VS_COLOR_NEUTRAL, level mouth",
        "fill": VS_COLOR_NEUTRAL,
        "features": eyes() + [
            # A level mouth, and nothing above the eyes.  This is the face a
            # session wears before the first cloud reading lands, so it has to
            # be legible as "watching" rather than as any verdict at all.
            segment(15.5, 29.5, 28.5, 29.5, 2.6),
        ],
    },
    {
        "name": "velasight_face_happy_44",
        "label": "happy -- VS_COLOR_HAPPY, upturned mouth",
        "fill": VS_COLOR_HAPPY,
        "features": eyes() + [
            # Bottom of a circle centred at (22,23): the curve runs
            # (13.1,28.6) -> (22,33.5) -> (30.9,28.6), clearing the disc edge
            # by more than 8 px at its lowest point.
            arc(22.0, 23.0, 10.5, 32.0, 148.0, 2.8),
        ],
    },
    {
        "name": "velasight_face_confused_44",
        "label": "confused -- VS_COLOR_CONFUSED, one raised brow, slanted mouth",
        "fill": VS_COLOR_CONFUSED,
        "features": eyes() + [
            # One brow flat and high, the other lower and tilted, with a mouth
            # that is level at neither end.  An asymmetry is what separates
            # this from the neutral face at 44 px; a symmetric "puzzled" mouth
            # alone did not.
            segment(25.5, 11.5, 32.0, 11.5, 2.6),
            segment(12.0, 13.4, 18.5, 12.4, 2.6),
            segment(15.5, 30.0, 28.5, 27.8, 2.6),
        ],
    },
    {
        "name": "velasight_face_tense_44",
        "label": "tense -- VS_COLOR_TENSE, angled brows, downturned mouth",
        "fill": VS_COLOR_TENSE,
        "features": eyes() + [
            # Two brows dropping towards the centre, which is the cue this
            # whole graphic exists for: the red bucket is the one the session
            # raises an alert on, and a colour alone does not say why.  A pair
            # of true diagonals is also the reason these are bitmaps and not
            # composed from LVGL primitives -- lv_obj cannot be rotated.
            segment(12.5, 10.6, 18.6, 13.2, 2.6),
            segment(31.5, 10.6, 25.4, 13.2, 2.6),
            # Top of a circle centred below the mouth: (13.1,31.4) ->
            # (22,26.5) -> (30.9,31.4).
            arc(22.0, 37.0, 10.5, 212.0, 328.0, 2.8),
        ],
    },
]


def render(face):
    """Return SIZE*SIZE (r,g,b) tuples, antialiased and already composited."""

    disc_test = disc(CENTER[0], CENTER[1], DISC_RADIUS)
    fill = face["fill"]
    features = face["features"]
    step = 1.0 / SUPERSAMPLE
    offset = step / 2.0
    pixels = []

    for py in range(SIZE):
        for px in range(SIZE):
            red = green = blue = 0
            for sy in range(SUPERSAMPLE):
                y = py + offset + sy * step
                for sx in range(SUPERSAMPLE):
                    x = px + offset + sx * step
                    if disc_test(x, y) and not any(f(x, y) for f in features):
                        sample = fill
                    else:
                        sample = VS_PANEL_BG

                    red += sample[0]
                    green += sample[1]
                    blue += sample[2]

            total = SUPERSAMPLE * SUPERSAMPLE
            pixels.append(((red + total // 2) // total,
                           (green + total // 2) // total,
                           (blue + total // 2) // total))

    return pixels


def to_rgb565(pixels):
    """Pack to RGB565 half-words.

    The truncating shifts are LVGL's own, copied from scripts/LVGLImage.py's
    RGB565 packer, so these are the same values that tool would have produced
    for the same pixels.  It writes them as a uint8_t array of little-endian
    pairs; this emits uint16_t instead, for alignment.  LVGL hands an
    unconverted RGB565 source straight to lv_draw_sw_blend_to_rgb565(), which
    reads it as uint16_t, and both LV_ATTRIBUTE_MEM_ALIGN and
    LV_ATTRIBUTE_LARGE_CONST expand to nothing in this build -- so a uint8_t
    array would be one-byte aligned with an 88-byte stride, and every row of an
    oddly placed image would be misaligned.  Declaring the element type the
    hardware actually loads makes that unrepresentable rather than merely
    unlikely.
    """

    return [((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
            for red, green, blue in pixels]


def emit_map(name, halfwords):
    # Eleven a line, so one 44 px image row is exactly four lines.
    lines = []
    for start in range(0, len(halfwords), 11):
        chunk = halfwords[start:start + 11]
        lines.append("  " + " ".join(f"0x{value:04x}," for value in chunk))

    return (f"static LV_ATTRIBUTE_LARGE_CONST const uint16_t {name}_map[] =\n"
            "{\n" + "\n".join(lines) + "\n};\n")


def emit_dsc(name):
    return (f"const lv_image_dsc_t {name} =\n"
            "{\n"
            "  .header.magic  = LV_IMAGE_HEADER_MAGIC,\n"
            "  .header.cf     = LV_COLOR_FORMAT_RGB565,\n"
            "  .header.flags  = 0,\n"
            f"  .header.w      = {SIZE},\n"
            f"  .header.h      = {SIZE},\n"
            f"  .header.stride = {SIZE * 2},\n"
            f"  .data_size     = sizeof({name}_map),\n"
            f"  .data          = (const uint8_t *){name}_map,\n"
            "};\n")


def preview(face):
    """Print one face as text, for checking the geometry without a board.

    Ramped on how far the pixel got from the background, so the disc, the
    features cut out of it and the antialiased edges between them are all
    visible: the features come out as background because that is what they are.
    """

    ramp = " .:-=+*#%@"
    pixels = render(face)
    fill = face["fill"]
    span = sum(abs(fill[i] - VS_PANEL_BG[i]) for i in range(3))
    print(f"{face['name']}: {face['label']}")
    for py in range(SIZE):
        row = ""
        for px in range(SIZE):
            pixel = pixels[py * SIZE + px]
            distance = sum(abs(pixel[i] - VS_PANEL_BG[i]) for i in range(3))
            level = 0 if span == 0 else int(distance * (len(ramp) - 1) / span)
            row += ramp[_clamp(level, 0, len(ramp) - 1)]

        print(f"  {row}")

    print()


def main() -> int:
    if "--preview" in sys.argv[1:]:
        for face in FACES:
            preview(face)

        return 0

    root = pathlib.Path(__file__).resolve().parents[1]
    dst = root / "velasight_faces_44.c"
    total = 0
    body = []

    for face in FACES:
        halfwords = to_rgb565(render(face))
        total += len(halfwords) * 2
        body.append(f"/* {face['label']} */\n\n"
                    + emit_map(face["name"], halfwords)
                    + "\n"
                    + emit_dsc(face["name"]))

    text = f"""/****************************************************************************
 * app/velasight/velasight_faces_44.c
 *
 * GENERATED FILE -- do not edit.  Source of truth:
 *   app/velasight/tools/gen_faces.py
 * Regenerate with:
 *   python3 app/velasight/tools/gen_faces.py
 *
 * The social session's four expression faces, {SIZE}x{SIZE} RGB565 with the panel
 * background already composited into every antialiased edge.  RGB565 and no
 * alpha is what lets LVGL copy these straight onto the framebuffer instead of
 * blending them; see the generator for the whole argument.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <lvgl/lvgl.h>

{chr(10).join(body)}"""

    dst.write_text(text, encoding="utf-8")
    print(f"gen_faces: {dst.relative_to(root.parents[1])} "
          f"({len(FACES)} faces, {SIZE}x{SIZE} RGB565, {total} bytes of pixels)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
