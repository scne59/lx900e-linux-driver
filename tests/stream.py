"""Parse and decode streams produced by driver/rastertoLX900.c.

This is a test harness, not part of the driver.  It implements the format
described in docs/PROTOCOL.md from the description rather than from the C code,
so a change that breaks the format shows up here instead of on a printer.

Only the transport framing, page info, band records and the CRDRE row codec are
covered - that is everything the driver emits on its own.
"""

PAGE_INFO_TAG = b"\x1b\x23\x01\x00"
BAND_TAG = b"\x1b\x23\x01\x05"
END_PAGE = b"\x1b\x23\x01\x02\x00"
EOD_0E = bytes.fromhex("a500110001030000000101001b2a050e00000001")
EOD_0B = bytes.fromhex("a500110001030000000101001b2a050b00000001")

FRAME_HEAD = 12
FRAME_TRAIL = 8
FRAME_HEAD_REST = bytes.fromhex("0001030708090a0200")
FRAME_TRAILER = bytes.fromhex("0708090a0708090a")


class StreamError(Exception):
    pass


def strip_frames(data):
    """Remove the transport framing, returning the concatenated payloads."""
    out = bytearray()
    p = 0
    while p < len(data):
        if data[p] != 0xA5:
            raise StreamError("no frame marker at offset %d" % p)
        if p + FRAME_HEAD + FRAME_TRAIL > len(data):
            raise StreamError("truncated frame header at %d" % p)
        if data[p + 3:p + 6] != b"\x00\x01\x03":
            raise StreamError("unexpected frame header at %d" % p)
        length = (data[p + 1] << 8) | data[p + 2]
        payload = length - 9 - FRAME_TRAIL
        if payload < 0:
            raise StreamError("bad frame length %d at %d" % (length, p))
        end = p + FRAME_HEAD + payload + FRAME_TRAIL
        if end > len(data):
            raise StreamError("frame at %d runs past the end" % p)
        trailer = data[end - FRAME_TRAIL:end]
        if payload and data[p + 3:p + FRAME_HEAD] != FRAME_HEAD_REST:
            raise StreamError("unexpected frame header at %d" % p)
        if payload and trailer != FRAME_TRAILER:
            raise StreamError("bad frame trailer at %d" % p)
        if not payload and trailer[0:3] != b"\x1b\x2a\x05":
            # zero-length frames carry an end-of-document command instead
            raise StreamError("bad command frame at %d" % p)
        out += data[p + FRAME_HEAD:p + FRAME_HEAD + payload]
        p = end
    return bytes(out)


def _field(buf, p, maximum, value):
    """Read an extended skip/count field (see PROTOCOL.md 3.4)."""
    if value != maximum:
        return value, p
    total = maximum
    while True:
        if p >= len(buf):
            raise StreamError("truncated field extension")
        b = buf[p]
        p += 1
        total += b
        if b != 0xFF:
            return total, p


def decode_row(payload, wbytes, seed):
    """Decode one CRDRE row against `seed`.  Returns (row, bytes consumed).

    An empty payload means the row is identical to the seed.
    """
    if not payload:
        return bytes(seed), 0
    out = bytearray(wbytes)
    p = o = 0
    while p < len(payload) and o < wbytes:
        ctrl = payload[p]
        p += 1
        if ctrl & 0x80:
            skip, p = _field(payload, p, 3, (ctrl >> 5) & 3)
            count, p = _field(payload, p, 31, ctrl & 0x1F)
            count += 1
            if o + skip > wbytes:
                raise StreamError("seed copy overruns the row")
            out[o:o + skip] = seed[o:o + skip]
            o += skip
            # `count` is a number of bytes, not a number of pairs: read pairs
            # until that many bytes have been produced.
            remaining = count
            while remaining > 0 and o < wbytes:
                if p + 2 > len(payload):
                    raise StreamError("truncated run/value pair")
                n = min(payload[p] + 1, remaining, wbytes - o)
                v = payload[p + 1]
                p += 2
                if n <= 0:
                    raise StreamError("run overruns the row")
                out[o:o + n] = bytes([v]) * n
                o += n
                remaining -= n
        else:
            skip, p = _field(payload, p, 0x0F, (ctrl >> 3) & 0x0F)
            count, p = _field(payload, p, 7, ctrl & 7)
            count += 1
            if o + skip > wbytes:
                raise StreamError("seed copy overruns the row")
            out[o:o + skip] = seed[o:o + skip]
            o += skip
            if p + count > len(payload):
                raise StreamError("truncated literal")
            out[o:o + count] = payload[p:p + count]
            p += count
            o += count
    return bytes(out), p


