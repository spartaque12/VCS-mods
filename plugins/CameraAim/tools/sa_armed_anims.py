#!/usr/bin/env python3
"""San Andreas' armed idle / walk / run holds for VCS (CameraAim.asi's "SA armed anims", docs: game-internals.md 12w).

    python sa_armed_anims.py "<GTA SA>/anim/ped.ifp" [out]     (out: <VCS>/build/mods/anims/sa_armed.bin)

Reads the stock ped.ifp (ANP3) of the user's own GTA San Andreas and writes the upper body (spine1, neck, head, both
arms: bones 3, 4, 5, 21-24, 31-34) of IDLE_armed, WALK_armed, run_armed and WALK_start_armed - the anims VCS plays
for a ped holding a two-handed gun (the same names in VCS's resident ped anims). The root, pelvis, spine (its legs'
parent) and legs stay VCS's own, so the steps, speed and footing are unchanged. No game data is in this script; its
output is the user's own converted data (never commit it).

The bones are the same Biped ids with the same frames in both games (tools/anim_convert.py in the port's tree, 12i), so
SA's local rotations are used as they are. Times are kept as fractions of the anim (the mod stretches them to VCS's
durations, its root track drives the movement). Each anim is written at several thinning tolerances: the mod rewrites
VCS's keys in place (the resident anims have no room to grow) with the finest level that fits.

File (little endian): "VSPA", u32 version 1, u32 anim count; per anim: name[24], f32 SA duration, u32 level count; per
level: f32 tolerance (degrees), u32 track count; per track: u16 bone, u16 key count, keys (f32 time 0..1, f32 x y z w).
"""
import math
import os
import struct
import sys

ANIMS = ["IDLE_armed", "WALK_armed", "run_armed", "WALK_start_armed"]
BONES = [3, 4, 5, 31, 32, 33, 34, 21, 22, 23, 24]
TOLERANCES = [0.4, 0.8, 1.5, 2.5, 4.0, 6.0]


def read_ifp(path):
    """ANP3 -> {name lower: (name, {bone: [(t s, (x, y, z, w))]})}"""
    d = open(path, "rb").read()
    if d[:4] != b"ANP3":
        raise SystemExit("%s: not an ANP3 IFP (GTA SA's ped.ifp)" % path)
    p, anims = 36, {}
    for _ in range(struct.unpack_from("<I", d, 32)[0]):
        name = d[p:p + 24].split(b"\0")[0].decode("latin1")
        nn, _fsz, comp = struct.unpack_from("<III", d, p + 24)
        p += 36
        bones = {}
        for _ in range(nn):
            ft, nf, bid = struct.unpack_from("<III", d, p + 24)
            p += 36
            rot_only = ft in (1, 3)
            keys = []
            for _ in range(nf):
                if comp:
                    if rot_only:
                        v = struct.unpack_from("<5h", d, p); p += 10
                    else:
                        v = struct.unpack_from("<8h", d, p); p += 16
                    keys.append((v[4] / 60.0, tuple(x / 4096.0 for x in v[0:4])))
                else:
                    if rot_only:
                        v = struct.unpack_from("<5f", d, p); p += 20
                    else:
                        v = struct.unpack_from("<8f", d, p); p += 32
                    keys.append((v[4], tuple(v[0:4])))
            bones.setdefault(bid, keys)
        anims[name.lower()] = (name, bones)
    return anims


def qnorm(q):
    n = math.sqrt(sum(x * x for x in q)) or 1.0
    return tuple(x / n for x in q)


def slerp(a, b, t):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0:
        b, d = tuple(-x for x in b), -d
    if d > 0.9995:
        return qnorm(tuple(x + (y - x) * t for x, y in zip(a, b)))
    th = math.acos(min(1.0, d))
    s = math.sin(th)
    return tuple(math.sin((1 - t) * th) / s * x + math.sin(t * th) / s * y for x, y in zip(a, b))


def angle(a, b):
    return math.degrees(2 * math.acos(min(1.0, abs(sum(x * y for x, y in zip(a, b))))))


def thin(keys, tol):
    """drop keys a slerp of their kept neighbours reproduces within tol degrees"""
    if len(keys) <= 2:
        return keys
    kept, i = [0], 0
    while i < len(keys) - 1:
        j = i + 2
        while j < len(keys):
            t0, q0 = keys[i]
            t1, q1 = keys[j]
            if any(angle(slerp(q0, q1, (keys[k][0] - t0) / (t1 - t0) if t1 > t0 else 0.0), keys[k][1]) > tol
                   for k in range(i + 1, j)):
                break
            j += 1
        kept.append(j - 1)
        i = j - 1
    return [keys[k] for k in kept]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    here = os.path.dirname(os.path.abspath(__file__))
    vcs = os.environ.get("VCS_RECOMP_DIR", os.path.normpath(os.path.join(here, "..", "..", "..", "..")))
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(vcs, "build", "mods", "anims", "sa_armed.bin")
    sa = read_ifp(sys.argv[1])
    b = bytearray(b"VSPA" + struct.pack("<II", 1, len(ANIMS)))
    for name in ANIMS:
        if name.lower() not in sa:
            raise SystemExit("%s has no %s" % (sys.argv[1], name))
        sname, bones = sa[name.lower()]
        dur = max(k[-1][0] for k in bones.values() if k)
        b += sname.encode("latin1")[:23].ljust(24, b"\0") + struct.pack("<fI", dur, len(TOLERANCES))
        tracks = {}
        for bone in BONES:
            keys, prev = [], None
            for t, q in bones.get(bone, []):
                q = qnorm(q)
                if prev is not None and sum(x * y for x, y in zip(prev, q)) < 0:
                    q = tuple(-x for x in q)  # the short way round
                prev = q
                keys.append((t / dur if dur > 0 else 0.0, q))
            if keys:
                if keys[0][0] > 0:
                    keys.insert(0, (0.0, keys[0][1]))
                if keys[-1][0] < 1.0:
                    keys.append((1.0, keys[-1][1]))
                tracks[bone] = keys
        sizes = []
        for tol in TOLERANCES:
            b += struct.pack("<fI", tol, len(tracks))
            n = 0
            for bone, keys in tracks.items():
                ks = thin(keys, tol)
                n += len(ks)
                b += struct.pack("<HH", bone, len(ks))
                for t, q in ks:
                    b += struct.pack("<5f", t, *q)
            sizes.append(n)
        print("%-18s %.3f s, %d tracks, keys per level %s" % (sname, dur, len(tracks), sizes))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(b)
    print("->", out, len(b), "bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
