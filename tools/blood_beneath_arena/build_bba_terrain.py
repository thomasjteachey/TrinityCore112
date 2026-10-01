"""Build the client terrain for map 1685 (Blood Beneath the Arena).

The cave was built inside Stranglethorn's ADT Azeroth_31_56 (map 0). This lifts that single
tile into a private map directory so the dungeon is its own map:

    <out>/World/Maps/BloodBeneathArena/BloodBeneathArena.wdt
    <out>/World/Maps/BloodBeneathArena/BloodBeneathArena_31_56.adt

* Source = the highest-priority archive holding each file (patch-X, the art base, which was
  checked CRC-identical to the published patch-Y.zip).
* WDT: Azeroth's, with MAIN flags cleared for every tile except (31,56).
* ADT: byte copy except every MCNK's areaId (MCNK data offset 0x34) is rewritten to the
  dungeon's own AreaTable row, so the client's zone text / PvP rules read
  "Blood Beneath the Arena" instead of Gurubashi Arena / Battle Ring.
"""
import os, struct, sys, hashlib, glob
sys.path.insert(0, r'C:\Projects\Gamedev\wow\tools\mpqpy')
from mpqread import MPQ

DATA = r'C:\Projects\Gamedev\wow\clients\centurion\Data'
OUT = sys.argv[1] if len(sys.argv) > 1 else r'C:\Projects\Gamedev\wow\data\patch-staging\BloodBeneathArena'
SRC, DST = 'Azeroth', 'BloodBeneathArena'
TILE = (31, 56)          # ADT <Map>_<x>_<y>; MAIN index [y][x]
AREA_ID = 30609


def rank(path):
    name = os.path.basename(path)[:-4]
    low = name.lower()
    base = ['common', 'common-2', 'expansion', 'lichking', 'locale-enus', 'patch', 'patch-enus']
    if low in base:
        return (0, base.index(low))
    c = name[-1].upper()
    return (1, (10 + int(c)) if c.isdigit() else (20 + ord(c) - 64))


def resolve(rel):
    arcs = sorted(glob.glob(DATA + r'\*.MPQ'), key=rank)
    hit = None
    for a in arcs:
        try:
            m = MPQ(a)
        except Exception:
            continue
        if m.find(rel) is not None:
            hit = (a, m)
    if not hit:
        raise SystemExit('not found: ' + rel)
    return os.path.basename(hit[0]), hit[1].extract(rel)


def chunks(blob):
    pos = 0
    while pos + 8 <= len(blob):
        magic = blob[pos:pos + 4][::-1].decode('latin1')
        size = struct.unpack_from('<I', blob, pos + 4)[0]
        yield magic, pos + 8, size
        pos += 8 + size


def rewrite_wdt(blob):
    out = bytearray(blob)
    for magic, body, size in chunks(blob):
        if magic == 'MAIN':
            assert size == 64 * 64 * 8
            kept = cleared = 0
            for y in range(64):
                for x in range(64):
                    off = body + (y * 64 + x) * 8
                    fl = struct.unpack_from('<I', blob, off)[0]
                    if not fl & 1:
                        continue
                    if (x, y) == TILE:
                        kept += 1
                    else:
                        struct.pack_into('<I', out, off, fl & ~1)
                        cleared += 1
            assert kept == 1, 'tile %s not flagged in source WDT' % (TILE,)
            print('  WDT MAIN: kept %d, cleared %d' % (kept, cleared))
            return bytes(out)
    raise SystemExit('no MAIN in WDT')


def rewrite_adt(blob):
    out = bytearray(blob)
    n = 0
    old = {}
    names = []
    for magic, body, size in chunks(blob):
        if magic == 'MCNK':
            a = struct.unpack_from('<I', blob, body + 0x34)[0]
            old[a] = old.get(a, 0) + 1
            struct.pack_into('<I', out, body + 0x34, AREA_ID)
            n += 1
        elif magic in ('MMDX', 'MWMO'):
            names += [s.decode('latin1') for s in blob[body:body + size].split(b'\0') if s]
    assert n == 256, 'expected 256 MCNK, found %d' % n
    print('  ADT: rewrote %d MCNK areaIds (was %s) -> %d' % (n, old, AREA_ID))
    bt = [s for s in names if '8tr_' in s.lower() or '8und_' in s.lower()]
    wmo = [s for s in names if s.lower().endswith('.wmo')]
    print('  ADT references %d models (%d BfA troll/cave models), WMOs: %s' % (len(names), len(bt), wmo))
    return bytes(out)


def main():
    dest = os.path.join(OUT, 'World', 'Maps', DST)
    os.makedirs(dest, exist_ok=True)
    src_wdt = 'World\\Maps\\%s\\%s.wdt' % (SRC, SRC)
    src_adt = 'World\\Maps\\%s\\%s_%d_%d.adt' % (SRC, SRC, TILE[0], TILE[1])
    a1, wdt = resolve(src_wdt)
    a2, adt = resolve(src_adt)
    print('WDT from %s (%d B), ADT from %s (%d B)' % (a1, len(wdt), a2, len(adt)))
    for name, data in (('%s.wdt' % DST, rewrite_wdt(wdt)),
                       ('%s_%d_%d.adt' % (DST, TILE[0], TILE[1]), rewrite_adt(adt))):
        p = os.path.join(dest, name)
        open(p, 'wb').write(data)
        print('  wrote %s  %d B  md5 %s' % (p, len(data), hashlib.md5(data).hexdigest()))


if __name__ == '__main__':
    main()
