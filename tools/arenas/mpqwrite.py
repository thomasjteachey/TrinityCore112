"""Minimal StormLib binding for writing MPQ archives.

StormLib is the reference implementation -- it is what the game itself reads
with and what the server's smpq wraps -- so archives written through it behave
the same as every other patch in the client.

Only what this pipeline needs: create, open, add, close. Everything is the ANSI
entry point, which is what the shipped DLL exports.

The DLL ships with WDBXEditor at tools/WDBX/x64/StormLib.dll.
"""

import ctypes
import os
from ctypes import wintypes

DLL_CANDIDATES = [
    r"C:\Projects\Gamedev\wow\tools\WDBX\x64\StormLib.dll",
    r"C:\Projects\Gamedev\wow\tools\WDBX\StormLib.dll",
    r"C:\Projects\Gamedev\wow\tools\WDBX\code\WDBXEditor\WDBXEditor\x64\StormLib.dll",
]

# --- create flags
MPQ_CREATE_LISTFILE   = 0x00100000
MPQ_CREATE_ATTRIBUTES = 0x00200000
MPQ_CREATE_ARCHIVE_V1 = 0x00000000
MPQ_CREATE_ARCHIVE_V2 = 0x01000000

# --- file flags
MPQ_FILE_IMPLODE         = 0x00000100
MPQ_FILE_COMPRESS        = 0x00000200
MPQ_FILE_ENCRYPTED       = 0x00010000
MPQ_FILE_FIX_KEY         = 0x00020000
MPQ_FILE_SINGLE_UNIT     = 0x01000000
MPQ_FILE_REPLACEEXISTING = 0x80000000

# --- compression
MPQ_COMPRESSION_ZLIB  = 0x02
MPQ_COMPRESSION_BZIP2 = 0x10

# WoW's own data files are zlib-compressed with a 4096-byte sector size, which
# is what SFileCreateArchive uses by default.
DEFAULT_ADD_FLAGS = MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING


class StormError(RuntimeError):
    pass


class Storm:
    def __init__(self, path=None):
        for cand in ([path] if path else DLL_CANDIDATES):
            if cand and os.path.exists(cand):
                self.dll = ctypes.WinDLL(cand)
                self.path = cand
                break
        else:
            raise StormError("StormLib.dll not found in %s" % (DLL_CANDIDATES,))

        d = self.dll
        # StormLib takes filesystem paths as TCHAR, so whether they are char* or
        # wchar_t* depends on how the DLL was built -- and the export name is the
        # same either way, so it cannot be told apart by inspection. Archived
        # names are always ANSI regardless. Probe for it in _detect_width below.
        self.wide = False

        d.SFileCreateArchive.restype = wintypes.BOOL
        d.SFileOpenArchive.restype = wintypes.BOOL
        d.SFileAddFileEx.restype = wintypes.BOOL
        self._set_path_width(False)

        d.SFileSetMaxFileCount.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        d.SFileSetMaxFileCount.restype = wintypes.BOOL

        d.SFileFlushArchive.argtypes = [wintypes.HANDLE]
        d.SFileFlushArchive.restype = wintypes.BOOL

        d.SFileCloseArchive.argtypes = [wintypes.HANDLE]
        d.SFileCloseArchive.restype = wintypes.BOOL

        d.SFileCompactArchive.argtypes = [wintypes.HANDLE, ctypes.c_char_p, wintypes.BOOL]
        d.SFileCompactArchive.restype = wintypes.BOOL

    # ---------------------------------------------------------------- helpers
    def _set_path_width(self, wide):
        self.wide = wide
        pt = ctypes.c_wchar_p if wide else ctypes.c_char_p
        d = self.dll
        d.SFileCreateArchive.argtypes = [pt, wintypes.DWORD, wintypes.DWORD,
                                         ctypes.POINTER(wintypes.HANDLE)]
        d.SFileOpenArchive.argtypes = [pt, wintypes.DWORD, wintypes.DWORD,
                                       ctypes.POINTER(wintypes.HANDLE)]
        # local path is TCHAR, archived name is always ANSI
        d.SFileAddFileEx.argtypes = [wintypes.HANDLE, pt, ctypes.c_char_p,
                                     wintypes.DWORD, wintypes.DWORD, wintypes.DWORD]

    def detect_path_width(self):
        """Create a throwaway archive each way and keep whichever really lands."""
        import tempfile
        for wide in (False, True):
            self._set_path_width(wide)
            tmp = tempfile.mkdtemp(prefix="stormprobe")
            probe = os.path.join(tmp, "probe.MPQ")
            try:
                h = wintypes.HANDLE()
                flags = MPQ_CREATE_LISTFILE | MPQ_CREATE_ARCHIVE_V1
                ok = self.dll.SFileCreateArchive(self._path(probe), flags, 16, ctypes.byref(h))
                if ok:
                    self.dll.SFileCloseArchive(h)
                if ok and os.path.exists(probe) and os.path.getsize(probe) > 0:
                    return wide
            except Exception:
                pass
            finally:
                import shutil
                shutil.rmtree(tmp, ignore_errors=True)
        raise StormError("could not determine StormLib path width -- neither "
                         "char* nor wchar_t* produced an archive")

    def _path(self, p):
        """A filesystem path in whatever width this DLL wants."""
        return p if self.wide else p.encode("mbcs")

    @staticmethod
    def _enc(p):
        """An archived name -- always ANSI."""
        return p.encode("mbcs") if isinstance(p, str) else p

    def _err(self, what):
        return StormError("%s failed (GetLastError=%d)" % (what, ctypes.get_last_error()
                                                           if False else ctypes.GetLastError()))

    def create(self, path, max_files, v2=False):
        h = wintypes.HANDLE()
        flags = MPQ_CREATE_LISTFILE | MPQ_CREATE_ATTRIBUTES
        flags |= MPQ_CREATE_ARCHIVE_V2 if v2 else MPQ_CREATE_ARCHIVE_V1
        if not self.dll.SFileCreateArchive(self._path(path), flags, max_files, ctypes.byref(h)):
            raise self._err("SFileCreateArchive(%s)" % path)
        if not os.path.exists(path):
            raise StormError("SFileCreateArchive(%s) reported success but wrote nothing" % path)
        return h

    def open(self, path):
        h = wintypes.HANDLE()
        if not self.dll.SFileOpenArchive(self._path(path), 0, 0, ctypes.byref(h)):
            raise self._err("SFileOpenArchive(%s)" % path)
        return h

    def set_max_files(self, h, n):
        if not self.dll.SFileSetMaxFileCount(h, n):
            raise self._err("SFileSetMaxFileCount(%d)" % n)

    def add(self, h, local_path, archived_name,
            flags=DEFAULT_ADD_FLAGS, compression=MPQ_COMPRESSION_ZLIB):
        name = archived_name.replace("/", "\\")
        if not self.dll.SFileAddFileEx(h, self._path(local_path), self._enc(name),
                                       flags, compression, compression):
            raise self._err("SFileAddFileEx(%s)" % name)

    def compact(self, h):
        if not self.dll.SFileCompactArchive(h, None, False):
            raise self._err("SFileCompactArchive")

    def close(self, h):
        self.dll.SFileFlushArchive(h)
        if not self.dll.SFileCloseArchive(h):
            raise self._err("SFileCloseArchive")


