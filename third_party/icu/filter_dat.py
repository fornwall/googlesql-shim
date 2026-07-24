"""Removes items from an ICU common-data (.dat) package.

ICU's full `icudt76l.dat` is ~30 MB and lands verbatim in every binary that
links the static ICU data (`dat_to_c.py` embeds it as a C array). Most of it
is data GoogleSQL -- and therefore smallquery/BigQuery -- has no code path to:
legacy charset converters, and the per-locale display-name trees (currency,
language, region, unit names) plus rule-based number spelling and
transliteration. Dropping those is ~16 MB off the shipped data with no reach
into the collation, normalization, character-property or timezone-rule data
GoogleSQL string/timestamp functions actually use.

This is a pure-Python re-implementation of `icupkg -r`, so the build needs no
ICU tools built first (the same reason `dat_to_c.py` re-implements genccode).
Its output was verified byte-for-byte identical to `icupkg -r` on the same
removal list, and the removals are gated behaviorally: smallquery's compliance
corpus and string/collation/normalize/timestamp test suites run against a shim
built with the filtered data.

The .dat format (little-endian `UDataOffsetTOC`): a `UDataHeader` of
`headerSize` bytes, then `uint32 count`, then `count` pairs of
`(uint32 nameOffset, uint32 dataOffset)` -- both relative to the start of the
ToC (the byte after the header). NUL-terminated item names follow the entry
table, then the 16-byte-aligned item bodies, in the same (name-sorted) order.

Usage: filter_dat.py <input.dat> <remove-patterns.txt> <output.dat>
where the patterns file is one Python regex per line (`#` comments and blank
lines ignored), matched with `re.search` against each item name (which
includes the package prefix, e.g. `icudt76l/curr/en.res`).
"""

import re
import struct
import sys


def filter_dat(data: bytes, should_remove) -> bytes:
    base = struct.unpack_from("<H", data, 0)[0]  # headerSize
    header = data[:base]
    (count,) = struct.unpack_from("<I", data, base)

    entries = [struct.unpack_from("<II", data, base + 4 + i * 8) for i in range(count)]
    kept = []
    for i, (name_off, data_off) in enumerate(entries):
        end = data.index(b"\0", base + name_off)
        name = data[base + name_off : end].decode("ascii")
        # Bodies are contiguous in ToC order, so the next item's offset (or
        # end of file) bounds this one.
        body_end = entries[i + 1][1] if i + 1 < count else len(data) - base
        body = data[base + data_off : base + body_end]
        if not should_remove(name):
            kept.append((name, body))

    n = len(kept)
    toc_size = 4 + n * 8

    names_blob = bytearray()
    name_offsets = []
    for name, _ in kept:
        name_offsets.append(toc_size + len(names_blob))
        names_blob += name.encode("ascii") + b"\0"
    data_start = toc_size + len(names_blob)
    data_start += (-data_start) % 16  # 16-align the body region
    names_blob += b"\0" * (data_start - toc_size - len(names_blob))

    body_blob = bytearray()
    data_offsets = []
    for _, body in kept:
        data_offsets.append(data_start + len(body_blob))
        body_blob += body + b"\0" * ((-len(body)) % 16)  # keep each body 16-aligned

    toc = bytearray(struct.pack("<I", n))
    for name_off, data_off in zip(name_offsets, data_offsets):
        toc += struct.pack("<II", name_off, data_off)

    return header + bytes(toc) + bytes(names_blob) + bytes(body_blob)


def main() -> None:
    src, patterns_path, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(patterns_path, encoding="ascii") as f:
        patterns = [
            re.compile(line.strip())
            for line in f
            if line.strip() and not line.startswith("#")
        ]

    def should_remove(name: str) -> bool:
        return any(p.search(name) for p in patterns)

    with open(src, "rb") as f:
        data = f.read()
    with open(dst, "wb") as f:
        f.write(filter_dat(data, should_remove))


if __name__ == "__main__":
    main()
