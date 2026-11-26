#!/usr/bin/env python3
"""walking_scan.py - find sovietkit's walking-distance sites in a SOVIET64.exe.

    python tools/walking_scan.py  C:\\...\\SovietRepublic\\SOVIET64.exe
    python tools/walking_scan.py  --self-test

Standard library only. It reads the file; it never writes one.

WHAT IT DOES

  1. Prints the build: PE TimeDateStamp, size, SHA-256, sections.

  2. If the stamp is one src/wk_sites.h already has a table for, it checks every
     site the way the plugin does at startup (the instruction shape, the .rdata
     constant its displacement resolves to, and the float stored there) and
     says OK or FAIL for each. Exit code 0 only if all of them hold.

  3. If the stamp is NOT one it knows (a game update), it tries to work out the
     new table:

       * lists every 480 / 530 / 2500 / 2600 float in a non-code section, every
         `movss xmm0,[rip+disp32]` that reads one, and every `mov dword
         [rsp/rbp+disp8], 480.0f` immediate;
       * PREDICTS each site by taking a known build's address and adding the
         shift that region of the executable moved by. The two builds in
         wk_sites.h show that the shift is one number per region (builders and
         collectors, overlay, the save loader's test, the float pool), so one
         unambiguous anchor per region finds the rest;
       * accepts a prediction only if it STRUCTURALLY verifies: the site is the
         right instruction, it resolves to a float, and the float is the right
         value - the same three checks the plugin repeats before it writes.

     and prints the sixteen [sites] keys for sovietkit.ini.

WHAT IT CANNOT DO

  It does not disassemble anything. A predicted site is "the right kind of
  instruction reading the right constant at the offset every earlier build
  predicts", which is strong evidence and not a proof that it is the walking
  distance. The output says PREDICTED, not VERIFIED, for exactly that reason.
  Set `probe = 1` in [walking], start the game and read sovietkit.log, then
  check in game that citizens reach farther. If the scan reports INCOMPLETE,
  the layout moved in a way the shift model does not cover, and the candidate
  lists it prints are what has to be read by hand.
"""

import hashlib
import struct
import sys

# --------------------------------------------------------------------------
# the known tables - keep in step with src/wk_sites.h
# --------------------------------------------------------------------------
KEYS = ["walk_build_stamp", "walk_limit_op", "walk_const", "car_const",
        "walk_radius_const", "car_radius_const", "walk_queued",
        "walk_overlay_1", "walk_overlay_2", "car_build", "car_overlay",
        "walk_rebuild", "car_rebuild", "regen_cmp", "regen_test",
        "save_version"]

PROFILES = {
    "v1.1.1.9": dict(zip(KEYS, [
        0x6A3EB6AD, 0x12E2CD, 0x90AF20, 0x90B104, 0x90AF58, 0x90B108,
        0x12E79F, 0x43F0EA, 0x43F8D5, 0x12F916, 0x440053, 0x12DE97, 0x12F4F2,
        0x438406, 0x438413, 0x9E9C3C])),
    "v1.2.0.8": dict(zip(KEYS, [
        0x6B0097B2, 0x146CBD, 0x952468, 0x95264C, 0x9524A0, 0x952650,
        0x14718F, 0x4636EA, 0x463ED5, 0x148306, 0x464653, 0x146887, 0x147EE2,
        0x45C7E4, 0x45C7F1, 0xA33CFC])),
}

WALK, CAR, WALK_R, CAR_R = 480.0, 2500.0, 530.0, 2600.0

# which constant each movss site reads, and the value that has to be in it
MOVSS = {
    "walk_queued":    ("walk_const",        WALK),
    "walk_overlay_1": ("walk_const",        WALK),
    "walk_overlay_2": ("walk_const",        WALK),
    "car_build":      ("car_const",         CAR),
    "car_overlay":    ("car_const",         CAR),
    "walk_rebuild":   ("walk_radius_const", WALK_R),
    "car_rebuild":    ("car_radius_const",  CAR_R),
}

