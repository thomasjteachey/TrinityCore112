"""Clone the cave's server terrain from map 0 to map 1685 (Blood Beneath the Arena).

  python3 bba_clone_server.py <data dir> [--apply]

Copies the 3x3 block of grids around the cave (gx 55-57, gy 30-32; the cave itself is
grid 56/31 = ADT Azeroth_31_56) so the grids a player's visibility pulls in next to the cave
exist too:
  maps/000GGYY.map      -> maps/1685GGYY.map      (area grid rewritten to 30609)
  vmaps/000_YY_GG.vmtile -> vmaps/1685_YY_GG.vmtile (byte copy)
  mmaps/000GGYY.mmtile  -> mmaps/1685GGYY.mmtile  (byte copy)
  vmaps/000.vmtree      -> vmaps/1685.vmtree      (byte copy)
  mmaps/000.mmap        -> mmaps/1685.mmap        (byte copy)
None of these formats embed the map id (Tanaris 1620 / OBC 1615 were cloned the same way).

The area rewrite matters: a byte-copied .map keeps map 0's baked area ids (2177 Battle Ring
carries AREA_FLAG_ARENA -> FFA + the Gurubashi ring rules), so the dungeon must report its own
zone. Existing destination files are never overwritten. Writes go to a temp name, then rename.
"""
import os, struct, sys, shutil

SRC, DST = '000', '1685'
AREA = 30609
GX = (55, 56, 57)
GY = (30, 31, 32)


def rewrite_area(blob):
    hdr = struct.unpack_from('<4sI4s8I', blob, 0)
    magic, version, build, area_off, area_size = hdr[0], hdr[1], hdr[2], hdr[3], hdr[4]
    if magic != b'MAPS':
        raise ValueError('not a MAPS file')
    out = bytearray(blob)
    fourcc, flags, grid = struct.unpack_from('<IHH', blob, area_off)
    if fourcc != struct.unpack('<I', b'AREA')[0]:
        raise ValueError('no AREA header at %d' % area_off)
    old = {grid}
    struct.pack_into('<H', out, area_off + 6, AREA)
    n = 0
    if not flags & 0x0001:
        for i in range(256):
            o = area_off + 8 + i * 2
            old.add(struct.unpack_from('<H', blob, o)[0])
            struct.pack_into('<H', out, o, AREA)
            n += 1
    return bytes(out), flags, sorted(old), n


def put(path, data, apply):
    if os.path.exists(path):
        return 'exists'
    if apply:
        tmp = os.path.join(os.path.dirname(path), '.bba_tmp_' + os.path.basename(path))
        with open(tmp, 'wb') as f:
            f.write(data)
        os.rename(tmp, path)
    return 'written' if apply else 'would write'


def main():
    root = sys.argv[1]
    apply = '--apply' in sys.argv
    for gx in GX:
        for gy in GY:
            src = os.path.join(root, 'maps', '%s%02d%02d.map' % (SRC, gx, gy))
            dst = os.path.join(root, 'maps', '%s%02d%02d.map' % (DST, gx, gy))
            if os.path.exists(src):
                data, flags, old, n = rewrite_area(open(src, 'rb').read())
                print('%-50s areas %s -> %d (flags %d, %d cells)  %s' % (dst, old, AREA, flags, n, put(dst, data, apply)))
            for d, s, t in (('vmaps', '%s_%02d_%02d.vmtile' % (SRC, gy, gx), '%s_%02d_%02d.vmtile' % (DST, gy, gx)),
                            ('mmaps', '%s%02d%02d.mmtile' % (SRC, gx, gy), '%s%02d%02d.mmtile' % (DST, gx, gy))):
                sp = os.path.join(root, d, s)
                if os.path.exists(sp):
                    print('%-50s %s' % (os.path.join(root, d, t), put(os.path.join(root, d, t), open(sp, 'rb').read(), apply)))
    for d, s, t in (('vmaps', SRC + '.vmtree', DST + '.vmtree'), ('mmaps', SRC + '.mmap', DST + '.mmap')):
        sp = os.path.join(root, d, s)
        print('%-50s %s' % (os.path.join(root, d, t), put(os.path.join(root, d, t), open(sp, 'rb').read(), apply)))


if __name__ == '__main__':
    main()
