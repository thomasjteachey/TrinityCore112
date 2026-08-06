"""Read the arena rows back out of the binary DBCs and check them.

Appending to a DBC means appending to its string block and pointing the new
record at the offset. Get that wrong and the file still has a valid header and
the right record count -- it just returns garbage strings, or reads past the end
of the block. So the rows are read back and compared against what was intended
rather than trusting that the write said it worked.

Usage: python verify_arena_dbc.py <dbc-dir>
Exit status is non-zero if anything does not match.
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gen_arena_sql import (ARENAS, AREA_IDS, PVPDIFF_BASE,  # noqa: E402
                           WSL_BASE, WSUI_BASE, measured, start_positions)


class Dbc:
    def __init__(self, path):
        with open(path, "rb") as fh:
            self.b = fh.read()
        magic, self.count, self.fields, self.recsize, self.strsize = \
            struct.unpack_from("<4sIIII", self.b, 0)
        if magic != b"WDBC":
            raise ValueError("%s is not WDBC" % path)
        self.rec0 = 20
        self.str0 = 20 + self.count * self.recsize

    def i(self, rec, f):
        return struct.unpack_from("<i", self.b, self.rec0 + rec * self.recsize + f * 4)[0]

    def f(self, rec, fl):
        return struct.unpack_from("<f", self.b, self.rec0 + rec * self.recsize + fl * 4)[0]

    def s(self, rec, f):
        off = struct.unpack_from("<I", self.b, self.rec0 + rec * self.recsize + f * 4)[0]
        if off == 0:
            return ""
        if off >= self.strsize:
            raise ValueError("string offset %d past end of %d-byte block" % (off, self.strsize))
        end = self.b.index(b"\x00", self.str0 + off)
        return self.b[self.str0 + off:end].decode("utf-8", "replace")

    def find(self, wanted_id):
        for r in range(self.count):
            if self.i(r, 0) == wanted_id:
                return r
        return None


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    d = sys.argv[1]
    bad = []

    m = Dbc(os.path.join(d, "Map.dbc"))
    bml = Dbc(os.path.join(d, "BattlemasterList.dbc"))
    pvp = Dbc(os.path.join(d, "PvpDifficulty.dbc"))
    wsl = Dbc(os.path.join(d, "WorldSafeLocs.dbc"))
    wsui = Dbc(os.path.join(d, "WorldStateUI.dbc"))
    area = None
    area_path = os.path.join(d, "AreaTable.dbc")
    if os.path.exists(area_path):
        area = Dbc(area_path)

    print("%-5s %-5s %-20s %-34s %-9s %-16s %s"
          % ("BG", "MAP", "DIRECTORY", "NAME", "BRACKETS", "STARTS", "AREAS"))
    print("-" * 118)

    for i, (bg, mid, directory, name, cx, cy, cz, conf, ev) in enumerate(ARENAS):
        # Map.dbc
        r = m.find(mid)
        if r is None:
            bad.append("Map.dbc missing id %d" % mid)
            continue
        got_dir = m.s(r, 1)
        got_name = m.s(r, 5)
        inst = m.i(r, 2)
        if got_dir != directory:
            bad.append("Map %d Directory %r != %r" % (mid, got_dir, directory))
        if got_name != name:
            bad.append("Map %d name %r != %r" % (mid, got_name, name))
        if inst != 4:
            bad.append("Map %d InstanceType %d != 4 (arena)" % (mid, inst))

        # BattlemasterList.dbc
        rb = bml.find(bg)
        if rb is None:
            bad.append("BattlemasterList missing id %d" % bg)
        else:
            if bml.i(rb, 1) != mid:
                bad.append("BattlemasterList %d MapID_1 %d != %d" % (bg, bml.i(rb, 1), mid))
            if bml.i(rb, 9) != 4:
                bad.append("BattlemasterList %d InstanceType %d != 4" % (bg, bml.i(rb, 9)))
            if bml.s(rb, 11) != name:
                bad.append("BattlemasterList %d name %r != %r" % (bg, bml.s(rb, 11), name))

        # PvpDifficulty.dbc -- the silent queue killer, so count them
        brackets = sum(1 for r2 in range(pvp.count) if pvp.i(r2, 1) == mid)
        if brackets != 16:
            bad.append("PvpDifficulty for map %d has %d brackets, want 16" % (mid, brackets))

        # WorldSafeLocs.dbc -- compared against start_positions(), which is the
        # same function the writers use, so surveyed arenas are checked against
        # their surveyed values and derived ones against the derived formula.
        starts = []
        for k, (ex, ey, ez, _eo) in enumerate(start_positions(bg, cx, cy, cz)):
            rw = wsl.find(WSL_BASE + i * 2 + k)
            if rw is None:
                bad.append("WorldSafeLocs missing id %d" % (WSL_BASE + i * 2 + k))
                continue
            if wsl.i(rw, 1) != mid:
                bad.append("WorldSafeLocs %d Continent %d != %d" % (WSL_BASE + i * 2 + k, wsl.i(rw, 1), mid))
            gx, gy, gz = wsl.f(rw, 2), wsl.f(rw, 3), wsl.f(rw, 4)
            if abs(gx - ex) > 0.05 or abs(gy - ey) > 0.05 or abs(gz - ez) > 0.05:
                bad.append("WorldSafeLocs %d pos (%.2f,%.2f,%.2f) != (%.2f,%.2f,%.2f)" % (
                    WSL_BASE + i * 2 + k, gx, gy, gz, ex, ey, ez))
            starts.append("%.0f" % gx)

        # WorldStateUI.dbc -- two rows, both gated on state 3610
        for k in (0, 1):
            ru = wsui.find(WSUI_BASE + i * 2 + k)
            if ru is None:
                bad.append("WorldStateUI missing id %d" % (WSUI_BASE + i * 2 + k))
                continue
            if wsui.i(ru, 1) != mid:
                bad.append("WorldStateUI %d MapID %d != %d" % (WSUI_BASE + i * 2 + k, wsui.i(ru, 1), mid))
            txt = wsui.s(ru, 5)
            if "Players Remaining" not in txt:
                bad.append("WorldStateUI %d text %r unexpected" % (WSUI_BASE + i * 2 + k, txt))
            if wsui.i(ru, 39) != 3610:
                bad.append("WorldStateUI %d StateVariable %d != 3610" % (WSUI_BASE + i * 2 + k, wsui.i(ru, 39)))

        # AreaTable.dbc -- the rows that make the arena name itself rather than
        # showing whatever zone the player came from.
        area_note = "-"
        if area is not None:
            want = AREA_IDS.get(bg, [])
            found = []
            for aid in want:
                ra = area.find(aid)
                if ra is None:
                    bad.append("AreaTable missing id %d for map %d" % (aid, mid))
                    continue
                if area.i(ra, 1) != mid:
                    bad.append("AreaTable %d ContinentID %d != %d" % (aid, area.i(ra, 1), mid))
                if area.s(ra, 11) != name:
                    bad.append("AreaTable %d name %r != %r" % (aid, area.s(ra, 11), name))
                found.append(aid)
            area_note = ",".join(str(x) for x in found) if found else ("none" if want else "-")

            # Map.dbc should point at the first of them. AreaTableID is field
            # 22, not 6: fields 5-20 are the 16 MapName locale slots and 21 is
            # their mask, so the binary layout is much wider than the SQL
            # mirror's column list makes it look.
            if want and m.i(r, 22) != want[0]:
                bad.append("Map %d AreaTableID %d != %d" % (mid, m.i(r, 22), want[0]))

        print("%-5d %-5d %-20s %-34s %-9d %-16s %s" % (
            bg, mid, got_dir[:20], got_name[:34], brackets, " / ".join(starts), area_note))

    print()
    if bad:
        print("FAILED -- %d problems:" % len(bad))
        for b in bad:
            print("   %s" % b)
        sys.exit(1)
    print("OK -- all %d arenas verified in Map, BattlemasterList, PvpDifficulty, "
          "WorldSafeLocs, WorldStateUI%s" % (len(ARENAS), " and AreaTable" if area else ""))


if __name__ == "__main__":
    main()
