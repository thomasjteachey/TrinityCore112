"""Audit and build Violet Hold's client-side outdoor classification.

There is no ADT/MCNK "outside" flag in the 3.3.5 format.  Terrain chunks
carry an AreaTable id, and that AreaTable row carries INSIDE/OUTSIDE.  WMO
interiors are classified separately by the group MOGP flags, then may be
overridden by WMOAreaTable.dbc.  A coherent outdoor Violet Hold therefore has:

* every DalaranPrison ADT MCNK resolving to an AreaTable row flagged OUTSIDE;
* the client WMO root and group flagged exterior, with its render-batch counts
  reclassified from 16 interior / 0 exterior to 0 interior / 16 exterior;
* WMOAreaTable rows for root id 5282 with OUTSIDE set and INSIDE cleared.

The WMO fields are coupled rendering/culling metadata. Setting MOGP EXTERIOR
alone is invalid for this asset: the root remains indoor and all 16 MOBA batches
remain in the interior range, so the client culls the hold. The safe client
override changes MOHD, MOGI, MOGP, and the three MOGP batch counts together.
The server VMAP remains stock; map 1608's server override controls gameplay.

How the 3.3.5a (12340) client actually consumes these flags -- reverse
engineered 2026-08-15 after the first build (MOGI 0x2008 / MOGP 0x380D)
made the hold's water unswimmable:

* Position query (CWorld, 0x7C2A70/0x7C1DC0): the vertical ray hit against a
  group records "inside a WMO group" only when that group's MOGP flags lack
  EXTERIOR (0x8).  An exterior-flagged group never counts as "inside".
* CGUnit_C::IsOutdoors (0x71B7F0 -> 0x77FBF0): inside a group -> WMOAreaTable
  row (WMOID, NameSet, group uniqueID) Flags & 4 = outdoors, else indoors;
  not inside -> AreaTable(terrain MCNK area) OUTSIDE/INSIDE, default outdoors.
  This is the mount gate (Spell_C checks ATTR0 INDOORS_ONLY/OUTDOORS_ONLY
  0x4000/0x8000 through it).
* Liquid query (0x7A1BC0): inside a group -> only that group's MLIQ is
  consulted.  NOT inside -> WMO liquid is still consulted (0x7A09D0 ->
  0x7AEB90), but only for groups whose *MOGI* flags lack INTERIOR (0x2000)
  and whose MOGI bounding box contains the point; then terrain liquid.
  (This is how Stormwind's canals swim: exterior groups with MLIQ.)

So MOGP EXTERIOR alone flips the query to the "not inside" branch, and MOGI
INTERIOR then hides the group's water from the outdoor liquid path: water
renders (rendering gathers group liquids independently) but you walk along
the pool floor without swimming while the server -- whose VMAP is stock --
still counts you as under water.  The fix is CLIENT_MOGI_FLAGS below:
EXTERIOR only, INTERIOR cleared.  MOGP keeps INTERIOR|EXTERIOR: the culling
keys on MOGI EXTERIOR being present (that is why MOGP-EXTERIOR-alone culled
the hold), MOGP INTERIOR has no bearing on the liquid path, and leaving it
preserves the lighting the hold shipped with.

The DBC half is maintained by vhr_dbc.py.  This script audits the ADTs and
the client/server WMO flags using the scriptable StormLib wrapper.
"""

from __future__ import annotations

import argparse
import shutil
import struct
import tempfile
from collections import Counter
from pathlib import Path

from storm_mpq import MPQArchive


MAP_DIR_TOKEN = "world\\maps\\dalaranprison\\"
WMO_DIR_TOKEN = "world\\wmo\\dungeon\\nd_dalaranprison\\"
DBC_PREFIX = "DBFilesClient\\"
WMO_ROOT_ID = 5282
MOGP_EXTERIOR = 0x00000008
MOGP_INTERIOR = 0x00002000
STOCK_MOGP_FLAGS = 0x00003805
CLIENT_MOGP_FLAGS = STOCK_MOGP_FLAGS | MOGP_EXTERIOR
STOCK_MOGI_FLAGS = 0x00002000
# EXTERIOR only. The first build kept INTERIOR here (0x2008); the client's
# outdoor liquid path skips any group whose MOGI flags carry INTERIOR, which
# is what made the hold's pool unswimmable. See the module docstring.
CLIENT_MOGI_FLAGS = MOGP_EXTERIOR
BROKEN_WATER_MOGI_FLAGS = STOCK_MOGI_FLAGS | MOGP_EXTERIOR
STOCK_MOHD_FLAGS = 0x00000005
CLIENT_MOHD_FLAGS = STOCK_MOHD_FLAGS | 0x00000008
STOCK_BATCH_COUNTS = (0, 16, 0)
CLIENT_BATCH_COUNTS = (0, 0, 16)
MCNK_AREA_ID_OFFSET = 0x34
VMAP_MAGIC = b"VMAP_4.8"


