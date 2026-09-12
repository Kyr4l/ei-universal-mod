# Evil Islands Texture File Format (.mmp)

## Overview
Evil Islands `.mmp` (Mip-Mapped Picture) files are proprietary texture containers used by the game engine for models, terrain tiles, UI icons, and character redress textures.

Each `.mmp` file consists of a **76-byte header** (19 little-endian 32-bit integers) followed immediately by the texture payload (compressed block data or uncompressed pixel mipmaps).

---

## MMP 76-Byte Header Structure

| Offset | Type | Field Name | Description |
| :--- | :--- | :--- | :--- |
| `0x00` | `char[4]` | `magic` | Identifier string: `"MMP\0"` (`0x00504D4D`) |
| `0x04` | `uint32_t` | `width` | Texture width in pixels |
| `0x08` | `uint32_t` | `height` | Texture height in pixels |
| `0x0C` | `uint32_t` | `mipsOrDataLen` | For `PNT3`: compressed payload size in bytes.<br>For DXT/16-bit: mipmap level count. |
| `0x10` | `char[4]` | `fourcc` | Pixel format FourCC identifier (`DXT1`, `DXT3`, `PNT3`, `QU\0\0`, `PV\0\0`) |
| `0x14` | `uint32_t` | `bitDepth` | Bits per pixel (4 for DXT1, 8 for DXT3, 16 for 16-bit, 32 for PNT3) |
| `0x18` | `uint32_t` | `alphaMask` | Alpha channel bitmask |
| `0x1C` | `uint32_t` | `alphaShift` | Alpha bit shift offset |
| `0x20` | `uint32_t` | `alphaBits` | Number of bits allocated to Alpha |
| `0x24` | `uint32_t` | `redMask` | Red channel bitmask |
| `0x28` | `uint32_t` | `redShift` | Red bit shift offset |
| `0x2C` | `uint32_t` | `redBits` | Number of bits allocated to Red |
| `0x30` | `uint32_t` | `greenMask` | Green channel bitmask |
| `0x34` | `uint32_t` | `greenShift` | Green bit shift offset |
| `0x38` | `uint32_t` | `greenBits` | Number of bits allocated to Green |
| `0x3C` | `uint32_t` | `blueMask` | Blue channel bitmask |
| `0x40` | `uint32_t` | `blueShift` | Blue bit shift offset |
| `0x44` | `uint32_t` | `blueBits` | Number of bits allocated to Blue |
| `0x48` | `uint32_t` | `reserved` | Reserved field (always 0) |

---

## Supported Formats & Channel Masks

| FourCC | bpp | Format Description | Alpha Mask/Shift/Bits | Red Mask/Shift/Bits | Green Mask/Shift/Bits | Blue Mask/Shift/Bits |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `DXT1` | 4 | S3TC DXT1 Compressed | `0x8000` / 15 / 1 | `0x7C00` / 10 / 5 | `0x03E0` / 5 / 5 | `0x001F` / 0 / 5 |
| `DXT3` | 8 | S3TC DXT3 Compressed | `0xF000` / 12 / 4 | `0x0F00` / 8 / 4 | `0x00F0` / 4 / 4 | `0x000F` / 0 / 4 |
| `QU\0\0`| 16 | Uncompressed RGBA 5551 | `0x8000` / 15 / 1 | `0x7C00` / 10 / 5 | `0x03E0` / 5 / 5 | `0x001F` / 0 / 5 |
| `PV\0\0`| 16 | Uncompressed RGB 565 | `0x0000` / 0 / 0 | `0xF800` / 11 / 5 | `0x07E0` / 5 / 6 | `0x001F` / 0 / 5 |
| `PNT3` | 32 | 32-bit BGRA with 16-byte aligned zero RLE | `0x0000` / 0 / 0 | `0x0000` / 0 / 0 | `0x0000` / 0 / 0 | `0x0000` / 0 / 0 |

---

## PNT3 Zero Run-Length Encoding Algorithm

`PNT3` textures store raw 32-bit BGRA (`B, G, R, A`) pixels with a 16-byte aligned run-length compression for transparent black pixels (`0x00000000`):

### Encoding Logic:
1. Walk input 32-bit pixels.
2. If pixel value is non-zero (or alpha > 0):
   - Write the 4-byte BGRA pixel directly to the output stream.
3. If pixel value is `0x00000000`:
   - If the current position in the uncompressed buffer is **not 16-byte aligned**, write literal `0x00000000` pixels until reaching a 16-byte alignment boundary.
   - Once 16-byte aligned, count the total contiguous zero bytes up to the next non-zero pixel.
   - Calculate `chunk16 = (zero_bytes / 16) * 16`.
   - Write `uint32_t chunk16` (the number of zero bytes in the run).
   - Advance position by `chunk16`.

### Decoding Logic:
1. Read 4-byte `uint32_t val`.
2. If `(out_bytes % 16 == 0)` and `val > 0` and `(val % 16 == 0)` and `(val >> 24 == 0)`:
   - Output `val` zero bytes (`0x00`).
3. Otherwise:
   - Output `val` as a 4-byte pixel.

---

## DDS $\leftrightarrow$ MMP Conversion Mapping

### DDS $\to$ MMP:
- Read 128-byte DirectDraw Surface (DDS) header.
- Detect format from `ddspf.dwFourCC` (`DXT1`, `DXT3`, `DXT5`) or uncompressed RGB bitmask.
- Construct the 76-byte MMP header with corresponding channel masks and bit shifts.
- If `PNT3`, encode the payload using aligned RLE. For all other formats, copy raw payload bytes directly.

### MMP $\to$ DDS:
- Read 76-byte MMP header.
- Construct 128-byte standard DDS header with `DDSD_CAPS`, `DDSD_WIDTH`, `DDSD_HEIGHT`, `DDSD_PIXELFORMAT`, and appropriate `dwFlags` / `dwCaps`.
- If `PNT3`, decode the RLE stream into full uncompressed 32-bit BGRA buffer. For all other formats, write payload bytes verbatim.
