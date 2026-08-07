"""Small scriptable StormLib wrapper for WoW 3.3.5 MPQ maintenance.

This exists because Violet Hold patch work must be reproducible from a command
line.  It deliberately exposes only the few operations the patch scripts need:
enumerate, read, replace/add, flush, and close.
"""

from __future__ import annotations

import ctypes
import os
from pathlib import Path


DWORD = ctypes.c_uint32
HANDLE = ctypes.c_void_p
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
SFILE_OPEN_FROM_MPQ = 0
STREAM_FLAG_READ_ONLY = 0x00000100
MPQ_FILE_COMPRESS = 0x00000200
MPQ_FILE_REPLACEEXISTING = 0x80000000
MPQ_COMPRESSION_ZLIB = 0x02


class SFileFindData(ctypes.Structure):
    _fields_ = [
        ("cFileName", ctypes.c_char * 260),
        ("szPlainName", ctypes.c_void_p),
        ("dwHashIndex", DWORD),
        ("dwBlockIndex", DWORD),
        ("dwFileSize", DWORD),
        ("dwFileFlags", DWORD),
        ("dwCompSize", DWORD),
        ("dwFileTimeLo", DWORD),
        ("dwFileTimeHi", DWORD),
        ("lcLocale", DWORD),
    ]


def default_stormlib() -> Path:
    configured = os.environ.get("VHR_STORMLIB")
    if configured:
        return Path(configured)
    wow_root = Path(__file__).resolve().parents[4]
    return wow_root / "tools" / "WDBX" / "x64" / "StormLib.dll"


class MPQArchive:
    def __init__(self, path: os.PathLike[str] | str, writable: bool = False,
                 stormlib: os.PathLike[str] | str | None = None):
        self.path = Path(path)
        self.dll = ctypes.WinDLL(str(stormlib or default_stormlib()), use_last_error=True)
        self._bind()
        self.handle = HANDLE()
        flags = 0 if writable else STREAM_FLAG_READ_ONLY
        if not self.dll.SFileOpenArchive(
                str(self.path), 0, flags, ctypes.byref(self.handle)):
            self._raise("SFileOpenArchive")
        self.writable = writable

    def _bind(self) -> None:
        d = self.dll
        # This StormLib build uses TCHAR for filesystem paths (Unicode on
        # Windows), while names *inside* an MPQ remain narrow strings.
        d.SFileOpenArchive.argtypes = [ctypes.c_wchar_p, DWORD, DWORD, ctypes.POINTER(HANDLE)]
        d.SFileOpenArchive.restype = ctypes.c_int
        d.SFileCloseArchive.argtypes = [HANDLE]
        d.SFileCloseArchive.restype = ctypes.c_int
        d.SFileOpenFileEx.argtypes = [HANDLE, ctypes.c_char_p, DWORD, ctypes.POINTER(HANDLE)]
        d.SFileOpenFileEx.restype = ctypes.c_int
        d.SFileGetFileSize.argtypes = [HANDLE, ctypes.POINTER(DWORD)]
        d.SFileGetFileSize.restype = DWORD
        d.SFileReadFile.argtypes = [HANDLE, ctypes.c_void_p, DWORD, ctypes.POINTER(DWORD), ctypes.c_void_p]
        d.SFileReadFile.restype = ctypes.c_int
        d.SFileCloseFile.argtypes = [HANDLE]
        d.SFileCloseFile.restype = ctypes.c_int
        d.SFileFindFirstFile.argtypes = [HANDLE, ctypes.c_char_p, ctypes.POINTER(SFileFindData), ctypes.c_char_p]
        d.SFileFindFirstFile.restype = HANDLE
        d.SFileFindNextFile.argtypes = [HANDLE, ctypes.POINTER(SFileFindData)]
        d.SFileFindNextFile.restype = ctypes.c_int
        d.SFileFindClose.argtypes = [HANDLE]
        d.SFileFindClose.restype = ctypes.c_int
        d.SFileAddFileEx.argtypes = [HANDLE, ctypes.c_wchar_p, ctypes.c_char_p, DWORD, DWORD, DWORD]
        d.SFileAddFileEx.restype = ctypes.c_int
        if hasattr(d, "SFileFlushArchive"):
            d.SFileFlushArchive.argtypes = [HANDLE]
            d.SFileFlushArchive.restype = ctypes.c_int

    def _raise(self, operation: str) -> None:
        raise OSError(ctypes.get_last_error(), f"{operation} failed for {self.path}")

    def names(self, mask: str = "*") -> list[str]:
        data = SFileFindData()
        finder = self.dll.SFileFindFirstFile(
            self.handle, mask.encode("ascii"), ctypes.byref(data), None)
        if finder == INVALID_HANDLE_VALUE or not finder:
            return []
        out = []
        try:
            while True:
                out.append(bytes(data.cFileName).split(b"\0", 1)[0].decode("utf-8", "replace"))
                if not self.dll.SFileFindNextFile(finder, ctypes.byref(data)):
                    break
        finally:
            self.dll.SFileFindClose(finder)
        return out

    def read(self, archived_name: str) -> bytes:
        file_handle = HANDLE()
        if not self.dll.SFileOpenFileEx(
                self.handle, archived_name.encode("utf-8"), SFILE_OPEN_FROM_MPQ,
                ctypes.byref(file_handle)):
            self._raise(f"SFileOpenFileEx({archived_name})")
        try:
            high = DWORD()
            low = self.dll.SFileGetFileSize(file_handle, ctypes.byref(high))
            size = (high.value << 32) | low
            buffer = ctypes.create_string_buffer(size)
            read = DWORD()
            if size and not self.dll.SFileReadFile(
                    file_handle, buffer, size, ctypes.byref(read), None):
                self._raise(f"SFileReadFile({archived_name})")
            if read.value != size:
                raise OSError(f"short MPQ read for {archived_name}: {read.value}/{size}")
            return buffer.raw[:size]
        finally:
            self.dll.SFileCloseFile(file_handle)

    def add(self, source: os.PathLike[str] | str, archived_name: str) -> None:
        if not self.writable:
            raise PermissionError(f"archive is read-only: {self.path}")
        flags = MPQ_FILE_REPLACEEXISTING | MPQ_FILE_COMPRESS
        if not self.dll.SFileAddFileEx(
                self.handle, str(Path(source)), archived_name.encode("utf-8"),
                flags, MPQ_COMPRESSION_ZLIB, MPQ_COMPRESSION_ZLIB):
            self._raise(f"SFileAddFileEx({archived_name})")

    def flush(self) -> None:
        if hasattr(self.dll, "SFileFlushArchive") and not self.dll.SFileFlushArchive(self.handle):
            self._raise("SFileFlushArchive")

    def close(self) -> None:
        if self.handle:
            if not self.dll.SFileCloseArchive(self.handle):
                self._raise("SFileCloseArchive")
            self.handle = HANDLE()

    def __enter__(self) -> "MPQArchive":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()