def iter_chunks(data: bytes | bytearray, start: int = 0, end: int | None = None):
    end = len(data) if end is None else end
    pos = start
    while pos + 8 <= end:
        magic = data[pos:pos + 4][::-1].decode("ascii", "replace")
        size = struct.unpack_from("<I", data, pos + 4)[0]
        payload = pos + 8
        if payload + size > end:
            raise ValueError(f"truncated {magic} chunk at {pos:#x}")
        yield magic, payload, size
        pos = payload + size


def find_chunk(data: bytes | bytearray, wanted: str):
    for magic, payload, size in iter_chunks(data):
        if magic == wanted:
            return payload, size
    raise ValueError(f"{wanted} chunk not found")


def audit_adt(data: bytes) -> tuple[Counter, Counter, Counter, Counter]:
    areas, mcnk_flags, modf_flags, name_sets = Counter(), Counter(), Counter(), Counter()
    for magic, payload, size in iter_chunks(data):
        if magic == "MCNK":
            if size < 128:
                raise ValueError(f"short MCNK header: {size}")
            mcnk_flags[struct.unpack_from("<I", data, payload)[0]] += 1
            areas[struct.unpack_from("<I", data, payload + MCNK_AREA_ID_OFFSET)[0]] += 1
        elif magic == "MODF":
            if size % 64:
                raise ValueError(f"MODF size is not a multiple of 64: {size}")
            for base in range(payload, payload + size, 64):
                flags, _doodad_set, name_set, _scale = struct.unpack_from("<4H", data, base + 56)
                modf_flags[flags] += 1
                name_sets[name_set] += 1
    return areas, mcnk_flags, modf_flags, name_sets


def mogp_flags(data: bytes | bytearray) -> int:
    payload, size = find_chunk(data, "MOGP")
    if size < 68:
        raise ValueError(f"short MOGP header: {size}")
    return struct.unpack_from("<I", data, payload + 8)[0]


def client_root_header(data: bytes | bytearray) -> tuple[int, int]:
    """Return (MOHD flags, first MOGI group flags)."""
    mohd, mohd_size = find_chunk(data, "MOHD")
    mogi, mogi_size = find_chunk(data, "MOGI")
    if mohd_size != 64 or mogi_size < 32:
        raise ValueError(f"unexpected MOHD/MOGI sizes: {mohd_size}/{mogi_size}")
    if struct.unpack_from("<I", data, mohd + 4)[0] != 1:
        raise ValueError("DalaranPrison root no longer has exactly one group")
    root_id = struct.unpack_from("<I", data, mohd + 32)[0]
    if root_id != WMO_ROOT_ID:
        raise ValueError(f"unexpected MOHD WMO id: {root_id}")
    return (struct.unpack_from("<I", data, mohd + 60)[0],
            struct.unpack_from("<I", data, mogi)[0])


def client_group_header(data: bytes | bytearray) -> tuple[int, tuple[int, int, int], int]:
    """Return (MOGP flags, batch counts A/interior/exterior, group id)."""
    payload, size = find_chunk(data, "MOGP")
    if size < 68:
        raise ValueError(f"short MOGP header: {size}")
    return (struct.unpack_from("<I", data, payload + 8)[0],
            struct.unpack_from("<3H", data, payload + 40),
            struct.unpack_from("<I", data, payload + 56)[0])


def force_client_wmo_outdoors(root: bytes, group: bytes) -> tuple[bytes, bytes]:
    """Patch every coupled client WMO outdoor/render field with strict guards."""
    root_flags, mogi_flags = client_root_header(root)
    group_flags, batches, group_id = client_group_header(group)
    expected = (STOCK_MOHD_FLAGS, STOCK_MOGI_FLAGS, STOCK_MOGP_FLAGS,
                STOCK_BATCH_COUNTS, 25154)
    actual = (root_flags, mogi_flags, group_flags, batches, group_id)
    if actual != expected:
        raise ValueError(f"refusing to patch unexpected stock WMO header: {actual!r}")

    patched_root = bytearray(root)
    mohd, _ = find_chunk(patched_root, "MOHD")
    mogi, _ = find_chunk(patched_root, "MOGI")
    struct.pack_into("<I", patched_root, mohd + 60, CLIENT_MOHD_FLAGS)
    struct.pack_into("<I", patched_root, mogi, CLIENT_MOGI_FLAGS)

    patched_group = bytearray(group)
    mogp, _ = find_chunk(patched_group, "MOGP")
    struct.pack_into("<I", patched_group, mogp + 8, CLIENT_MOGP_FLAGS)
    struct.pack_into("<3H", patched_group, mogp + 40, *CLIENT_BATCH_COUNTS)
    return bytes(patched_root), bytes(patched_group)


