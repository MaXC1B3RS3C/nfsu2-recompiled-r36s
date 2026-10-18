#!/usr/bin/env python3
"""HUD-fit test for the widescreen patch.

No game and no Docker run are needed: this re-applies exactly the geometry
src/runtime/widescreen.c computes, so a HUD that is pushed off the edge (or one
that never reaches the widened area) is caught in milliseconds.

Mirrored from widescreen.c:

  nfs_widescreen_set_resolution():
      hud_scale_x  = (1/W * (H/480)) * 2
      s_hud_pos_x  = 640 / (640 * hud_scale_x)          # == 240 * aspect
      s_hud_offset = -((640/2) - (480/2) * aspect)      # == s_hud_pos_x - 320

  widescreen_hud():
      base_offset  = (480*(16/9) - 640) / 2
      offset_x >= 0 : x += s_hud_offset + (offset_x - base_offset)
      offset_x <  0 : x -= s_hud_offset - (offset_x + base_offset)

The HUD is laid out in a virtual space that is 480 px tall and (480*aspect)
wide, with the game centre at s_hud_pos_x.  Every .dat element (name, pos_x,
pos_y, offset_x, offset_y) sits at

      screen_x = s_hud_pos_x + pos_x + move(offset_x)

in that space.  The test asserts:

  * every element anchor stays inside [0, virtual_width]  (nothing clipped);
  * on widened aspects the layout actually reaches the new edges (the left-most
    anchor is within `--edge` px of 0 and the right-most within `--edge` px of
    the right edge), which catches "HUD was not widened at all";
  * the 4:3 layout keeps the original 640-wide framing.

Run:  python3 tests/hud_fit_test.py
"""

import argparse
import os
import sys

DEFAULT_DAT = "ports/r36s/package/widescreen/NFSUnderground2.WidescreenFix.dat"

# (label, width, height) - the virtual width is 480*aspect regardless of the
# real pixel width, but the real sizes are what the port is asked to run at.
RESOLUTIONS = [
    ("4:3  640x480", 640, 480),
    ("16:9 854x480", 854, 480),
    ("16:9 1280x720", 1280, 720),
    ("16:10 1280x800", 1280, 800),
    ("21:9 2560x1080", 2560, 1080),
]


def load_dat(path):
    entries = []
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            p = s.split()
            if len(p) < 5:
                continue
            try:
                entries.append((p[0], float(p[1]), float(p[2]), float(p[3]), float(p[4])))
            except ValueError:
                continue
    return entries


def geometry(width, height):
    aspect = float(width) / float(height)
    hud_scale_x = (1.0 / width * (height / 480.0)) * 2.0
    s_hud_pos_x = 640.0 / (640.0 * hud_scale_x)
    s_hud_offset = -((640.0 / 2.0) - (480.0 / 2.0) * aspect)
    return aspect, s_hud_pos_x, s_hud_offset


def move_x(off_x, s_hud_offset):
    base_offset = (480.0 * (16.0 / 9.0) - 640.0) / 2.0
    if off_x >= 0.0:
        return s_hud_offset + (off_x - base_offset)
    return -(s_hud_offset - (off_x + base_offset))


def measure(entries, width, height):
    aspect, centre, sh = geometry(width, height)
    vw = 2.0 * centre
    placed = [(name, centre + px + move_x(ox, sh)) for name, px, py, ox, oy in entries]
    return aspect, centre, vw, placed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dat", default=DEFAULT_DAT, help="WidescreenFix.dat element table")
    ap.add_argument("--edge", type=float, default=80.0,
                    help="how close the outer anchors must get to the edge on widened aspects")
    ap.add_argument("--verbose", action="store_true", help="print every anchor")
    args = ap.parse_args()

    path = args.dat
    if not os.path.exists(path):
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        path = os.path.join(root, args.dat)
    if not os.path.exists(path):
        print("FAIL: .dat not found (%s)" % args.dat)
        return 2

    entries = load_dat(path)
    if not entries:
        print("FAIL: no HUD entries parsed from %s" % path)
        return 2
    print("HUD table: %s (%d elements)" % (path, len(entries)))

    failures = 0
    for label, w, h in RESOLUTIONS:
        aspect, centre, vw, placed = measure(entries, w, h)
        left = min(placed, key=lambda t: t[1])
        right = max(placed, key=lambda t: t[1])
        inside = [t for t in placed if t[1] < 0.0 or t[1] > vw]
        widened = aspect > 4.0 / 3.0 + 1e-3
        ok = not inside
        if widened:
            ok = ok and left[1] <= args.edge and right[1] >= vw - args.edge
        if aspect <= 4.0 / 3.0 + 1e-3:
            ok = ok and 0.0 <= left[1] and right[1] <= 640.0

        status = "PASS" if ok else "FAIL"
        if not ok:
            failures += 1
        print("  %-15s %s  virtual %.1f wide  centre %.1f  left %.1f (%s)  right %.1f (%s)"
              % (label, status, vw, centre, left[1], left[0], right[1], right[0]))
        for name, x in inside:
            print("      off-screen: %s at x=%.1f" % (name, x))
        if args.verbose:
            for name, x in sorted(placed, key=lambda t: t[1]):
                print("      %7.1f  %s" % (x, name))

    print("%s (%d/%d resolutions failed)"
          % ("ALL PASS" if failures == 0 else "FAILURES", failures, len(RESOLUTIONS)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
