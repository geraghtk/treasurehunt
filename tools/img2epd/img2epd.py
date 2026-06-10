"""Convert an image into a 1-bit PROGMEM byte array for the GxEPD2 e-paper display.

Output matches `display.drawInvertedBitmap(..., GxEPD_BLACK)`:
  - MSB-first bit packing
  - white = 1 bit, black = 0 bit
  - row stride = ceil(width / 8) bytes, trailing bits padded white (1)

Usage:
    python img2epd.py <image_path_or_filename> <array_name>
        [--mode dither|lineart] [--width 300] [--height 207]
        [--no-crop] [--replace] [--header PATH] [--preview]

If <image_path_or_filename> is bare (no path separator), the script looks in
`src/images_in/` relative to the project root.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from PIL import Image, ImageFilter, ImageOps

PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_INPUT_DIR = PROJECT_ROOT / "src" / "images_in"
DEFAULT_HEADER = PROJECT_ROOT / "src" / "ImageData.h"
DEFAULT_INO = PROJECT_ROOT / "src" / "BLE_TreasureHunt.ino"


def resolve_input(arg: str) -> Path:
    p = Path(arg)
    if not p.is_absolute() and "/" not in arg and "\\" not in arg:
        p = DEFAULT_INPUT_DIR / arg
    if not p.exists():
        sys.exit(f"error: input image not found: {p}")
    return p


def rotate_cw(img: Image.Image, degrees: int) -> Image.Image:
    """Rotate clockwise by 0/90/180/270 degrees (phone photos often come in sideways)."""
    if degrees == 0:
        return img
    transpose = {90: Image.ROTATE_270, 180: Image.ROTATE_180, 270: Image.ROTATE_90}[degrees]
    return img.transpose(transpose)


def autocrop_near_white(img: Image.Image, threshold: int = 200) -> Image.Image:
    """Crop off near-white margins. Source images often have off-white borders
    that a strict threshold misses."""
    gray = img.convert("L")
    w, h = gray.size
    px = gray.load()
    first_y = last_y = None
    first_x, last_x = w, 0
    for y in range(h):
        row_has_ink = False
        for x in range(w):
            if px[x, y] < threshold:
                row_has_ink = True
                if x < first_x:
                    first_x = x
                if x > last_x:
                    last_x = x
        if row_has_ink:
            if first_y is None:
                first_y = y
            last_y = y
    if first_y is None:
        return img
    return img.crop((first_x, first_y, last_x + 1, last_y + 1))


def to_dither(img: Image.Image, w: int, h: int) -> Image.Image:
    g = img.convert("L").resize((w, h), Image.LANCZOS)
    return g.convert("1", dither=Image.Dither.FLOYDSTEINBERG)


def equalize_tones(img: Image.Image) -> Image.Image:
    """Histogram-equalize the luminance. Rescues harsh/backlit photos whose crushed
    shadows would otherwise dither to solid black. Returns an L-mode image."""
    return ImageOps.equalize(img.convert("L"))


def to_threshold(img: Image.Image, w: int, h: int, level: int = 128) -> Image.Image:
    """Plain luminance threshold, no dithering or edge detection. The right mode for
    art that is ALREADY clean line work (e.g. an AI-generated line drawing): dark
    strokes -> black, paper -> white, strokes stay crisp single lines."""
    g = img.convert("L").resize((w, h), Image.LANCZOS)
    return g.point(lambda v: 255 if v >= level else 0, "L").convert("1")


def to_adaptive(img: Image.Image, w: int, h: int, block: int = 15, offset: float = 8.0) -> Image.Image:
    """Local adaptive threshold: a pixel goes black if it's darker than the average
    of its neighbourhood (a `block`x`block` window) by more than `offset`. Unlike a
    single global threshold, this keeps faint/thin lines that a fixed cutoff would
    drop, while flat white areas stay white — best detail retention for clean line
    art shrunk to e-paper size. Needs numpy + scipy."""
    import numpy as np
    from scipy.ndimage import uniform_filter

    g = img.convert("L").resize((w, h), Image.LANCZOS)
    a = np.asarray(g, dtype=float)
    local_mean = uniform_filter(a, size=block)
    out = np.where(a < (local_mean - offset), 0, 255).astype("uint8")
    return Image.fromarray(out, "L").convert("1")


def to_lineart(img: Image.Image, w: int, h: int) -> Image.Image:
    g = img.convert("L").resize((w, h), Image.LANCZOS)
    g = g.filter(ImageFilter.SMOOTH)
    edges = g.filter(ImageFilter.FIND_EDGES)
    inverted = ImageOps.invert(edges)
    return inverted.point(lambda v: 0 if v < 180 else 255, "1")


def to_canny(img: Image.Image, w: int, h: int,
             sigma: float = 1.4, low: float = 0.10, high: float = 0.22) -> Image.Image:
    """Canny edge detection: denoise, Sobel gradients, non-maximum suppression, and
    hysteresis thresholding. Identifies continuous lines and rejects isolated speckle,
    unlike the raw FIND_EDGES kernel in lineart mode. Needs numpy + scipy."""
    import numpy as np
    from scipy import ndimage

    g = img.convert("L").resize((w, h), Image.LANCZOS)
    arr = np.asarray(g, dtype=float) / 255.0
    sm = ndimage.gaussian_filter(arr, sigma)
    gx = ndimage.sobel(sm, axis=1)
    gy = ndimage.sobel(sm, axis=0)
    mag = np.hypot(gx, gy)
    mag /= mag.max() + 1e-12
    ang = np.rad2deg(np.arctan2(gy, gx))
    ang[ang < 0] += 180

    # Non-maximum suppression: thin each ridge to its peak along the gradient direction.
    nms = np.zeros_like(mag)
    H, W = mag.shape
    for i in range(1, H - 1):
        for j in range(1, W - 1):
            a, m = ang[i, j], mag[i, j]
            if a < 22.5 or a >= 157.5:
                n1, n2 = mag[i, j - 1], mag[i, j + 1]
            elif a < 67.5:
                n1, n2 = mag[i - 1, j + 1], mag[i + 1, j - 1]
            elif a < 112.5:
                n1, n2 = mag[i - 1, j], mag[i + 1, j]
            else:
                n1, n2 = mag[i - 1, j - 1], mag[i + 1, j + 1]
            if m >= n1 and m >= n2:
                nms[i, j] = m

    # Hysteresis: keep strong edges plus weak edges connected to a strong one.
    strong = nms >= high
    weak = nms >= low
    labels, _ = ndimage.label(weak)
    strong_labels = np.unique(labels[strong])
    strong_labels = strong_labels[strong_labels > 0]
    edges = np.isin(labels, strong_labels)

    out = np.where(edges, 0, 255).astype("uint8")  # black lines (0) on white (255)
    return Image.fromarray(out, "L").convert("1")


def pack_msb(img1bit: Image.Image) -> bytes:
    """Pack a 1-bit image MSB-first, white=1, padding bits=1."""
    w, h = img1bit.size
    px = img1bit.load()
    row_bytes = (w + 7) // 8
    out = bytearray(row_bytes * h)
    idx = 0
    for y in range(h):
        for bx in range(row_bytes):
            byte = 0
            for bit in range(8):
                x = bx * 8 + bit
                if x >= w or px[x, y]:
                    byte |= 1 << (7 - bit)
            out[idx] = byte
            idx += 1
    return bytes(out)


def emit_array(name: str, w: int, h: int, data: bytes) -> str:
    lines = [f"// '{name}', {w}x{h}px",
             f"const unsigned char epd_bitmap_{name} [] PROGMEM = {{"]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        body = ", ".join(f"0x{b:02x}" for b in chunk)
        suffix = "," if i + 16 < len(data) else ""
        lines.append(f"\t{body}{suffix} ")
    lines.append("};")
    return "\n".join(lines) + "\n"


def splice_into_header(header: Path, name: str, block: str, replace: bool) -> str:
    text = header.read_text()
    array_re = re.compile(
        rf"(?ms)^//\s*'{re.escape(name)}'.*?^const unsigned char epd_bitmap_{re.escape(name)}\s*\[\].*?^}};\s*\n",
    )
    decl_re = re.compile(rf"(?m)^const unsigned char epd_bitmap_{re.escape(name)}\s*\[\]")

    if decl_re.search(text):
        if not replace:
            sys.exit(f"error: epd_bitmap_{name} already exists in {header.name}; pass --replace to overwrite")
        new_text, n = array_re.subn(block, text, count=1)
        if n == 0:
            # declaration exists but our regex didn't match — bail rather than corrupt the file
            sys.exit(f"error: could not locate full block for epd_bitmap_{name}; edit by hand")
        action = "replaced"
    else:
        new_text = text.rstrip() + "\n\n" + block
        action = "appended"

    header.write_text(new_text)
    return action


def patch_clue(ino: Path, label: str, width: int, height: int, name: str) -> str:
    """Update the width, height, and bitmap reference for the clue whose
    trailing `// Label` comment matches `label` (case-insensitive)."""
    text = ino.read_text()
    lines = text.splitlines(keepends=True)
    label_re = re.compile(rf"//\s*{re.escape(label)}\b", re.IGNORECASE)
    field_re = re.compile(r"(\d+\s*,\s*\d+\s*,\s*epd_bitmap_)\w+(\s*\})")
    todo_re = re.compile(r"[ \t]*TODO:[ \t]*replace image[ \t]*", re.IGNORECASE)

    matches = [i for i, ln in enumerate(lines) if label_re.search(ln) and "{{0x" in ln]
    if not matches:
        sys.exit(f"error: no clue line found with label '{label}' in {ino.name}")
    if len(matches) > 1:
        sys.exit(f"error: multiple clue lines matched label '{label}'; refusing to guess")

    i = matches[0]
    new_line, n = field_re.subn(rf"{width}, {height}, epd_bitmap_{name}\2", lines[i], count=1)
    if n == 0:
        sys.exit(f"error: clue line for '{label}' did not match expected width/height/bitmap pattern")
    new_line = todo_re.sub(" ", new_line)
    if new_line == lines[i]:
        return f"clue '{label}' already references epd_bitmap_{name} at {width}x{height}"
    lines[i] = new_line
    ino.write_text("".join(lines))
    return f"patched clue '{label}' -> epd_bitmap_{name} ({width}x{height})"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="image filename (looked up in src/images_in/) or full path")
    ap.add_argument("name", help="array name suffix; emits epd_bitmap_<name>")
    ap.add_argument("--mode", choices=("dither", "lineart", "canny", "threshold", "adaptive"), default="dither")
    ap.add_argument("--threshold", type=int, default=128,
                    help="threshold mode: luminance cutoff 0-255 (lower = more white, higher = more black)")
    ap.add_argument("--block", type=int, default=15,
                    help="adaptive mode: neighbourhood window size in px (odd; larger = coarser)")
    ap.add_argument("--offset", type=float, default=8.0,
                    help="adaptive mode: how much darker than the local average a pixel must be to ink black")
    ap.add_argument("--sigma", type=float, default=1.4,
                    help="canny: Gaussian denoise strength (higher = fewer, cleaner lines)")
    ap.add_argument("--low", type=float, default=0.10, help="canny: low hysteresis threshold (0-1)")
    ap.add_argument("--high", type=float, default=0.22, help="canny: high hysteresis threshold (0-1)")
    ap.add_argument("--width", type=int, default=300)
    ap.add_argument("--height", type=int, default=207)
    ap.add_argument("--rotate", type=int, choices=(0, 90, 180, 270), default=0,
                    help="rotate source clockwise this many degrees before processing")
    ap.add_argument("--equalize", action="store_true",
                    help="histogram-equalize luminance before conversion (rescues backlit/dark photos)")
    ap.add_argument("--no-crop", action="store_true", help="skip near-white margin auto-crop")
    ap.add_argument("--replace", action="store_true", help="replace existing array of this name")
    ap.add_argument("--header", type=Path, default=DEFAULT_HEADER)
    ap.add_argument("--preview", action="store_true", help="also write a PNG preview next to the source")
    ap.add_argument("--clue", help="also wire this bitmap into the named clue (e.g. Dumbo) in BLE_TreasureHunt.ino")
    ap.add_argument("--ino", type=Path, default=DEFAULT_INO)
    args = ap.parse_args()

    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", args.name):
        sys.exit(f"error: name '{args.name}' is not a valid C identifier suffix")

    src = resolve_input(args.image)
    img = Image.open(src)
    img = rotate_cw(img, args.rotate)

    if not args.no_crop:
        img = autocrop_near_white(img)

    if args.equalize:
        img = equalize_tones(img)

    if args.mode == "dither":
        bw = to_dither(img, args.width, args.height)
    elif args.mode == "lineart":
        bw = to_lineart(img, args.width, args.height)
    elif args.mode == "threshold":
        bw = to_threshold(img, args.width, args.height, args.threshold)
    elif args.mode == "adaptive":
        bw = to_adaptive(img, args.width, args.height, args.block, args.offset)
    else:
        bw = to_canny(img, args.width, args.height, args.sigma, args.low, args.high)

    if args.preview:
        preview_path = src.with_name(f"{src.stem}.preview.png")
        bw.save(preview_path)
        print(f"preview: {preview_path}")

    data = pack_msb(bw)
    expected = ((args.width + 7) // 8) * args.height
    assert len(data) == expected, f"size mismatch: got {len(data)}, expected {expected}"

    block = emit_array(args.name, args.width, args.height, data)
    action = splice_into_header(args.header, args.name, block, args.replace)
    print(f"{action} epd_bitmap_{args.name} ({args.width}x{args.height}, {len(data)} bytes) in {args.header}")

    if args.clue:
        msg = patch_clue(args.ino, args.clue, args.width, args.height, args.name)
        print(msg)


if __name__ == "__main__":
    main()
