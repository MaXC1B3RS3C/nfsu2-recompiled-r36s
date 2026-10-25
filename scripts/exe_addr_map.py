#!/usr/bin/env python3
"""Map guest addresses between two builds of SPEED2.EXE (developer tool).

The European v1.2 retail build and the v1.0 build the port was first lifted from are
the same code (v1.2 = v1.0 plus the patch changes) compiled and linked separately:
section addresses are identical, but functions sit at different offsets (the
shift varies through .text, some functions moved or were folded) and a few
data items moved by a few bytes. Hand-written code that names guest functions
or addresses (native replacements, hooks, overrides, call sites) therefore
needs one address per exe. This script derives the map; nothing here ships
game bytes, only addresses.

Evidence, strongest first:
  1. Aligned instruction streams: both .text sections disassembled with
     objdump, constants in the image normalised away, then diffed (GNU diff).
     Every aligned pair maps one instruction address to the other.
  2. Constant pairs inside aligned instructions: call/jmp targets and data
     operands give the counterpart of the address they name, including
     functions that moved out of order and data that shifted.
  3. Pointer slots in .rdata/.data: a dword slot that holds a code pointer in
     both exes (vtables, callback tables) pairs those two functions.
  4. Functions found only through 2/3 are aligned locally from their start.

Usage:
  exe_addr_map.py REF.exe TARGET.exe --query 0x005BF2E0 0x0075BC00 ...
  exe_addr_map.py REF.exe TARGET.exe --query-file addrs.txt [--json out.json]
  exe_addr_map.py REF.exe TARGET.exe --stats

Requires GNU binutils objdump (pei-i386) and GNU diff on PATH.
"""
import argparse, collections, json, os, re, struct, subprocess, sys, tempfile

IMAGE_BASE = 0x400000
HEX = re.compile(r'0x([0-9a-f]+)')


class Pe:
    def __init__(self, path):
        self.path = path
        self.b = open(path, 'rb').read()
        o = struct.unpack_from('<I', self.b, 0x3c)[0]
        n = struct.unpack_from('<H', self.b, o + 6)[0]
        so = o + 24 + struct.unpack_from('<H', self.b, o + 20)[0]
        self.secs = []
        for i in range(n):
            name, vs, va, rs, ro = struct.unpack_from('<8sIIII', self.b, so + 40 * i)
            self.secs.append((name.rstrip(b'\0'), vs, va, rs, ro))
        t = self.secs[0]
        self.text_lo = IMAGE_BASE + t[2]
        self.text_hi = IMAGE_BASE + t[2] + max(t[1], t[3])
        self.img_hi = IMAGE_BASE + max(s[2] + max(s[1], s[3]) for s in self.secs)

    def sec_bytes(self, i):
        name, vs, va, rs, ro = self.secs[i]
        return IMAGE_BASE + va, self.b[ro:ro + rs]

    def is_code(self, v):
        return self.text_lo <= v < self.text_hi

    def is_image(self, v):
        return self.text_lo <= v < self.img_hi


def disassemble(pe):
    """[(addr, normalised text, [raw image constants])] for .text, linear sweep."""
    out = subprocess.run(['objdump', '-d', '-M', 'intel', '--no-show-raw-insn', '-j', '.text', pe.path],
                         check=True, capture_output=True, text=True).stdout
    res = []
    for line in out.splitlines():
        if not line.startswith('  ') or ':\t' not in line:
            continue
        a, ins = line.split(':\t', 1)
        ins = re.sub(r'\s+<[^>]*>', '', ins.strip())
        ins = re.sub(r'\s+', ' ', ins)
        consts = []

        def norm(m):
            v = int(m.group(1), 16)
            if pe.is_code(v):
                consts.append(v); return 'C'
            if pe.is_image(v):
                consts.append(v); return 'D'
            return m.group(0)
        res.append((int(a, 16), HEX.sub(norm, ins), consts))
    return res


