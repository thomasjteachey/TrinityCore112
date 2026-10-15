"""Client archive access: the winning copy of any file, as the client resolves it."""
import os, sys, struct, re, io
sys.path.insert(0, r"C:\Projects\Gamedev\wow\tools\mpqpy")
from mpqread import MPQ
CLIENT = os.environ.get("WOW_CLIENT_DATA", r"C:\Projects\Gamedev\wow\clients\centurion\Data")
BS = "\\"
_arcs = None


def rank(fn):
    b = fn.lower()[:-4]
    m = re.match(r".*patch(?:-[a-z]{4})?-(.)$", b)
    if m:
        c = m.group(1)
        return (2, 10 + int(c)) if c.isdigit() else (2, 20 + ord(c) - 96)
    if b.startswith("patch"):
        return (1, 0)
    return (0, 0)


def arcs():
    global _arcs
    if _arcs is None:
        _arcs = []
        for d in (CLIENT, os.path.join(CLIENT, "enUS")):
            for fn in os.listdir(d):
                if fn.lower().endswith(".mpq"):
                    try:
                        _arcs.append((fn, MPQ(os.path.join(d, fn))))
                    except Exception as e:
                        print("cannot open", fn, e)
        _arcs.sort(key=lambda a: rank(a[0]))
    return _arcs


def holders(name):
    return [fn for fn, a in arcs() if a.find(name) is not None]


def winner(name):
    h = None
    for fn, a in arcs():
        if a.find(name) is not None:
            h = (fn, a)
    return h


def read(name):
    w = winner(name)
    if not w:
        return None
    return w[1].extract(name)


def src(name):
    w = winner(name)
    return w[0] if w else None


class DBC:
    def __init__(self, b):
        self.b = b
        self.n, self.f, self.rs, self.ss = struct.unpack_from('<4I', b, 4)
        self.sb = 20 + self.n * self.rs

    def rows(self):
        for i in range(self.n):
            yield struct.unpack_from('<%dI' % self.f, self.b, 20 + i * self.rs)

    def s(self, o):
        return self.b[self.sb + o:self.b.index(b'\0', self.sb + o)].decode('utf-8', 'replace')


def f32(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]


def trs_map():
    t = read(r"textures\Minimap\md5translate.trs").decode('latin1')
    m = {}
    for line in t.splitlines():
        if '\t' in line:
            k, v = line.split('\t', 1)
            m[k.strip().lower().replace('/', BS)] = v.strip()
    return m
