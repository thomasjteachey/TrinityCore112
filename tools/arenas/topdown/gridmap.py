"""TrinityCore .map (GridMap) reader: terrain height + area id, mirroring GridMap::getHeightFromFloat."""
import os, struct
import numpy as np

SZ = 1600.0 / 3
MAP_RES = 128


class GridMap:
    def __init__(self, path):
        b = open(path, "rb").read()
        (self.magic, ver, build, aofs, asz, hofs, hsz, lofs, lsz, hoofs, hosz) = struct.unpack_from("<4sI4s8I", b, 0)
        self.area = None
        self.gridArea = 0
        if aofs:
            fourcc, flags, ga = struct.unpack_from("<IHH", b, aofs)
            self.gridArea = ga
            if not flags & 1:
                self.area = np.frombuffer(b, dtype="<u2", count=256, offset=aofs + 8).reshape(16, 16)
        fourcc, hflags, gh, gmh = struct.unpack_from("<IIff", b, hofs)
        self.hflags = hflags
        self.gridHeight = gh
        p = hofs + 16
        if hflags & 1:
            self.v9 = None
            self.v8 = None
        elif hflags & 2:
            m = (gmh - gh) / 65535
            self.v9 = np.frombuffer(b, dtype="<u2", count=129 * 129, offset=p).reshape(129, 129) * m + gh
            p += 129 * 129 * 2
            self.v8 = np.frombuffer(b, dtype="<u2", count=128 * 128, offset=p).reshape(128, 128) * m + gh
        elif hflags & 4:
            m = (gmh - gh) / 255
            self.v9 = np.frombuffer(b, dtype="u1", count=129 * 129, offset=p).reshape(129, 129) * m + gh
            p += 129 * 129
            self.v8 = np.frombuffer(b, dtype="u1", count=128 * 128, offset=p).reshape(128, 128) * m + gh
        else:
            self.v9 = np.frombuffer(b, dtype="<f4", count=129 * 129, offset=p).reshape(129, 129).astype(np.float64)
            p += 129 * 129 * 4
            self.v8 = np.frombuffer(b, dtype="<f4", count=128 * 128, offset=p).reshape(128, 128).astype(np.float64)
        self.holes = None
        if hoofs and hosz:
            self.holes = np.frombuffer(b, dtype="u1", count=hosz, offset=hoofs)

    def height(self, x, y):
        if self.v9 is None:
            return self.gridHeight
        x = MAP_RES * (32 - x / SZ)
        y = MAP_RES * (32 - y / SZ)
        xi, yi = int(x), int(y)
        x -= xi
        y -= yi
        xi &= MAP_RES - 1
        yi &= MAP_RES - 1
        v9, v8 = self.v9, self.v8
        if x + y < 1:
            if x > y:
                h1 = v9[xi, yi]; h2 = v9[xi + 1, yi]; h5 = 2 * v8[xi, yi]
                a = h2 - h1; b = h5 - h1 - h2; c = h1
            else:
                h1 = v9[xi, yi]; h3 = v9[xi, yi + 1]; h5 = 2 * v8[xi, yi]
                a = h5 - h1 - h3; b = h3 - h1; c = h1
        else:
            if x > y:
                h2 = v9[xi + 1, yi]; h4 = v9[xi + 1, yi + 1]; h5 = 2 * v8[xi, yi]
                a = h2 + h4 - h5; b = h4 - h2; c = h5 - h4
            else:
                h3 = v9[xi, yi + 1]; h4 = v9[xi + 1, yi + 1]; h5 = 2 * v8[xi, yi]
                a = h4 - h3; b = h3 + h4 - h5; c = h5 - h4
        return a * x + b * y + c

    def area_at(self, x, y):
        if self.area is None:
            return self.gridArea
        lx = int(16 * (32 - x / SZ)) & 15
        ly = int(16 * (32 - y / SZ)) & 15
        return int(self.area[lx, ly])


class MapData:
    def __init__(self, maps_dir, mapid):
        self.dir = maps_dir
        self.mapid = mapid
        self.cache = {}

    def grid(self, x, y):
        gx = int(32 - x / SZ)
        gy = int(32 - y / SZ)
        k = (gx, gy)
        if k not in self.cache:
            p = os.path.join(self.dir, "%03d%02d%02d.map" % (self.mapid, gx, gy))
            self.cache[k] = GridMap(p) if os.path.exists(p) else None
        return self.cache[k]

    def height(self, x, y):
        g = self.grid(x, y)
        return None if g is None else g.height(x, y)

    def area(self, x, y):
        g = self.grid(x, y)
        return None if g is None else g.area_at(x, y)
