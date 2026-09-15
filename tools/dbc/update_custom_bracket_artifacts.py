"""Build local PvpDifficulty and patch-enUS-8 artifacts for custom BG brackets.

This is deliberately an artifact builder: it reads the existing client/server
copies, replaces only the rows for Scarlet Chapel (1189), Blackrock Throne
(1230), and Violet Hold (1608), and preserves every other record/file.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import struct
import subprocess
import zipfile
from pathlib import Path


ROOT = Path(r"C:\Projects\Gamedev\wow")
SERVER_DBC = ROOT / "data" / "dbc" / "lplus" / "PvpDifficulty.dbc"
CLIENT_ZIP = ROOT / "clients" / "centurion" / "Data" / "enUS" / "patch-enUS-8.zip"
MPQTOOL = ROOT / "tools" / "mpqtool" / "mpqtool.exe"
OUT = Path(__file__).resolve().parents[2] / "Build" / "artifacts"

BRACKETS = {
    1189: [(91189, 0), (93229, 1), (93230, 2), (93231, 3), (93232, 4), (93233, 5)],
    1230: [(91230, 0), (93234, 1), (93235, 2), (93236, 3), (93237, 4), (93238, 5)],
    1608: [(91608, 0), (93224, 1), (93225, 2), (93226, 3), (93227, 4), (93228, 5)],
}


def rewrite_pvp(blob: bytes) -> bytes:
    magic, count, fields, rec_size, str_size = struct.unpack_from("<4sIIII", blob)
    if magic != b"WDBC" or fields != 6 or rec_size != 24:
        raise ValueError("unexpected PvpDifficulty.dbc header")
    records = [list(struct.unpack_from("<6i", blob, 20 + i * rec_size))
               for i in range(count)]
    wanted_maps = set(BRACKETS)
    kept = [row for row in records if row[1] not in wanted_maps]
    used_ids = {row[0] for row in kept}
    new_rows = []
    for map_id, specs in BRACKETS.items():
        for row_id, index in specs:
            if row_id in used_ids:
                raise ValueError(f"new id {row_id} already exists outside map {map_id}")
            new_rows.append([row_id, map_id, index, 10 + index * 10, 19 + index * 10, 0])
    kept.extend(new_rows)
    records_blob = b"".join(struct.pack("<6i", *row) for row in kept)
    strings = blob[20 + count * rec_size:]
    if len(strings) != str_size:
        raise ValueError("string block length mismatch")
    return struct.pack("<4sIIII", b"WDBC", len(kept), fields, rec_size, str_size) + records_blob + strings


def rows(blob: bytes) -> list[tuple[int, int, int, int, int, int]]:
    _magic, count, _fields, rec_size, _str_size = struct.unpack_from("<4sIIII", blob)
    return [struct.unpack_from("<6i", blob, 20 + i * rec_size) for i in range(count)]


def run(*args: str) -> None:
    subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def main() -> None:
    out_dbc = OUT / "dbc" / "lplus" / "PvpDifficulty.dbc"
    out_zip = OUT / "client" / "patch-enUS-8.zip"
    work = OUT / "_patch8_work"
    mpq = work / "patch-enUS-8.MPQ"
    extracted = work / "files"
    extracted.mkdir(parents=True, exist_ok=True)
    out_dbc.parent.mkdir(parents=True, exist_ok=True)
    out_zip.parent.mkdir(parents=True, exist_ok=True)

    original = SERVER_DBC.read_bytes()
    updated = rewrite_pvp(original)
    out_dbc.write_bytes(updated)

    with zipfile.ZipFile(CLIENT_ZIP) as zf:
        member = next(n for n in zf.namelist() if n.lower().endswith("patch-enus-8.mpq"))
        mpq.write_bytes(zf.read(member))
    run(str(MPQTOOL), "extract", "-o", str(extracted), str(mpq))
    client_dbc = extracted / "DBFilesClient" / "PvpDifficulty.dbc"
    if not client_dbc.exists():
        raise FileNotFoundError(client_dbc)
    client_dbc.write_bytes(rewrite_pvp(client_dbc.read_bytes()))

    rebuilt = work / "patch-enUS-8-rebuilt.MPQ"
    run(str(MPQTOOL), "new", str(extracted), str(rebuilt))
    rebuilt_blob = rebuilt.read_bytes()
    with zipfile.ZipFile(out_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        zf.writestr("patch-enUS-8.MPQ", rebuilt_blob)

    want_maps = set(BRACKETS)
    for label, blob in (("server", updated), ("client", client_dbc.read_bytes())):
        check = [r for r in rows(blob) if r[1] in want_maps]
        print(label, "rows:", check)
    print("server artifact:", out_dbc, hashlib.sha256(updated).hexdigest())
    print("client artifact:", out_zip, hashlib.sha256(out_zip.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
