"""Reclassify arena WMOs as exterior, client-side.

Generalises tools/violet_hold/vhr_outdoors.py, which hardcoded Violet Hold's
values. The transformation is the same shape for every arena; only the batch
counts differ, so those are read from the asset rather than assumed.

Four fields must move together or the client culls the geometry - setting the
MOGP exterior bit alone leaves every render batch in the interior range and the
building disappears:

  MOHD.flags        |= 0x8
  MOGI[].flags      |= 0x8, clear 0x2000
  MOGP.flags        |= 0x8, clear 0x2000
  MOGP batch counts  ext += int, int = 0

This is only the client half. The server (and the client's own area lookup)
also consult AreaTable.Flags and WMOAreaTable.Flags, handled by the migrations
sql/custom/dbc/2026_08_06_07_dbc_custom_arenas_outdoors.sql and ..._08_....

Reads each WMO out of the client archives, rewrites it, and writes the result
into patch-Y. Group files are patched too. Several roots can be passed in one
run, which matters because each run copies the 1.2GB patch-Y aside as a backup.

  wmo_outdoors.py <WMO path> [<WMO path> ...]            audit only
  wmo_outdoors.py <WMO path> [<WMO path> ...] --apply    patch into patch-Y

Arena WMO paths live in tools/arenas/arena_geometry.json under each arena's
directory key. Applied so far: ULDUAR_ARENA (Spark of Creator, verified in
game), KARAZHAN_ARENA (Guardian's Hall), 7VS_BLACKROOKHOLD_ARENA,
TB_TOWER (Baradin Hold).
"""
import ctypes
import os
import shutil
import struct
import sys
import time
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mpqwrite

SEP = chr(92)
CLIENT = r"C:\Projects\Gamedev\wow\clients\centurion\Data"
PATCH_Y = os.path.join(CLIENT, "patch-Y.MPQ")

EXTERIOR = 0x00000008
INTERIOR = 0x00002000

APPLY = "--apply" in sys.argv
ROOTS = [a.replace("/", SEP) for a in sys.argv[1:] if a != "--apply"]
if not ROOTS:
    raise SystemExit("usage: wmo_outdoors.py <WORLD\\WMO\\...\\FOO.WMO> [...] [--apply]")

STAMP = time.strftime("%Y%m%d-%H%M%S")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_wmoout")

s = mpqwrite.Storm()
s._set_path_width(s.detect_path_width())
d = s.dll
H, B, DW = wintypes.HANDLE, wintypes.BOOL, wintypes.DWORD
d.SFileOpenFileEx.argtypes = [H, ctypes.c_char_p, DW, ctypes.POINTER(H)]
d.SFileOpenFileEx.restype = B
d.SFileGetFileSize.argtypes = [H, ctypes.POINTER(DW)]
d.SFileGetFileSize.restype = DW
d.SFileReadFile.argtypes = [H, ctypes.c_void_p, DW, ctypes.POINTER(DW), ctypes.c_void_p]
d.SFileReadFile.restype = B
d.SFileCloseFile.argtypes = [H]
d.SFileCloseArchive.argtypes = [H]

archives = []
for f in sorted(os.listdir(CLIENT)):
    if f.lower().endswith(".mpq"):
        h = wintypes.HANDLE()
        if d.SFileOpenArchive(s._path(os.path.join(CLIENT, f)), 0, 0x100, ctypes.byref(h)):
            archives.append((f, h))


def read(name):
    for label, h in archives:
        fh = wintypes.HANDLE()
        if not d.SFileOpenFileEx(h, name.encode("mbcs"), 0, ctypes.byref(fh)):
            continue
        hi = DW(0)
        size = d.SFileGetFileSize(fh, ctypes.byref(hi))
        if size in (0, 0xFFFFFFFF):
            d.SFileCloseFile(fh)
            continue
        buf = ctypes.create_string_buffer(size)
        got = DW(0)
        d.SFileReadFile(fh, buf, size, ctypes.byref(got), None)
        d.SFileCloseFile(fh)
        if got.value:
            return bytearray(buf.raw[:got.value]), label
    return None, None


