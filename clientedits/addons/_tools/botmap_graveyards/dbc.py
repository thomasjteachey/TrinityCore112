import struct


def read_dbc(path):
    data = open(path, 'rb').read()
    magic, nrec, nfld, rsize, ssize = struct.unpack_from('<4s4I', data, 0)
    assert magic == b'WDBC', path
    assert rsize == nfld * 4, (path, rsize, nfld)
    recs = data[20:20 + nrec * rsize]
    strings = data[20 + nrec * rsize:]
    rows = []
    for i in range(nrec):
        rows.append(recs[i * rsize:(i + 1) * rsize])
    return rows, nfld, strings


def u32(rec, f):
    return struct.unpack_from('<I', rec, f * 4)[0]


def i32(rec, f):
    return struct.unpack_from('<i', rec, f * 4)[0]


def f32(rec, f):
    return struct.unpack_from('<f', rec, f * 4)[0]


def s(rec, f, strings):
    off = u32(rec, f)
    end = strings.index(b'\0', off)
    return strings[off:end].decode('utf-8', 'replace')


def worldsafelocs(path):
    rows, nfld, st = read_dbc(path)
    assert nfld == 22, nfld
    out = {}
    for r in rows:
        out[u32(r, 0)] = (u32(r, 1), f32(r, 2), f32(r, 3), f32(r, 4), s(r, 5, st))
    return out


def worldmaparea(path):
    rows, nfld, st = read_dbc(path)
    assert nfld == 11, nfld
    out = {}
    for r in rows:
        out[u32(r, 0)] = dict(map=i32(r, 1), area=u32(r, 2), name=s(r, 3, st),
                              left=f32(r, 4), right=f32(r, 5), top=f32(r, 6), bottom=f32(r, 7),
                              display=i32(r, 8))
    return out


def maps(path):
    rows, nfld, st = read_dbc(path)
    out = {}
    for r in rows:
        out[u32(r, 0)] = dict(dir=s(r, 1, st), type=u32(r, 2), name=s(r, 5, st))
    return out