def verify_client_wmo(root: bytes, group: bytes) -> None:
    root_flags, mogi_flags = client_root_header(root)
    group_flags, batches, group_id = client_group_header(group)
    print(f"client root: MOHD={root_flags:#010x} MOGI={mogi_flags:#010x}")
    print(f"client group: id={group_id} MOGP={group_flags:#010x} "
          f"batches(A/interior/exterior)={batches}")
    expected = (CLIENT_MOHD_FLAGS, CLIENT_MOGI_FLAGS, CLIENT_MOGP_FLAGS,
                CLIENT_BATCH_COUNTS, 25154)
    actual = (root_flags, mogi_flags, group_flags, batches, group_id)
    if actual != expected:
        raise SystemExit(f"client WMO outdoor/render audit failed: {actual!r}")


def vmo_group_header(data: bytes | bytearray) -> tuple[int, int, int, int]:
    """Return (root id, group count, group id, MOGP flags) for this one-group WMO."""
    if len(data) < 60 or data[:8] != VMAP_MAGIC or data[8:12] != b"WMOD":
        raise ValueError("not a Trinity VMAP 4.8 world model")
    root_id = struct.unpack_from("<I", data, 16)[0]
    if data[20:24] != b"GMOD":
        raise ValueError("VMO has no GMOD chunk")
    group_count = struct.unpack_from("<I", data, 24)[0]
    if root_id != WMO_ROOT_ID or group_count != 1:
        raise ValueError(f"expected root {WMO_ROOT_ID} with one group, got {root_id}/{group_count}")
    # GroupModel starts at 28: AABox (two Vector3s, 24 bytes), flags, group id.
    flags = struct.unpack_from("<I", data, 52)[0]
    group_id = struct.unpack_from("<I", data, 56)[0]
    return root_id, group_count, group_id, flags


def audit_vmo(path: Path) -> None:
    root_id, groups, group_id, flags = vmo_group_header(path.read_bytes())
    exterior = bool(flags & MOGP_EXTERIOR)
    interior = bool(flags & MOGP_INTERIOR)
    print(f"{path}: root={root_id} groups={groups} group={group_id} "
          f"flags={flags:#010x} exterior={exterior} interior={interior}")
    if flags != STOCK_MOGP_FLAGS or exterior or not interior:
        raise SystemExit(
            f"VMO must retain stock render flags {STOCK_MOGP_FLAGS:#010x}; "
            "outdoor gameplay belongs in AreaTable/WMOAreaTable/map logic")


def dbc_records(data: bytes) -> dict[int, tuple[int, ...]]:
    magic, count, fields, record_size, _string_size = struct.unpack_from("<4sIIII", data)
    if magic != b"WDBC" or record_size != fields * 4:
        raise ValueError("invalid or unsupported WDBC header")
    rows = {}
    for index in range(count):
        offset = 20 + index * record_size
        row = struct.unpack_from(f"<{fields}I", data, offset)
        rows[row[0]] = row
    return rows


def audit_patch_dbc(mpq: Path) -> None:
    with MPQArchive(mpq) as archive:
        area = dbc_records(archive.read(DBC_PREFIX + "AreaTable.dbc"))
        wmo = dbc_records(archive.read(DBC_PREFIX + "WMOAreaTable.dbc"))
        maps = dbc_records(archive.read(DBC_PREFIX + "Map.dbc"))
        battlemasters = dbc_records(archive.read(DBC_PREFIX + "BattlemasterList.dbc"))
        brackets = dbc_records(archive.read(DBC_PREFIX + "PvpDifficulty.dbc"))
        world_states = dbc_records(archive.read(DBC_PREFIX + "WorldStateUI.dbc"))

    problems = []
    for area_id in (4415, 30608):
        flags = area.get(area_id, (0, 0, 0, 0, 0))[4]
        outside = bool(flags & 0x04000000)
        inside = bool(flags & 0x02000000)
        print(f"AreaTable {area_id}: flags={flags:#010x} outside={outside} inside={inside}")
        if not outside or inside:
            problems.append(f"AreaTable {area_id} is not unambiguously outside")

    wmo_rows = [row for row in wmo.values() if row[1] == WMO_ROOT_ID]
    print(f"WMOAreaTable root {WMO_ROOT_ID}: {len(wmo_rows)} rows")
    if not wmo_rows:
        problems.append(f"no WMOAreaTable rows for root {WMO_ROOT_ID}")
    for row in sorted(wmo_rows):
        flags = row[9]
        outside = bool(flags & 4)
        inside = bool(flags & 2)
        print(f"  id={row[0]} group={row[3]} flags={flags:#x} "
              f"area={row[10]} outside={outside} inside={inside}")
        if not outside or inside or row[10] != 4415:
            problems.append(
                f"WMOAreaTable {row[0]} must be outside and resolve to AreaTable 4415")

    queue_checks = {
        "Map 1608": 1608 in maps,
        "BattlemasterList 105": 105 in battlemasters and battlemasters[105][1] == 1608,
        "PvpDifficulty 91608": 91608 in brackets and brackets[91608][1] == 1608,
        "WorldStateUI 90025-90027": all(i in world_states for i in (90025, 90026, 90027)),
    }
    for label, okay in queue_checks.items():
        print(f"{label}: {'OK' if okay else 'MISSING'}")
        if not okay:
            problems.append(label)
    if problems:
        raise SystemExit("patch audit failed: " + "; ".join(problems))


