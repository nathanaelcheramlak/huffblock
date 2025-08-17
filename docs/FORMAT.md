# huffblock format, version 1

All sizes and offsets below are in bytes unless specified otherwise. All
multibyte integers are unsigned and little-endian. Headers are serialized field
by field, with no implicit alignment or C++ structure padding.

The file consists of a 16-byte file header, zero or more block records, and a
required 24-byte end marker. No trailing bytes or concatenated archives are
permitted. There is no index, filename, timestamp, or directory metadata.

## File header

| Offset | Width | Field | Required value |
|---:|---:|---|---|
| 0 | 8 | Magic | `48 55 46 46 42 4C 4B 00` (`HUFFBLK` plus NUL) |
| 8 | 2 | Version | `1` |
| 10 | 2 | Flags | `0` |
| 12 | 4 | Block-size limit | `1..16777216` |

The encoder normally writes 2,097,152 as the block-size limit. A decoder rejects
any data block whose original size exceeds the declared limit. Smaller blocks
are allowed anywhere in the stream; the encoder fills each block except the last.

## Block header

Every data block and the end marker start with this 24-byte header:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 1 | Encoding: `0` raw, `1` canonical Huffman, `255` end marker |
| 1 | 3 | Reserved; all zero |
| 4 | 4 | Original size |
| 8 | 4 | Body size, immediately following this header |
| 12 | 4 | CRC-32 of the original bytes |
| 16 | 8 | Encoded bit count |

Unknown encodings and nonzero reserved bytes are rejected. Data blocks must have
an original size from 1 through the file's block-size limit. Original size zero
is reserved for the end marker. Body sizes and bit counts must satisfy the rules
below before the body is allocated or read.

### Raw block (`0`)

The body is exactly the original bytes. Body size equals original size. Encoded
bit count equals `8 * original_size`. The checksum must match the body.

### Canonical Huffman block (`1`)

The body starts with 256 one-byte code lengths, indexed by unsigned byte value
`0..255`, followed by the encoded bitstream.

- Length `0` means the byte is absent from the codebook.
- Nonzero lengths must be between `1` and `32`.
- At least one symbol must be present.
- Multiple-symbol codebooks must describe a complete prefix tree: neither
  overlapping codes nor unused code space is accepted.
- A single-symbol codebook must assign that symbol length `1` and code `0`.

Canonical assignment orders symbols by increasing code length, then increasing
unsigned byte value. For a precise reconstruction, let `count[L]` be the number
of symbols with length `L`, with `count[0] = 0`:

```text
code = 0
for L = 1..32:
    code = (code + count[L - 1]) << 1
    next[L] = code

for symbol = 0..255:
    L = lengths[symbol]
    if L != 0:
        assign symbol the L-bit representation of next[L]
        next[L] += 1
```

These computations need at least 64-bit unsigned arithmetic, even though an
individual code fits in 32 bits.

Emit each code most-significant bit first. Fill each payload byte from its most
significant bit downwards. Unused low bits in the last byte must be zero.

Encoded bit count includes only meaningful payload bits, excluding the length
table and padding. It must be between `original_size` and `32 * original_size`.
Body size must be exactly `256 + ceil(encoded_bit_count / 8)`.

The decoder must consume every meaningful bit, finish at a code boundary, and
produce exactly the original size. It must then verify the checksum. A Huffman
block may be larger than its original data according to the format; this encoder
chooses raw storage whenever Huffman would not save body space.

Example: lengths `A=1`, `B=2`, `C=2` give codes `A=0`, `B=10`, `C=11`. Encoding
`ABC` produces five meaningful bits, `01011`, stored as payload byte `58` in hex.
The body is the 256-byte table followed by that byte. This small example is valid
for decoding, although the encoder would store three input bytes raw.

If tree construction ever produces a length above 32, this encoder uses raw
storage instead of truncating lengths or attempting length-limited coding.

### End marker (`255`)

All other 23 header bytes must be zero. There is no body, and EOF must follow
immediately. An empty archive is therefore exactly 40 bytes. EOF without the end
marker, including immediately after a complete data block, is an error.

## CRC-32

Use CRC-32/ISO-HDLC (the conventional ZIP/gzip CRC-32):

- Reflected polynomial: `0xEDB88320`.
- Initial value: `0xFFFFFFFF`.
- Process bytes in original order, reflected bit processing.
- Final XOR: `0xFFFFFFFF`.
- Check value for ASCII `123456789`: `0xCBF43926`.
- Check value for an empty byte sequence: `0`.

The block checksum covers only uncompressed bytes, not headers or length tables.
It is a corruption check, not a cryptographic authentication mechanism.

## Deterministic encoder rules

Build a Huffman tree from nonzero byte frequencies. Repeatedly merge the two
nodes with the smallest `(frequency, minimum_symbol_in_subtree)` pair, taking
the smaller pair first. A leaf's code length is its depth, except that a
single-symbol tree uses length one. Then apply canonical assignment above.

The encoder selects Huffman only when its complete body is strictly smaller
than the original bytes. Raw and Huffman records have the same header size.

## Resource bounds and evolution

The hard original-block limit is 16 MiB. The largest legal Huffman body is
`256 + 4 * original_size`, so malformed body-size declarations cannot request
arbitrary allocations. The wire format itself does not cap total decoded size.
The implementation supports a caller-selected output limit and uses subtraction
before addition to prevent overflow in size accounting. Its default live block
buffer budget is 256 MiB; admission is clamped to that budget. Resource options
are application policy, not additional wire fields.

Unknown versions are rejected. A future incompatible format must use a new
version. Indexes, alternate codebook encodings, and additional block modes are
not part of version 1.

Parallel processing does not change version 1. Records are written in original
block order, and worker count and admission limits are not stored in the file.
