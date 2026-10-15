"""Textured orthographic top-down render of WMO geometry - a bird's-eye "screenshot" built
from the client's own files, used to make minimap tiles that show what is really there.

World frame: X north (image up), Y west (image left).
"""
import io, math, struct
import numpy as np
from PIL import Image
import cl, geo

_tex = {}


def texture(name):
    key = name.upper()
    if key in _tex:
        return _tex[key]
    b = cl.read(name)
    arr = None
    if b is not None:
        try:
            im = Image.open(io.BytesIO(b))
            im.load()
            arr = np.asarray(im.convert("RGBA"), dtype=np.float32) / 255.0
        except Exception as e:
            print("texture decode failed", name, e)
    _tex[key] = arr
    return arr


def wmo_textured(root_name):
    """[(verts Nx3, uv Nx2, colours Nx3 or None, tris Mx3, tex_index M, visible M)], [texture arrays]"""
    root = cl.read(root_name)
    o, s = geo.find(root, "MOHD")
    ngroups = struct.unpack_from("<I", root, o + 4)[0]
    to, ts = geo.find(root, "MOTX")
    motx = root[to:to + ts]
    mo, ms = geo.find(root, "MOMT")
    mats = []
    for i in range(ms // 64):
        flags, shader, blend, tex1 = struct.unpack_from("<4I", root, mo + i * 64)
        name = motx[tex1:motx.index(b"\0", tex1)].decode("latin1") if tex1 < len(motx) else ""
        mats.append((flags, blend, name))
    texs = [texture(m[2]) if m[2] else None for m in mats]
    groups = []
    base = root_name[:-4]
    for gi in range(ngroups):
        g = cl.read("%s_%03d.wmo" % (base, gi))
        if g is None:
            continue
        po, ps = geo.find(g, "MOGP")
        verts = tris = mopy = uv = cols = None
        for m, so, ss in geo.chunks(g, po + 0x44, po + ps):
            if m == "MOVT" and verts is None:
                verts = np.frombuffer(g, dtype="<f4", count=ss // 4, offset=so).reshape(-1, 3).astype(np.float64)
            elif m == "MOVI" and tris is None:
                tris = np.frombuffer(g, dtype="<u2", count=ss // 2, offset=so).reshape(-1, 3).astype(np.int64)
            elif m == "MOPY" and mopy is None:
                mopy = np.frombuffer(g, dtype="u1", count=ss, offset=so).reshape(-1, 2)
            elif m == "MOTV" and uv is None:
                uv = np.frombuffer(g, dtype="<f4", count=ss // 4, offset=so).reshape(-1, 2).astype(np.float64)
            elif m == "MOCV" and cols is None:
                c = np.frombuffer(g, dtype="u1", count=ss, offset=so).reshape(-1, 4).astype(np.float64) / 255.0
                cols = c[:, [2, 1, 0]]
        if verts is None or tris is None or mopy is None:
            continue
        mat = mopy[:, 1].astype(np.int64)
        visible = mat != 0xFF
        groups.append((verts, uv, cols, tris, mat, visible))
    return groups, texs, mats


class TexRaster:
    def __init__(self, x_top, y_left, width_yd, height_yd, px, zclip):
        self.x_top, self.y_left = x_top, y_left
        self.px = px
        self.w = int(round(width_yd / px))
        self.h = int(round(height_yd / px))
        self.z = np.full((self.h, self.w), -1e9)
        self.rgb = np.zeros((self.h, self.w, 3))
        self.zclip = zclip

    def uv_of(self, wx, wy):
        return (self.y_left - wy) / self.px, (self.x_top - wx) / self.px

    def draw(self, P, UV, tex, light, vcol=None, alphatest=False):
        """P: (M,3,3) world; UV: (M,3,2); tex: texture array or None; light: (M,) shade."""
        u = (self.y_left - P[:, :, 1]) / self.px
        v = (self.x_top - P[:, :, 0]) / self.px
        z = P[:, :, 2]
        keep = (u.max(1) >= 0) & (u.min(1) < self.w) & (v.max(1) >= 0) & (v.min(1) < self.h) & (z.min(1) < self.zclip)
        idx = np.nonzero(keep)[0]
        for k in idx:
            self._tri(u[k], v[k], z[k], UV[k] if UV is not None else None, tex, light[k], None if vcol is None else vcol[k], alphatest)

    def _tri(self, u, v, z, uv, tex, shade, vc, alphatest=False):
        u0, u1 = max(int(math.floor(u.min())), 0), min(int(math.ceil(u.max())), self.w - 1)
        v0, v1 = max(int(math.floor(v.min())), 0), min(int(math.ceil(v.max())), self.h - 1)
        if u1 < u0 or v1 < v0:
            return
        uu, vv = np.meshgrid(np.arange(u0, u1 + 1) + 0.5, np.arange(v0, v1 + 1) + 0.5)
        d = (v[1] - v[2]) * (u[0] - u[2]) + (u[2] - u[1]) * (v[0] - v[2])
        if abs(d) < 1e-12:
            return
        a = ((v[1] - v[2]) * (uu - u[2]) + (u[2] - u[1]) * (vv - v[2])) / d
        b = ((v[2] - v[0]) * (uu - u[2]) + (u[0] - u[2]) * (vv - v[2])) / d
        c = 1 - a - b
        m = (a >= -0.01) & (b >= -0.01) & (c >= -0.01)
        if not m.any():
            return
        zz = a * z[0] + b * z[1] + c * z[2]
        m &= zz < self.zclip
        sub = self.z[v0:v1 + 1, u0:u1 + 1]
        upd = m & (zz > sub)
        if not upd.any():
            return
        if tex is not None and uv is not None:
            tu = a * uv[0, 0] + b * uv[1, 0] + c * uv[2, 0]
            tv = a * uv[0, 1] + b * uv[1, 1] + c * uv[2, 1]
            th, tw = tex.shape[:2]
            ti = (np.floor(np.mod(tv, 1.0) * th).astype(int)) % th
            tj = (np.floor(np.mod(tu, 1.0) * tw).astype(int)) % tw
            col = tex[ti, tj, :3]
            if alphatest:
                upd &= tex[ti, tj, 3] > 0.35
                if not upd.any():
                    return
        else:
            col = np.full(uu.shape + (3,), 0.5)
        sub[upd] = zz[upd]
        if vc is not None:
            vcol = a[..., None] * vc[0] + b[..., None] * vc[1] + c[..., None] * vc[2]
            col = col * np.clip(vcol * 2.0, 0.25, 1.6)
        col = col * shade
        self.rgb[v0:v1 + 1, u0:u1 + 1][upd] = col[upd]

    def image(self):
        img = np.clip(self.rgb, 0, 1)
        empty = self.z < -1e8
        img[empty] = 0
        return Image.fromarray((img * 255).astype(np.uint8)), ~empty


LIGHT = np.array([0.35, 0.25, 0.90])
LIGHT = LIGHT / np.linalg.norm(LIGHT)


def render_wmo(ras, root_name, pos, rot, scale=1.0):
    groups, texs, mats = wmo_textured(root_name)
    f = geo.placement_to_world(pos, rot, scale)
    for verts, uv, cols, tris, mat, visible in groups:
        wv = f(verts)
        T = tris[visible]
        M = mat[visible]
        P = wv[T]
        n = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
        ln = np.linalg.norm(n, axis=1) + 1e-12
        n = n / ln[:, None]
        n[n[:, 2] < 0] *= -1
        shade = 0.45 + 0.75 * np.clip(n @ LIGHT, 0, 1)
        UV = uv[T] if uv is not None else None
        VC = cols[T] if cols is not None else None
        for mi in np.unique(M):
            sel = M == mi
            tex = texs[mi] if mi < len(texs) else None
            blend = mats[mi][1] if mi < len(mats) else 0
            ras.draw(P[sel], None if UV is None else UV[sel], tex, shade[sel], None if VC is None else VC[sel], alphatest=blend >= 1)