def chunks(data):
    pos = 0
    while pos + 8 <= len(data):
        magic = data[pos:pos + 4][::-1].decode("ascii", "replace")
        size = struct.unpack_from("<I", data, pos + 4)[0]
        yield magic, pos + 8, size
        pos += 8 + size


changes = []
touched = 0

for ROOT in ROOTS:
    root, src = read(ROOT)
    if root is None:
        raise SystemExit("root WMO not found: %s" % ROOT)
    print("=" * 70)
    print("root %s  (%s, %d bytes)" % (ROOT, src, len(root)))

    ngroups = 0
    for magic, off, size in chunks(root):
        if magic == "MOHD":
            ngroups = struct.unpack_from("<I", root, off + 4)[0]
            fo = off + 0x3C
            cur = struct.unpack_from("<I", root, fo)[0]
            new = cur | EXTERIOR
            print("  MOHD.flags   0x%08X -> 0x%08X   groups=%d" % (cur, new, ngroups))
            if new != cur:
                struct.pack_into("<I", root, fo, new)
                touched += 1
        elif magic == "MOGI":
            for i in range(size // 32):
                fo = off + i * 32
                cur = struct.unpack_from("<I", root, fo)[0]
                new = (cur | EXTERIOR) & ~INTERIOR
                if new != cur:
                    struct.pack_into("<I", root, fo, new)
                    touched += 1
                print("  MOGI[%d]      0x%08X -> 0x%08X%s"
                      % (i, cur, new, "" if new != cur else "   (already ok)"))
    changes.append((ROOT, root))

    base = ROOT[:-4]
    for g in range(ngroups):
        gname = "%s_%03d.wmo" % (base, g)
        gdata, gsrc = read(gname)
        if gdata is None:
            print("  group %-3d MISSING (%s)" % (g, gname))
            continue
        for magic, off, size in chunks(gdata):
            if magic != "MOGP":
                continue
            fo = off + 8
            cur = struct.unpack_from("<I", gdata, fo)[0]
            new = (cur | EXTERIOR) & ~INTERIOR
            t, i_, e = struct.unpack_from("<HHH", gdata, off + 0x28)
            nt, ni, ne = t, 0, e + i_
            note = "" if (new != cur or ni != i_) else "   (already ok)"
            print("  group %-3d flags 0x%08X -> 0x%08X   batches (%d,%d,%d) -> (%d,%d,%d)%s"
                  % (g, cur, new, t, i_, e, nt, ni, ne, note))
            if new != cur or ni != i_:
                touched += 1
            struct.pack_into("<I", gdata, fo, new)
            struct.pack_into("<HHH", gdata, off + 0x28, nt, ni, ne)
            break
        changes.append((gname, gdata))

for _, h in archives:
    d.SFileCloseArchive(h)

print("=" * 70)
print("%d files, %d fields changed" % (len(changes), touched))

if not APPLY:
    print()
    print("audit only - pass --apply to write into patch-Y")
    raise SystemExit

shutil.rmtree(OUT, ignore_errors=True)
for name, data in changes:
    p = os.path.join(OUT, name.replace(SEP, os.sep))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)

bak = PATCH_Y + ".bak-wmoout-" + STAMP
if not os.path.exists(bak):
    shutil.copyfile(PATCH_Y, bak)
print()
print("backup: %s" % os.path.basename(bak))

# Let the writer probe for itself rather than inheriting the reader's width -
# detect_path_width() leaves the binding in the state add() expects, and
# reusing a bare boolean is not equivalent.
s2 = mpqwrite.Storm()
s2._set_path_width(s2.detect_path_width())
h = s2.open(PATCH_Y)
for name, _ in changes:
    s2.add(h, os.path.join(OUT, name.replace(SEP, os.sep)), name)
    print("  written into patch-Y: %s" % name)
s2.close(h)
print()
print("patch-Y now %d bytes" % os.path.getsize(PATCH_Y))