class Record(object):
    def __init__(self, data, off, plane_count):
        self.offset = off
        self.total = int.from_bytes(data[off + 4:off + 8], "big")
        self.y = int.from_bytes(data[off + 8:off + 12], "big")
        self.b12 = int.from_bytes(data[off + 12:off + 14], "big")
        self.rows = int.from_bytes(data[off + 14:off + 16], "big")
        self.width = int.from_bytes(data[off + 16:off + 18], "big")
        self.minb = int.from_bytes(data[off + 18:off + 20], "big")
        self.maxb = int.from_bytes(data[off + 20:off + 22], "big")
        self.fmt = data[off + 22]
        self.plane = data[off + 23]
        self.plane_count = plane_count
        self.counts = []
        self.lens = []
        self.payload = []
        self.rows_bits = []
        if self.total == 24:
            return
        p = off + 24
        self.counts = [int.from_bytes(data[p + 4 * i:p + 4 * i + 4], "big")
                       for i in range(self.rows)]
        p += 4 * self.rows
        for _ in range(self.rows):
            n = int.from_bytes(data[p:p + 2], "big")
            p += 2
            self.lens.append(n)
            self.payload.append(data[p:p + n])
            p += n

    @property
    def skipped(self):
        return self.total == 24

    def __repr__(self):
        return "<record y=%d plane=%d rows=%d total=%d>" % (
            self.y, self.plane, self.rows, self.total)


class Stream(object):
    def __init__(self, data):
        self.raw = data
        self.data = strip_frames(data)
        d = self.data

        off = d.find(PAGE_INFO_TAG)
        if off < 0:
            raise StreamError("no page-info packet")
        self.page_info_offset = off
        self.prologue = d[:off]
        total = int.from_bytes(d[off + 4:off + 8], "big")
        self.page_info_length = total
        self.tags = {}
        p = off + 11
        while p + 5 <= off + total and d[p] not in (0x1B, 0x2C):
            self.tags[d[p]] = int.from_bytes(d[p + 2:p + 4], "big")
            p += 5
        # the packet ends with a bare 0x2c 0x02 0x00 <res code> tail
        self.res_code = d[p + 3] if (p + 4 <= off + total and d[p] == 0x2C
                                     and d[p + 1] == 0x02) else -1

        self.records = []
        p = d.find(BAND_TAG)
        if p < 0:
            raise StreamError("no band records")
        self.band_offset = p
        self._planes = 0
        while d[p:p + 4] == BAND_TAG:
            if p + 24 > len(d):
                raise StreamError("truncated record header at %d" % p)
            total = int.from_bytes(d[p + 4:p + 8], "big")
            if total < 24 or p + total > len(d):
                raise StreamError("bad record length at %d" % p)
            self._planes = max(self._planes, d[p + 23] + 1)
            rec = Record(d, p, self._planes)
            self.records.append(rec)
            p += total
        self.after_bands = p

        self.end_page_count = d.count(END_PAGE)
        self.has_eod_0b = self.raw.endswith(EOD_0B)
        self.has_eod_0e = self.has_eod_0b and self.raw[-40:-20] == EOD_0E

    def page_info_planes(self):
        """Plane count, inferred from the records (page info does not say)."""
        return self._planes

    # -- geometry -----------------------------------------------------

    @property
    def width(self):
        """Row width in dots as the band records state it (byte padded)."""
        return self.records[0].width if self.records else 0

    @property
    def dot_width(self):
        """Exact page width in dots, from page-info tag 0x15."""
        return self.tags.get(0x15, self.width)

    @property
    def height(self):
        return self.tags.get(0x14, 0)


    @property
    def plane_count(self):
        return self.page_info_planes()

    def bands(self):
        """Group records into bands (consecutive records sharing a `y`)."""
        out = []
        for rec in self.records:
            if out and out[-1][0].y == rec.y:
                out[-1].append(rec)
            else:
                out.append([rec])
        return out

    # -- decoding -----------------------------------------------------

    def plane_rows(self):
        """Decode every plane into bit rows, chaining the codec seed."""
        wbytes = (self.width + 7) // 8 if self.width else 0
        state = {}
        for rec in self.records:
            st = state.setdefault(rec.plane,
                                  {"seed": bytes(wbytes), "rows": {}})
            if rec.skipped:
                st["seed"] = bytes(wbytes)
                continue
            seed = st["seed"]
            for i, payload in enumerate(rec.payload):
                row, used = decode_row(payload, wbytes, seed)
                if used != len(payload):
                    raise StreamError("row %d: %d of %d bytes used" %
                                      (i, used, len(payload)))
                seed = row
                rec.rows_bits.append(row)
                st["rows"][rec.y + i] = row
            st["seed"] = seed
        return {pl: st["rows"] for pl, st in state.items()}

    def coverage(self, plane):
        """Fraction of dots set on `plane` over the rows it covers."""
        rows = self.plane_rows().get(plane, {})
        if not rows:
            return 0.0
        dots = len(rows) * self.dot_width
        on = 0
        for row in rows.values():
            for b in row:
                on += bin(b).count("1")
        return on / dots if dots else 0.0

    def bitmap(self, plane):
        """Full-page bitmap for `plane`, as a list of ints (0/1 per dot)."""
        rows = self.plane_rows().get(plane, {})
        out = []
        for y in sorted(rows):
            row = rows[y]
            bits = []
            for b in row:
                for k in range(8):
                    bits.append((b >> (7 - k)) & 1)
            out.append((y, bits[:self.dot_width]))
        return out