def gnu_diff(a_lines, b_lines):
    """Yield (op, a0, a1, b0, b1) hunks, 0-based half-open ranges."""
    with tempfile.TemporaryDirectory() as d:
        fa, fb = os.path.join(d, 'a'), os.path.join(d, 'b')
        with open(fa, 'w') as f: f.write('\n'.join(a_lines) + '\n')
        with open(fb, 'w') as f: f.write('\n'.join(b_lines) + '\n')
        out = subprocess.run(['diff', fa, fb], capture_output=True, text=True).stdout
    hunk = re.compile(r'^(\d+)(?:,(\d+))?([acd])(\d+)(?:,(\d+))?$')
    for line in out.splitlines():
        m = hunk.match(line)
        if not m:
            continue
        a0 = int(m.group(1)); a1 = int(m.group(2) or a0)
        op = m.group(3)
        b0 = int(m.group(4)); b1 = int(m.group(5) or b0)
        if op == 'a':   # after line a0 of A, B lines b0..b1 added
            yield op, a0, a0, b0 - 1, b1
        elif op == 'd':  # A lines a0..a1 deleted, after line b0 of B
            yield op, a0 - 1, a1, b0, b0
        else:
            yield op, a0 - 1, a1, b0 - 1, b1


class AddrMap:
    def __init__(self, ref_path, tgt_path, log=sys.stderr):
        self.log = log
        self.ref, self.tgt = Pe(ref_path), Pe(tgt_path)
        self.A, self.B = disassemble(self.ref), disassemble(self.tgt)
        self.a_index = {a: i for i, (a, _, _) in enumerate(self.A)}
        self.b_index = {a: i for i, (a, _, _) in enumerate(self.B)}
        self.ins = {}                                # ref insn addr -> tgt insn addr
        self.ins_weak = {}                           # same, from equal-length changed hunks
        self.votes = collections.defaultdict(collections.Counter)   # ref const -> tgt const votes
        self._align()
        self._pointer_slots()
        self._extend_moved()
        self._resolve_consts()

    def _pair(self, i, j, weak=False):
        a, na, ca = self.A[i]; b, nb, cb = self.B[j]
        (self.ins_weak if weak else self.ins)[a] = b
        if not weak and len(ca) == len(cb):      # changed instructions give no reliable operand pairs
            for x, y in zip(ca, cb):
                self.votes[x][y] += 1

    def _align(self):
        an = [n for _, n, _ in self.A]; bn = [n for _, n, _ in self.B]
        i = j = 0; hunks = 0
        for op, a0, a1, b0, b1 in gnu_diff(an, bn):
            hunks += 1
            while i < a0 and j < b0:
                self._pair(i, j); i += 1; j += 1
            if op == 'c' and a1 - a0 == b1 - b0:
                for k in range(a1 - a0):
                    if an[a0 + k].split(' ')[0] == bn[b0 + k].split(' ')[0]:
                        self._pair(a0 + k, b0 + k, weak=True)
            i, j = a1, b1
        while i < len(an) and j < len(bn):
            self._pair(i, j); i += 1; j += 1
        self.hunks = hunks
        print(f"aligned {len(self.ins)} instructions ({len(self.ins_weak)} more in changed hunks), "
              f"{hunks} diff hunks; ref {len(self.A)} / target {len(self.B)} instructions", file=self.log)

    def _pointer_slots(self):
        n = 0
        for i in (1, 2):                             # .rdata, .data
            base, a = self.ref.sec_bytes(i); _, b = self.tgt.sec_bytes(i)
            for k in range(0, min(len(a), len(b)) - 3, 4):
                x = struct.unpack_from('<I', a, k)[0]; y = struct.unpack_from('<I', b, k)[0]
                if self.ref.is_code(x) and self.tgt.is_code(y):
                    self.votes[x][y] += 4; n += 1
        print(f"{n} code-pointer slots paired in .rdata/.data", file=self.log)

    def _walk(self, ra, ta):
        """Align instruction streams from a known pair until they diverge."""
        i = self.a_index.get(ra); j = self.b_index.get(ta); n = 0
        if i is None or j is None:
            return 0
        while i < len(self.A) and j < len(self.B) and self.A[i][1] == self.B[j][1]:
            if self.A[i][0] in self.ins:
                break
            self._pair(i, j); n += 1
            i += 1; j += 1
        return n

    def _extend_moved(self):
        added = 0
        for _ in range(4):
            new = 0
            for x, c in list(self.votes.items()):
                if not self.ref.is_code(x) or x in self.ins:
                    continue
                y, _ = c.most_common(1)[0]
                new += self._walk(x, y)
            added += new
            if not new:
                break
        print(f"{added} instructions aligned from moved functions", file=self.log)

    def _resolve_consts(self):
        self.const = {}
        self.conflicts = 0
        for x, c in self.votes.items():
            (y, n), *rest = c.most_common(2) + [(None, 0)]
            if rest and rest[0][1] and rest[0][1] * 4 >= n:
                self.conflicts += 1
            self.const[x] = y

    # ---- queries ----
    def lookup(self, va):
        """(target va or None, how, confidence note)."""
        if va in self.ins:
            if va in self.const and self.const[va] != self.ins[va]:
                # Call targets and pointer slots name a function start directly; the diff can pair the
                # wrong instructions next to padding or a function present in one build only.
                return self.const[va], 'operand/pointer', f'instruction alignment said {self.ins[va]:#010x}'
            return self.ins[va], 'insn', ''
        if va in self.ins_weak:
            return self.ins_weak[va], 'insn-changed-hunk', 'instruction differs between builds'
        if va in self.const:
            return self.const[va], 'operand/pointer', ''
        if self.ref.is_code(va):
            # inside an instruction or unreferenced: nearest aligned instruction before it
            i = self._floor(va)
            if i is not None:
                a = self.A[i][0]
                if a in self.ins:
                    return self.ins[a] + (va - a), 'nearest-insn', f'{va - a:+#x} from {a:#010x}'
            return None, 'unmapped', ''
        # data: identity unless some operand evidence moved it nearby
        d = self._data_shift(va)
        return va + d, 'data', ('' if d == 0 else f'shifted {d:+#x} like nearby operands')

    def _floor(self, va):
        lo, hi = 0, len(self.A) - 1
        while lo <= hi:
            m = (lo + hi) // 2
            if self.A[m][0] <= va: lo = m + 1
            else: hi = m - 1
        return hi if hi >= 0 else None

    def _data_shift(self, va):
        if va in self.const:
            return self.const[va] - va
        best = None
        for x, y in self.const.items():
            if self.ref.is_code(x) or abs(x - va) > 0x40:
                continue
            if best is None or abs(x - va) < abs(best[0] - va):
                best = (x, y - x)
        return best[1] if best else 0


def parse_addrs(tokens):
    for t in tokens:
        t = t.strip()
        if not t or t.startswith('#'):
            continue
        t = t.split()[0]
        t = t[4:] if t.startswith('sub_') else t
        yield int(t, 16)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ref'); ap.add_argument('target')
    ap.add_argument('--query', nargs='*', default=[])
    ap.add_argument('--query-file')
    ap.add_argument('--json')
    ap.add_argument('--stats', action='store_true')
    args = ap.parse_args()
    m = AddrMap(args.ref, args.target)
    addrs = list(parse_addrs(args.query))
    if args.query_file:
        addrs += list(parse_addrs(open(args.query_file)))
    res = {}
    for va in addrs:
        y, how, note = m.lookup(va)
        res[f'{va:#010x}'] = {'target': None if y is None else f'{y:#010x}', 'how': how, 'note': note}
        print(f'{va:#010x} -> {("%#010x" % y) if y is not None else "????????"}  {how}  {note}')
    if args.stats:
        print(f'constant conflicts: {m.conflicts}', file=sys.stderr)
    if args.json:
        json.dump(res, open(args.json, 'w'), indent=1)


if __name__ == '__main__':
    main()