# regions that moved together, with the site that is the easiest to anchor
CLUSTERS = [
    # builders and collectors; the car rebuild collector's 2600 has one reader
    (["walk_limit_op", "walk_queued", "car_build", "walk_rebuild", "car_rebuild"],
     "car_rebuild"),
    # the overlay
    (["walk_overlay_1", "walk_overlay_2", "car_overlay"], "car_overlay"),
]


# --------------------------------------------------------------------------
# the image
# --------------------------------------------------------------------------
class Image:
    def __init__(self, raw):
        self.raw = raw
        if raw[:2] != b"MZ":
            raise ValueError("not a PE file (no MZ)")
        pe = struct.unpack_from("<I", raw, 0x3C)[0]
        if raw[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file (no PE signature)")
        (machine, nsec, stamp, _, _, optsz, _) = struct.unpack_from("<HHIIIHH", raw, pe + 4)
        opt = pe + 24
        if struct.unpack_from("<H", raw, opt)[0] != 0x20B:
            raise ValueError("not a PE32+ (64-bit) executable")
        self.machine, self.stamp = machine, stamp
        self.size_of_image = struct.unpack_from("<I", raw, opt + 56)[0]
        exc_rva, exc_size = struct.unpack_from("<II", raw, opt + 112 + 3 * 8)
        self.sections = []
        sec = opt + optsz
        for i in range(nsec):
            name, vsize, va, rsize, rptr, _, _, _, _, ch = struct.unpack_from(
                "<8sIIIIIIHHI", raw, sec + i * 40)
            self.sections.append(dict(name=name.rstrip(b"\0").decode("ascii", "replace"),
                                      va=va, vsize=vsize, rptr=rptr, rsize=rsize, ch=ch))
        # flat virtual image, so an RVA is an index
        self.img = bytearray(self.size_of_image)
        for s in self.sections:
            n = min(s["rsize"], s["vsize"]) if s["rsize"] else 0
            self.img[s["va"]:s["va"] + n] = raw[s["rptr"]:s["rptr"] + n]
        self.funcs = []
        if exc_rva and exc_size:
            for i in range(0, exc_size - 11, 12):
                b = struct.unpack_from("<I", self.img, exc_rva + i)[0]
                if b:
                    self.funcs.append(b)
            self.funcs.sort()

    def exec_sections(self):
        return [s for s in self.sections if s["ch"] & 0x20000000]

    def data_sections(self):
        return [s for s in self.sections if not s["ch"] & 0x20000000]

    def u32(self, rva):
        return struct.unpack_from("<I", self.img, rva)[0] if 0 <= rva <= len(self.img) - 4 else None

    def s32(self, rva):
        return struct.unpack_from("<i", self.img, rva)[0] if 0 <= rva <= len(self.img) - 4 else None

    def f32(self, rva):
        return struct.unpack_from("<f", self.img, rva)[0] if 0 <= rva <= len(self.img) - 4 else None

    def in_image(self, rva, n):
        return 0 <= rva and rva + n <= len(self.img)

    def func_of(self, rva):
        lo, hi, best = 0, len(self.funcs) - 1, None
        while lo <= hi:
            mid = (lo + hi) // 2
            if self.funcs[mid] <= rva:
                best, lo = self.funcs[mid], mid + 1
            else:
                hi = mid - 1
        return best

    def find_all(self, pat, sections):
        out = []
        for s in sections:
            start, end = s["va"], s["va"] + s["vsize"]
            i = self.img.find(pat, start, end)
            while i != -1:
                out.append(i)
                i = self.img.find(pat, i + 1, end)
        return out


# --------------------------------------------------------------------------
# the same structural checks mod_walking.inc performs
# --------------------------------------------------------------------------
def imm_oplen(img, rva):
    b = img.img
    if not img.in_image(rva, 8) or b[rva] != 0xC7:
        return 0
    if b[rva + 1] == 0x45:
        return 3
    if b[rva + 1] == 0x44 and b[rva + 2] == 0x24:
        return 4
    return 0


def check_imm(img, rva):
    n = imm_oplen(img, rva)
    if not n:
        return False, "not mov dword [rsp/rbp+disp8],imm32"
    v = img.f32(rva + n)
    return (v == WALK), "immediate is %r" % v


def movss_target(img, rva):
    if not img.in_image(rva, 8) or bytes(img.img[rva:rva + 4]) != b"\xF3\x0F\x10\x05":
        return None
    return rva + 8 + img.s32(rva + 4)


def check_movss(img, rva, const_rva, value):
    t = movss_target(img, rva)
    if t is None:
        return False, "not movss xmm0,[rip+disp32]"
    if const_rva is not None and t != const_rva:
        return False, "reads 0x%X, expected 0x%X" % (t, const_rva)
    v = img.f32(t) if img.in_image(t, 4) else None
    return (v == value), "0x%X holds %r" % (t, v)


def check_regen(img, t):
    c, tt, sv = t["regen_cmp"], t["regen_test"], t["save_version"]
    if not img.in_image(c, 7) or not img.in_image(tt, 4):
        return False, "outside the image"
    if img.img[c] != 0x83 or img.img[c + 1] != 0x3D:
        return False, "regen_cmp is not cmp dword [rip+disp32],imm8"
    if c + 7 + img.s32(c + 2) != sv or img.img[c + 6] != 110:
        return False, "compares 0x%X against %d" % (c + 7 + img.s32(c + 2), img.img[c + 6])
    if bytes(img.img[tt:tt + 4]) != b"\x84\xC0\x0F\x85":
        return False, "regen_test is not test al,al / jnz"
    return True, ""


def verify_table(img, t):
    rows, ok = [], True
    st, why = check_imm(img, t["walk_limit_op"])
    rows.append(("walk_limit_op", t["walk_limit_op"], st, why)); ok &= st
    for key, (ckey, val) in MOVSS.items():
        st, why = check_movss(img, t[key], t[ckey], val)
        rows.append((key, t[key], st, why)); ok &= st
    st, why = check_regen(img, t)
    rows.append(("regen_cmp/test", t["regen_cmp"], st, why)); ok &= st
    return ok, rows


# --------------------------------------------------------------------------
# discovery
# --------------------------------------------------------------------------
def float_bytes(v):
    return struct.pack("<f", v)


def collect(img):
    consts = {}
    for v in (WALK, CAR, WALK_R, CAR_R):
        hits = [i for i in img.find_all(float_bytes(v), img.data_sections()) if i % 4 == 0]
        consts[v] = hits
    refs = {}
    for v, hits in consts.items():
        for h in hits:
            refs[h] = []
    for s in img.exec_sections():
        start, end = s["va"], s["va"] + s["vsize"]
        i = img.img.find(b"\xF3\x0F\x10\x05", start, end)
        while i != -1:
            t = i + 8 + img.s32(i + 4)
            if t in refs:
                refs[t].append(i)
            i = img.img.find(b"\xF3\x0F\x10\x05", i + 1, end)
    imms = []
    for pre in (b"\xC7\x45", b"\xC7\x44\x24"):
        for i in img.find_all(pre, img.exec_sections()):
            n = imm_oplen(img, i)
            if n and img.f32(i + n) == WALK:
                imms.append(i)
    regen = []
    for i in img.find_all(b"\x83\x3D", img.exec_sections()):
        if img.img[i + 6] == 110 and img.in_image(i + 7, 0x30):
            j = img.img.find(b"\x84\xC0\x0F\x85", i + 7, i + 7 + 0x30)
            if j != -1:
                regen.append((i, j))
    return consts, refs, imms, regen


def predict(img, consts, refs, imms, regen):
    """Returns (table or None, notes, per-profile attempts)."""
    notes = []
    best = None
    for pname, ref in PROFILES.items():
        t = {"walk_build_stamp": img.stamp}
        ok = True
        note = ["reference %s" % pname]

        # --- clusters of code sites, each moved by one shift ---------------
        anchor_const = {"car_rebuild": CAR_R, "car_overlay": CAR}
        for sites, anchor in CLUSTERS:
            cands = set()
            for h in consts[anchor_const[anchor]]:
                for r in refs.get(h, []):
                    cands.add(r - ref[anchor])
            good = []
            for d in sorted(cands):
                fine = True
                for s in sites:
                    rva = ref[s] + d
                    if s == "walk_limit_op":
                        fine &= check_imm(img, rva)[0]
                    else:
                        fine &= check_movss(img, rva, None, MOVSS[s][1])[0]
                if fine:
                    good.append(d)
            if len(good) != 1:
                ok = False
                note.append("%s: %d consistent shift(s) from the %s anchor (need exactly 1)"
                            % ("/".join(sites[:2]) + "...", len(good), anchor))
                continue
            note.append("shift %s0x%X for %s" % ("-" if good[0] < 0 else "+", abs(good[0]), ", ".join(sites)))
            for s in sites:
                t[s] = ref[s] + good[0]

        # --- the constants come out of the sites that read them -------------
        if ok:
            for key, (ckey, val) in MOVSS.items():
                tgt = movss_target(img, t[key])
                if ckey in t and t[ckey] != tgt:
                    ok = False
                    note.append("%s and an earlier site disagree on %s (0x%X vs 0x%X)"
                                % (key, ckey, t[ckey], tgt))
                t[ckey] = tgt

        # --- the save loader's regeneration test -----------------------------
        shift_regen = None
        if ok:
            # candidates: every cmp [rip],110 followed by test al,al / jnz whose
            # slot is in a data section; prefer the one nearest the predicted spot
            data_rvas = [(s["va"], s["va"] + s["vsize"]) for s in img.data_sections()]
            cs = [(c, j) for (c, j) in regen
                  if any(a <= c + 7 + img.s32(c + 2) < b for a, b in data_rvas)]
            nearest = sorted(cs, key=lambda cj: abs(cj[0] - (ref["regen_cmp"] + (t["walk_overlay_1"] - ref["walk_overlay_1"]))))
            exact = [cj for cj in cs if cj[1] - cj[0] == ref["regen_test"] - ref["regen_cmp"]]
            pick = None
            if len(exact) == 1:
                pick = exact[0]
            elif nearest and len(cs) <= 3:
                pick = nearest[0]
            if pick:
                t["regen_cmp"], t["regen_test"] = pick
                t["save_version"] = pick[0] + 7 + img.s32(pick[0] + 2)
                note.append("regen: %d candidate(s), took cmp 0x%X / test 0x%X" % (len(cs), pick[0], pick[1]))
            else:
                ok = False
                note.append("regen: %d candidate(s) for `cmp dword [rip],110 ... test al,al / jnz` - cannot pick" % len(cs))

        if ok:
            good, rows = verify_table(img, t)
            ok = good
            if not good:
                note.append("assembled table failed its own structural check")
        notes.append((pname, ok, note))
        if ok and best is None:
            best = t
    return best, notes


def ini_block(t):
    lines = []
    for k in KEYS:
        lines.append("%-17s = 0x%X" % (k, t[k]))
    return "\n".join(lines)


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def describe(img, raw):
    print("PE TimeDateStamp  0x%08X" % img.stamp)
    print("size              %d bytes" % len(raw))
    print("SHA-256           %s" % hashlib.sha256(raw).hexdigest())
    for s in img.sections:
        print("  %-8s rva 0x%06X  vsize 0x%06X%s" % (s["name"], s["va"], s["vsize"],
              "  (code)" if s["ch"] & 0x20000000 else ""))


def run(path):
    raw = open(path, "rb").read()
    img = Image(raw)
    describe(img, raw)
    print()

    for pname, t in PROFILES.items():
        if t["walk_build_stamp"] == img.stamp:
            print("This is %s - the built-in table. Checking every site:" % pname)
            ok, rows = verify_table(img, t)
            for k, rva, st, why in rows:
                print("  %-15s 0x%06X  %s  %s" % (k, rva, "OK  " if st else "FAIL", "" if st else why))
            print("\nALL SITES VERIFY" if ok else "\nSOME SITES DO NOT VERIFY")
            return 0 if ok else 1

    print("No built-in table for stamp 0x%08X (known: %s)." %
          (img.stamp, ", ".join("%s 0x%08X" % (n, p["walk_build_stamp"]) for n, p in PROFILES.items())))
    consts, refs, imms, regen = collect(img)

    print("\nCandidate constants (non-code sections) and the movss instructions reading them:")
    for v, hits in consts.items():
        for h in hits:
            rs = refs.get(h, [])
            print("  %6.0f at 0x%06X   %d reader(s)%s" % (v, h, len(rs),
                  ": " + ", ".join("0x%X" % r for r in rs[:12]) + (" ..." if len(rs) > 12 else "")))
    print("\nmov dword [rsp/rbp+disp8], 480.0f immediates: " +
          (", ".join("0x%X" % i for i in imms) if imms else "none"))
    print("cmp dword [rip],110 followed by test al,al / jnz: " +
          (", ".join("0x%X/0x%X" % cj for cj in regen) if regen else "none"))

    best, notes = predict(img, consts, refs, imms, regen)
    print("\nPrediction from the known builds:")
    for pname, ok, note in notes:
        print("  %s: %s" % (pname, "ok" if ok else "FAILED"))
        for n in note:
            print("      " + n)
    if best:
        print("\nPREDICTED TABLE (structurally verified, not disassembled).")
        print("Paste these sixteen lines under [sites] in sovietkit.ini:\n")
        print(ini_block(best))
        print("\nThen set  [walking] probe = 1,  start the game and read sovietkit.log.")
        return 0
    print("\nINCOMPLETE - the shift model did not resolve every site. Send this output"
          "\nalong with the exe's stamp and SHA-256 above.")
    return 2


# --------------------------------------------------------------------------
# self test: a synthetic PE built from a known table, moved by invented shifts
# --------------------------------------------------------------------------
def build_pe(t, shift_a, shift_b, shift_r, shift_pool, shift_ver, imm_len, stamp, decoy=True):
    TEXT, RDATA, DATA, PDATA = 0x1000, 0x900000, 0xA00000, 0xB00000
    size = 0xB01000
    img = bytearray(size)

    def put_f(rva, v): struct.pack_into("<f", img, rva, v)

    def movss(rva, tgt):
        img[rva:rva + 4] = b"\xF3\x0F\x10\x05"
        struct.pack_into("<i", img, rva + 4, tgt - (rva + 8))

    ref = t
    consts = {k: ref[k] + shift_pool for k in ("walk_const", "car_const", "walk_radius_const", "car_radius_const")}
    put_f(consts["walk_const"], WALK); put_f(consts["car_const"], CAR)
    put_f(consts["walk_radius_const"], WALK_R); put_f(consts["car_radius_const"], CAR_R)
    if decoy:  # unrelated readers of the shared literal pool, as in the real exe
        for i in range(31):
            movss(0x200000 + i * 16, consts["walk_const"])
        for i in range(60):
            movss(0x300000 + i * 16, consts["walk_radius_const"])
    sites = {}
    for k in ("walk_limit_op", "walk_queued", "car_build", "walk_rebuild", "car_rebuild"):
        sites[k] = ref[k] + shift_a
    for k in ("walk_overlay_1", "walk_overlay_2", "car_overlay"):
        sites[k] = ref[k] + shift_b
    if imm_len == 3:
        img[sites["walk_limit_op"]:sites["walk_limit_op"] + 3] = b"\xC7\x45\x80"
    else:
        img[sites["walk_limit_op"]:sites["walk_limit_op"] + 4] = b"\xC7\x44\x24\x7C"
    put_f(sites["walk_limit_op"] + imm_len, WALK)
    for k, (ckey, _) in MOVSS.items():
        movss(sites[k], consts[ckey])
    c = ref["regen_cmp"] + shift_r
    sv = ref["save_version"] + shift_ver
    img[c:c + 2] = b"\x83\x3D"; struct.pack_into("<i", img, c + 2, sv - (c + 7)); img[c + 6] = 110
    tt = ref["regen_test"] + shift_r
    img[tt:tt + 4] = b"\x84\xC0\x0F\x85"

    # headers: flat layout, raw pointer == rva
    pe = 0x80
    img[0:2] = b"MZ"; struct.pack_into("<I", img, 0x3C, pe)
    img[pe:pe + 4] = b"PE\0\0"
    nsec, optsz = 4, 240
    struct.pack_into("<HHIIIHH", img, pe + 4, 0x8664, nsec, stamp, 0, 0, optsz, 0x22)
    opt = pe + 24
    struct.pack_into("<H", img, opt, 0x20B)
    struct.pack_into("<I", img, opt + 56, size)
    pd_rva, pd_size = PDATA, 12 * 4
    struct.pack_into("<II", img, opt + 112 + 3 * 8, pd_rva, pd_size)
    secs = [(b".text", TEXT, RDATA - TEXT, 0x60000020), (b".rdata", RDATA, DATA - RDATA, 0x40000040),
            (b".data", DATA, PDATA - DATA, 0xC0000040), (b".pdata", PDATA, 0x1000, 0x40000040)]
    for i, (n, va, vs, ch) in enumerate(secs):
        struct.pack_into("<8sIIIIIIHHI", img, opt + optsz + i * 40, n, vs, va, vs, va, 0, 0, 0, 0, ch)
    return bytes(img)


def self_test():
    import os
    import tempfile
    fails = 0

    def expect(cond, what):
        nonlocal fails
        print("  %s  %s" % ("ok  " if cond else "FAIL", what))
        fails += 0 if cond else 1

    base = PROFILES["v1.1.1.9"]

    print("synthetic build moved by invented shifts (op length 3):")
    pe = build_pe(base, 0x1230, 0x2340, 0x340, 0x48, 0x5C0, 3, 0x6C000001)
    with tempfile.NamedTemporaryFile(delete=False, suffix=".exe") as f:
        f.write(pe); path = f.name
    img = Image(pe)
    consts, refs, imms, regen = collect(img)
    best, notes = predict(img, consts, refs, imms, regen)
    expect(best is not None, "a table is predicted")
    if best:
        want = dict(base); want.update(
            walk_build_stamp=0x6C000001, walk_limit_op=base["walk_limit_op"] + 0x1230,
            walk_const=base["walk_const"] + 0x48, car_const=base["car_const"] + 0x48,
            walk_radius_const=base["walk_radius_const"] + 0x48, car_radius_const=base["car_radius_const"] + 0x48,
            walk_queued=base["walk_queued"] + 0x1230, car_build=base["car_build"] + 0x1230,
            walk_rebuild=base["walk_rebuild"] + 0x1230, car_rebuild=base["car_rebuild"] + 0x1230,
            walk_overlay_1=base["walk_overlay_1"] + 0x2340, walk_overlay_2=base["walk_overlay_2"] + 0x2340,
            car_overlay=base["car_overlay"] + 0x2340, regen_cmp=base["regen_cmp"] + 0x340,
            regen_test=base["regen_test"] + 0x340, save_version=base["save_version"] + 0x5C0)
        expect(best == want, "every one of the sixteen keys is recovered")
    expect(run(path) == 0, "the command line path prints a table and exits 0")

    print("an exe whose layout the model does not cover:")
    bad = bytearray(pe)
    r = base["walk_queued"] + 0x1230
    bad[r:r + 4] = b"\x90\x90\x90\x90"      # a site that is no longer a movss
    img2 = Image(bytes(bad))
    best2, _ = predict(img2, *collect(img2))
    expect(best2 is None, "refuses to guess (INCOMPLETE)")

    print("a known build verifies against itself:")
    for pname, t in PROFILES.items():
        pe = build_pe(t, 0, 0, 0, 0, 0, 4 if pname == "v1.1.1.9" else 3, t["walk_build_stamp"], decoy=False)
        with tempfile.NamedTemporaryFile(delete=False, suffix=".exe") as f:
            f.write(pe); p = f.name
        expect(run(p) == 0, "%s: all sites verify" % pname)
        os.unlink(p)
    os.unlink(path)
    print("\n%s" % ("ALL OK" if not fails else "%d FAILED" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--self-test":
        sys.exit(self_test())
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(64)
    sys.exit(run(sys.argv[1]))