def relevant_names(archive: MPQArchive) -> tuple[list[str], list[str], list[str]]:
    names = archive.names()
    adts = sorted(n for n in names if MAP_DIR_TOKEN in n.lower() and n.lower().endswith(".adt"))
    roots = sorted(n for n in names if WMO_DIR_TOKEN in n.lower() and n.lower().endswith("dalaranprison.wmo"))
    groups = sorted(n for n in names if WMO_DIR_TOKEN in n.lower()
                    and n.lower().endswith(".wmo") and n not in roots)
    return adts, roots, groups


def audit_assets(mpq: Path) -> None:
    with MPQArchive(mpq) as archive:
        adts, roots, groups = relevant_names(archive)
        if not adts or not roots or not groups:
            raise SystemExit(
                f"{mpq}: expected DalaranPrison ADTs/root/groups, found "
                f"{len(adts)}/{len(roots)}/{len(groups)}")

        areas, mcnk, modf, namesets = Counter(), Counter(), Counter(), Counter()
        for name in adts:
            a, m, f, n = audit_adt(archive.read(name))
            areas.update(a)
            mcnk.update(m)
            modf.update(f)
            namesets.update(n)

        print(f"archive: {mpq}")
        print(f"ADTs: {len(adts)}")
        print(f"MCNK AreaTable ids: {dict(sorted(areas.items()))}")
        print(f"MCNK flags (not indoor/outdoor): {dict(sorted(mcnk.items()))}")
        print(f"MODF placement flags (not indoor/outdoor): {dict(sorted(modf.items()))}")
        print(f"MODF WMOAreaTable name sets: {dict(sorted(namesets.items()))}")
        print(f"WMO roots: {roots}")
        for name in roots:
            root_flags, mogi_flags = client_root_header(archive.read(name))
            print(f"WMO root {name}: MOHD={root_flags:#010x} MOGI={mogi_flags:#010x}")
        for name in groups:
            flags, batches, group_id = client_group_header(archive.read(name))
            print(f"WMO group {name}: flags={flags:#010x} "
                  f"group={group_id} batches={batches} "
                  f"exterior={bool(flags & MOGP_EXTERIOR)} "
                  f"interior={bool(flags & MOGP_INTERIOR)}")


def client_wmo_names(archive: MPQArchive) -> tuple[str, str]:
    _adts, roots, groups = relevant_names(archive)
    if len(roots) != 1 or len(groups) != 1:
        raise SystemExit(f"expected one DalaranPrison root/group, got {roots}/{groups}")
    return roots[0], groups[0]


def audit_client_wmo(mpq: Path) -> None:
    with MPQArchive(mpq) as archive:
        root_name, group_name = client_wmo_names(archive)
        verify_client_wmo(archive.read(root_name), archive.read(group_name))


def fix_client_mogi(root: bytes) -> bytes:
    """Clear INTERIOR from the already-built root's MOGI flags (0x2008 -> 0x8).

    Idempotent: a root that already carries CLIENT_MOGI_FLAGS is returned
    unchanged. Anything else (stock, or an unknown edit) is refused so this
    can never be run against the wrong asset.
    """
    root_flags, mogi_flags = client_root_header(root)
    if root_flags != CLIENT_MOHD_FLAGS:
        raise ValueError(f"refusing: MOHD flags {root_flags:#x} are not the built "
                         f"client value {CLIENT_MOHD_FLAGS:#x} (run build-client-wmo first)")
    if mogi_flags == CLIENT_MOGI_FLAGS:
        return root
    if mogi_flags != BROKEN_WATER_MOGI_FLAGS:
        raise ValueError(f"refusing: unexpected MOGI flags {mogi_flags:#x}")
    patched = bytearray(root)
    mogi, _ = find_chunk(patched, "MOGI")
    struct.pack_into("<I", patched, mogi, CLIENT_MOGI_FLAGS)
    return bytes(patched)


