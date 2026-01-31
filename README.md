# DTNo1ResPacker

`DTNo1ResPacker` is a C++/CMake command-line tool that **generates a GenClock V3 watchface `.res` file** from:

- a **thumbnail image** (`--thumbnail`)
- a **source folder** (`--src`) that contains:
  - `config.json`
  - the referenced image assets (equivalent to the `chunks_decoded/` folder produced by the Python unpacker)

It is intended as the “inverse” of the unpacker workflow: `config.json + images → .res`.

Download pre-built binaries from the [releases page](https://github.com/mondul/DTNo1ResPacker/releases/latest). Available for Windows x64 and macOS x64/ARM64 (**NOTE**: In macOS you might need to run `xattr -d com.apple.quarantine DTNo1ResPacker` after extracting the downloaded zip).

---

## What it builds

The output `_res` file contains:

1. A fixed-size header with:
   - `magic`
   - `clock_id`
   - section offsets/lengths for thumbnail, images, (optional) z-images, and layer data

2. A **thumbnail chunk** embedded in the res

3. An **image section** containing per-image “chunks” with a 16-byte header followed by payload

4. A **layer_data section** serialized from `config.json`

This follows the same overall structure used by the reference Python generator (`gen_clock_v3.00.12.py`).

---

## Requirements

- CMake: tested with **CMake 4.2.x**
- C++ compiler supporting **C++20**
- Works on Linux/macOS; Windows should work with minor adjustments

Dependencies (auto-fetched via CMake `FetchContent`):

- **nlohmann/json**
- **LZ4** (built locally from source; avoids fragile upstream target naming)

Image loading/writing:

- Uses stb headers (`stb_image.h` for loading, `stb_image_write.h` for writing).  
  These are included in the repository under `src/third_party/` (or the equivalent location used by the project).

---

## Build

From project root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j
```

> Note: `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` is helpful with newer CMake versions in environments that enforce policy minimums.

---

## Usage

```bash
./build/DTNo1ResPacker   --thumbnail <image>   --src <folder>   [--output-folder <folder>]   [--clock-id <n>]   [--idle]   [--in]   [--no-lz4]
```

### Required inputs

- `--thumbnail <image>`
  - Thumbnail image file.
  - Must be **exactly**:
    - **300 × 300**, or
    - **210 × 256**

- `--src <folder>`
  - Folder containing:
    - `config.json`
    - referenced images (e.g. `chunk_000.png`, etc.)

### Output behavior

- The output filename is **always**:

  `Clock{id}_res`

  Example: `Clock50000_res`

- The output folder is controlled by:

  `--output-folder <folder>`

  If omitted, it defaults to the parent folder of `--src`.

### Clock id rules (important)

- The “base id” must be in range:

  **50000..65535 (inclusive)**

- The tool determines the base id as:
  1. `--clock-id <n>` if provided, else
  2. digits extracted from the `--src` folder name

If no digits exist in the `--src` folder name, you must pass `--clock-id`.

### Flags

- `--idle`
  - Writes the alternate magic string used for idle resources (`II@*24dG`), otherwise the default magic is used (`Sb@*O2GG`).

- `--in`
  - Sets the high bit (`0x80000000`) on the final `clock_id` (used by “built-in” resources in the reference generator but not active).

- `--no-lz4`
  - Disables LZ4 compression attempts for RGB payload chunks.

### Example

```bash
./build/DTNo1ResPacker   --thumbnail thumb.png   --src ./Clock50000_chunks_decoded   --output-folder ./out   --idle   --in
```

This produces:

`./out/Clock50000_res`

---

## How watchface resolution is determined

The generator needs the watch resolution (e.g. `466x466`) to pick the correct `clock_id` prefix.

`DTNo1ResPacker` determines resolution by:

1. Parsing `config.json`
2. Finding the **first referenced image file** in the config
3. Loading it and using its width/height as the **watchface resolution**

If the first referenced image cannot be found or loaded, generation fails.

---

## Layer fields: interval and area_num[]

Some watchfaces include extra per-layer fields beyond:

`drawType, dataType, alignType, x, y, num, imgArr`

This project supports the fields used by the reference generator:

- `interval`  
  Required when `dataType` is one of: **130, 59, 52**.

- `area_num` (array of int32)  
  Required when `dataType` is **112**.  
  All integers in the array are written as big-endian int32, in order (no count field is stored in the binary).

If a layer has one of those `dataType` values but the required field is missing, generation fails fast with a clear error.

---

## Compression behavior (LZ4)

Some chunk payload types can be stored compressed using LZ4 (similar to the format used by the unpacker):

- If a chunk is marked as compressed (`flag=1`), the chunk header includes the **decompressed** payload size,
  and the stored payload is LZ4-compressed bytes.
- The unpacker side expects to decompress to exactly that declared size.

By default, `DTNo1ResPacker` compresses eligible payloads with **LZ4-HC maximum level** when doing so reduces size.
If compression does not reduce size, it falls back to storing uncompressed payload.

Disable with `--no-lz4`.

---

## Notes about image formats and fidelity

### The key point

A `chunks_decoded/` folder **does not always contain enough metadata** to recreate the *original* encoding choices.

The generator can always make a valid `.res`, but **byte-for-byte parity** with the original `.res` may require extra information.

### What can be reproduced directly

- JPEG and GIF images can be embedded as-is, if the assets are already in those formats.
- PNG or other formats can be converted into “raw RGB” payload formats and then embedded.

### When extra metadata might be needed

If you need *exact* replication of original packing decisions, you may need one or more of these:

1. **Per-image intended payload type**
   - Example: whether an image should be stored as:
     - BGRA8888
     - RGB565 / ARGB1555 / etc.
     - indexed/palettized + bitmap
   - The decoded folder usually contains only PNG/JPG outputs, not “what the original `img_type` was”.

2. **Interval and area data**
   - Supported when present in `config.json` as described above.
   - If your source pipeline omits these fields but a watchface requires them, you’ll need to supply them.

---

## Troubleshooting

### Thumbnail size errors

`--thumbnail` must be exactly:
- 300×300, or
- 210×256

Anything else fails fast.

### Missing assets

If any filename referenced by `config.json` is missing from `--src`, generation will fail.

### Clock id errors

- Ensure base id is **50000..65535**.
- If your `--src` folder name does not contain digits, pass `--clock-id`.

---

## License / attribution

This project is a reimplementation for interoperability and tooling, based on publicly available scripts created and/or shared by [dipcore](https://github.com/dipcore):
- Unpacker script: [`unpack.py`](https://github.com/dipcore/unpack_clock_res/blob/main/unpack.py)
- Generator reference: [`gen_clock_v3.00.12.py`](https://gist.github.com/dipcore/26a8d0d6508675e5815087398f14499c)

The stb/LZ4/nlohmann dependencies are covered by their respective upstream licenses.
