#!/usr/bin/env python3
"""FMV / loading-screen fit test for the widescreen patch.

No game and no Docker run are needed: this re-applies exactly the geometry
src/runtime/widescreen.c computes (fmv_w2 / fmv_extents / fmv_center_y), so a
4:3 video that is left squashed, stretched or off-centre on a wide screen is
caught in milliseconds.

The four "centred full-screen quad" siblings (sub_005364E0 = EA Games / THX,
sub_005366E0 = NFSU2 title card + loading screens, sub_00536910, sub_00536BF0)
draw a quad at (320,240) with model half-extents (0.5,0.5), through
sub_0051AD40's matrix.  Measured on the game: on a non-4:3 target the whole Y
axis of that matrix is compressed by 1/w^2, w = aspect/(4/3) - the game's own
quad lands as 640x270 with its centre on y=135 at 16:9 instead of 640x480 at
240.  X is untouched (1:1, scaled only by H/480).  The patch therefore scales
the design Y (extents and position) by w^2:

    w2 = (aspect/(4/3))^2
    hx = 0.5            hy = 0.5 * w2
    X  = 240 * aspect   Y  = 240 * w2

and the quad lands as a full-height, centred 4:3 rectangle.  The test asserts,
for each target resolution:

  * the model->screen mapping (Y scaled by 1/w2, X only by H/480) with the
    game's own values reproduces the measured squashed rect - this pins the
    projection model to what the emulator actually did;
  * with the patch's values the quad covers the full height, is horizontally
    centred, and is exactly 4:3 (width = height * 4/3);
  * at 4:3 the patch's values are the game's own 0.5 / 0.5 / 320 / 240, i.e.
    4:3 output is bit-for-bit unchanged.

Run:  python3 tests/fmv_fit_test.py
"""

import argparse
import sys

# (label, width, height).  The R36S is 4:3 or 16:9; the rest guard against
# breaking other targets.
RESOLUTIONS = [
    ("4:3  640x480", 640, 480),
    ("16:9 854x480", 854, 480),
    ("16:9 1280x720", 1280, 720),
    ("16:10 1280x800", 1280, 800),
    ("21:9 2560x1080", 2560, 1080),
]

# The rect the game's own quad (hx=hy=0.5, X=320, Y=240) produces, measured from
# the captures: 640x480 at 4:3, 640x270 top-anchored at 854x480 (y 0..269) and
# 960x405 at 1280x720.  half-height = 480*hy*p, half-width = 640*hx*(H/480).
MEASURED = {
    (640, 480): (640.0, 480.0),
    (854, 480): (640.0, 270.0),
    (1280, 720): (960.0, 405.0),
}

TOL = 1.0


def project(hx, hy, X, Y, width, height):
    """Model quad -> screen rect, using the measured projection.

    Returns (x0, y0, x1, y1) in pixels.
    """
    aspect = float(width) / float(height)
    w = aspect / (4.0 / 3.0)
    p = 1.0 / (w * w)                      # the measured Y compression
    scale = height / 480.0                 # X and Y(design) scale with the height
    half_w = 640.0 * hx * scale
    cx = X * scale
    half_h = 480.0 * hy * p * scale
    cy = Y * p * scale
    return cx - half_w, cy - half_h, cx + half_w, cy + half_h


def patched(width, height):
    """The values widescreen.c writes (fmv_w2 / fmv_extents / fmv_center_y)."""
    aspect = float(width) / float(height)
    w = aspect / (4.0 / 3.0)
    w2 = w * w
    return 0.5, 0.5 * w2, 240.0 * aspect, 240.0 * w2


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    args = ap.parse_args()

    failures = 0
    for label, w, h in RESOLUTIONS:
        hx, hy, X, Y = patched(w, h)
        x0, y0, x1, y1 = project(hx, hy, X, Y, w, h)
        qw, qh = x1 - x0, y1 - y0

        checks = []
        # 1. The projection model reproduces what the emulator actually did.
        if (w, h) in MEASURED:
            gx0, gy0, gx1, gy1 = project(0.5, 0.5, 320.0, 240.0, w, h)
            mw, mh = MEASURED[(w, h)]
            checks.append(("model", abs((gx1 - gx0) - mw) <= TOL and abs((gy1 - gy0) - mh) <= TOL,
                           "game quad %.0fx%.0f vs measured %.0fx%.0f" % (gx1 - gx0, gy1 - gy0, mw, mh)))
        # 2. Patched quad: full height, centred, exactly 4:3.
        checks.append(("height", abs(qh - h) <= TOL, "height %.1f (want %d)" % (qh, h)))
        checks.append(("centre", abs((y0 + y1) / 2.0 - h / 2.0) <= TOL
                       and abs((x0 + x1) / 2.0 - w / 2.0) <= TOL,
                       "centre (%.1f,%.1f) (want %.1f,%.1f)" % ((x0 + x1) / 2.0, (y0 + y1) / 2.0, w / 2.0, h / 2.0)))
        checks.append(("4:3", abs(qw - qh * 4.0 / 3.0) <= TOL, "width %.1f = %.2fx height" % (qw, qw / qh if qh else 0.0)))
        # 3. At 4:3 the patch writes the game's own values.
        if abs(w / float(h) - 4.0 / 3.0) <= 1e-6:
            checks.append(("identity", hx == 0.5 and hy == 0.5 and X == 320.0 and Y == 240.0,
                           "hx/hy/X/Y = %g/%g/%g/%g" % (hx, hy, X, Y)))

        bad = [c for c in checks if not c[1]]
        if bad:
            failures += 1
        print("  %-15s %s  quad %.0fx%.0f at (%.0f,%.0f)  %s"
              % (label, "PASS" if not bad else "FAIL", qw, qh, (x0 + x1) / 2.0, (y0 + y1) / 2.0,
                 "  ".join("%s:%s" % (c[0], "ok" if c[1] else "FAIL(" + c[2] + ")") for c in checks)))

    print("%s (%d/%d resolutions failed)"
          % ("ALL PASS" if failures == 0 else "FAILURES", failures, len(RESOLUTIONS)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