def fix_client_mogi_in_mpq(mpq: Path, backup: Path | None) -> None:
    """Rewrite only the DalaranPrison root inside an existing (built) patch MPQ."""
    with MPQArchive(mpq) as archive:
        root_name, group_name = client_wmo_names(archive)
        root = archive.read(root_name)
        group = archive.read(group_name)
    fixed = fix_client_mogi(root)
    if fixed == root:
        print(f"{mpq}: MOGI already {CLIENT_MOGI_FLAGS:#x}, nothing to do")
        verify_client_wmo(root, group)
        return
    if backup is not None:
        backup.write_bytes(root)
        print(f"original root saved to {backup}")
    verify_client_wmo(fixed, group)
    with tempfile.TemporaryDirectory(prefix="vhr-client-mogi-") as temporary:
        root_file = Path(temporary) / "DalaranPrison.wmo"
        root_file.write_bytes(fixed)
        with MPQArchive(mpq, writable=True) as patch:
            patch.add(root_file, root_name)
            patch.flush()
    audit_client_wmo(mpq)
    print(f"{mpq}: MOGI {BROKEN_WATER_MOGI_FLAGS:#x} -> {CLIENT_MOGI_FLAGS:#x} written and re-verified")


def build_client_wmo(source_assets_mpq: Path, base_patch_mpq: Path,
                     output_patch_mpq: Path) -> None:
    if source_assets_mpq.resolve() == output_patch_mpq.resolve():
        raise SystemExit("source asset MPQ and output patch MPQ must differ")
    shutil.copy2(base_patch_mpq, output_patch_mpq)
    with MPQArchive(source_assets_mpq) as source:
        root_name, group_name = client_wmo_names(source)
        root, group = force_client_wmo_outdoors(
            source.read(root_name), source.read(group_name))
    verify_client_wmo(root, group)

    with tempfile.TemporaryDirectory(prefix="vhr-client-wmo-") as temporary:
        root_file = Path(temporary) / "DalaranPrison.wmo"
        group_file = Path(temporary) / "DalaranPrison_000.wmo"
        root_file.write_bytes(root)
        group_file.write_bytes(group)
        with MPQArchive(output_patch_mpq, writable=True) as patch:
            patch.add(root_file, root_name)
            patch.add(group_file, group_name)
            patch.flush()
    audit_client_wmo(output_patch_mpq)


def main() -> None:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    audit = sub.add_parser("audit-assets")
    audit.add_argument("mpq", type=Path)
    audit_patch = sub.add_parser("audit-patch")
    audit_patch.add_argument("mpq", type=Path)
    audit_server_vmo = sub.add_parser("audit-vmo")
    audit_server_vmo.add_argument("vmo", type=Path)
    audit_client = sub.add_parser("audit-client-wmo")
    audit_client.add_argument("mpq", type=Path)
    build_client = sub.add_parser("build-client-wmo")
    build_client.add_argument("source_assets_mpq", type=Path)
    build_client.add_argument("base_patch_mpq", type=Path)
    build_client.add_argument("output_patch_mpq", type=Path)
    fix_mogi = sub.add_parser(
        "fix-client-mogi",
        help="in an already-built patch MPQ, clear INTERIOR from the root MOGI "
             "flags so the client's outdoor liquid path sees the hold's water")
    fix_mogi.add_argument("patch_mpq", type=Path)
    fix_mogi.add_argument("--backup-root", type=Path, default=None,
                          help="where to save the original DalaranPrison.wmo before rewriting")
    args = parser.parse_args()

    if args.command == "audit-assets":
        audit_assets(args.mpq)
    elif args.command == "audit-patch":
        audit_patch_dbc(args.mpq)
    elif args.command == "audit-vmo":
        audit_vmo(args.vmo)
    elif args.command == "audit-client-wmo":
        audit_client_wmo(args.mpq)
    elif args.command == "fix-client-mogi":
        fix_client_mogi_in_mpq(args.patch_mpq, args.backup_root)
    else:
        build_client_wmo(args.source_assets_mpq, args.base_patch_mpq,
                         args.output_patch_mpq)


if __name__ == "__main__":
    main()