# ------------------------------------------------------------------- selftest
def _selftest():
    """Write a small archive, then read it back with our own reader.

    Proves the binding, the calling convention and the compression choice all
    line up before any of it is pointed at a real patch.
    """
    import shutil
    import sys
    import tempfile

    SCRATCH = (r"C:\Users\broki\AppData\Local\Temp\claude"
               r"\C--Ascension-Launcher-resources-ascension-live"
               r"\b4c02561-8768-40b5-abb1-376e8e1417b6\scratchpad")
    sys.path.insert(0, SCRATCH)
    from mpq import MPQArchive

    tmp = tempfile.mkdtemp(prefix="mpqtest")
    try:
        payloads = {
            "World\\Maps\\Test\\hello.txt": b"hello arena" * 500,
            "DBFilesClient\\Thing.dbc": bytes(range(256)) * 40,
            "deep\\path\\with spaces\\x.blp": os.urandom(9000),
        }
        for i, (name, data) in enumerate(payloads.items()):
            p = os.path.join(tmp, "f%d.bin" % i)
            with open(p, "wb") as fh:
                fh.write(data)
            payloads[name] = (data, p)

        arc = os.path.join(tmp, "test.MPQ")
        s = Storm()
        print("using %s" % s.path)
        wide = s.detect_path_width()
        s._set_path_width(wide)
        print("path width: %s" % ("wchar_t* (UNICODE build)" if wide else "char* (ANSI build)"))
        h = s.create(arc, 1024)
        for name, (_data, p) in payloads.items():
            s.add(h, p, name)
        s.close(h)
        print("wrote %s (%d bytes)" % (arc, os.path.getsize(arc)))

        a = MPQArchive(arc)
        listed = a.list_files()
        print("listfile: %d entries" % len(listed))
        ok = True
        for name, (data, _p) in payloads.items():
            got = a.read_file(name)
            same = got == data
            ok &= same
            print("  %-40s %s (%d bytes)" % (name, "OK" if same else "MISMATCH", len(got)))
        a.close()
        print("\n%s" % ("PASS - StormLib round-trips through our reader" if ok else "FAIL"))
        return ok
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    import sys
    sys.exit(0 if _selftest() else 1)
