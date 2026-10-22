#!/usr/bin/env python3
"""Rename lifted function definitions to <name>_orig so a native replacement can take their name.

    wrap_lifted.py <out dir> <func,func,...> <source> [<source> ...]

For every source that defines one of the functions (a line "void sub_XXXXXXXX(void) {"), a copy with only
those definition lines renamed is written to <out dir>/<basename> (rewritten only when its content changes,
so unchanged copies keep their timestamps and objects). Calls stay as they are: callers in any file then
reach the native function, which can still call <name>_orig (kill switch, verify mode).

Prints one "<source>;<copy>" line per wrapped source (for CMake to substitute in its source list). Exits
with an error when a function is defined nowhere, or more than once: then the list no longer matches the
lifted code and the build must not silently skip a replacement.
Same rule as the R36S build (ports/r36s/build_guest_vm.sh, `wrap=` lists).
"""
import os
import re
import sys


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    out_dir, funcs, sources = sys.argv[1], [f for f in sys.argv[2].split(',') if f], sys.argv[3:]
    found = {f: [] for f in funcs}
    os.makedirs(out_dir, exist_ok=True)
    for src in sources:
        with open(src, encoding='utf-8', errors='surrogateescape') as fh:
            text = fh.read()
        hit = [f for f in funcs if re.search(r'^void %s\(void\) \{$' % f, text, re.M)]
        if not hit:
            continue
        for f in hit:
            found[f].append(src)
            text = re.sub(r'^void %s\(void\) \{$' % f, 'void %s_orig(void) {' % f, text, flags=re.M)
        dst = os.path.join(out_dir, os.path.basename(src))
        old = None
        if os.path.exists(dst):
            with open(dst, encoding='utf-8', errors='surrogateescape') as fh:
                old = fh.read()
        if old != text:
            with open(dst, 'w', encoding='utf-8', errors='surrogateescape') as fh:
                fh.write(text)
        print('%s;%s' % (src, dst))
    bad = {f: s for f, s in found.items() if len(s) != 1}
    if bad:
        for f, s in bad.items():
            sys.stderr.write('wrap_lifted: %s defined %d times %s\n' % (f, len(s), s))
        sys.exit(1)


if __name__ == '__main__':
    main()
