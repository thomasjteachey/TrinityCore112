"""Tiny WDBC (WoW 3.3.5 client DB) reader."""

import struct


class DBC:
    def __init__(self, data):
        magic, self.count, self.fields, self.rec_size, self.str_size = struct.unpack_from(
            "<4sIIII", data, 0
        )
        if magic != b"WDBC":
            raise ValueError("not a WDBC file (%r)" % magic)
        self.rec_off = 20
        self.str_off = 20 + self.count * self.rec_size
        self.data = data

    def uint(self, rec, field):
        return struct.unpack_from(
            "<I", self.data, self.rec_off + rec * self.rec_size + field * 4
        )[0]

    def int(self, rec, field):
        return struct.unpack_from(
            "<i", self.data, self.rec_off + rec * self.rec_size + field * 4
        )[0]

    def flt(self, rec, field):
        return struct.unpack_from(
            "<f", self.data, self.rec_off + rec * self.rec_size + field * 4
        )[0]

    def str(self, rec, field):
        off = self.uint(rec, field)
        if off == 0 or off >= self.str_size:
            return ""
        start = self.str_off + off
        end = self.data.index(b"\x00", start)
        return self.data[start:end].decode("utf-8", "replace")

    def __len__(self):
        return self.count
