"""Extract animation frames from a GIF and pack them into a sprite sheet
suitable for actor animations in the engine.

Mirrors the conventions of cut_tileset.py:
  * Frames are normalized to a fixed square size (default 40x40).
  * Pixel art is preserved with nearest-neighbor scaling.
  * Transparency is preserved when present; otherwise the first pixel of
    the first frame is treated as the color-key (typical for old GIFs).

Outputs:
  * A single packed PNG sprite sheet at <out_dir>/<name>.png
  * A folder <out_dir>/<name>_frames/ with each frame as <name>_<i>.png
    (handy for reviewing or hand-tweaking individual frames).

Usage:
    python cut_frames_from_gif.py
        # uses the defaults at the bottom of this file
"""

from __future__ import annotations

from pathlib import Path
from typing import List, Optional, Tuple

from PIL import Image, ImageSequence


def _color_key_to_alpha(rgba: Image.Image, key_rgb: Tuple[int, int, int],
                        tolerance: int = 0) -> Image.Image:
    """Return a copy of `rgba` with pixels matching `key_rgb` set transparent.

    `tolerance` is a per-channel +/- range; 0 means strict equality. Useful
    when the GIF was saved with very slight palette drift around the
    background color.
    """
    out = rgba.copy()
    pixels = out.load()
    kr, kg, kb = key_rgb
    w, h = out.size
    for y in range(h):
        for x in range(w):
            r, g, b, _ = pixels[x, y]
            if (abs(r - kr) <= tolerance
                    and abs(g - kg) <= tolerance
                    and abs(b - kb) <= tolerance):
                pixels[x, y] = (r, g, b, 0)
    return out


def _fit_frame_to_size(frame: Image.Image, target_size: int,
                       trim_alpha: bool) -> Image.Image:
    """Trim transparent border, scale (NEAREST) to fit `target_size` square,
    centered on a transparent canvas. Mirrors fit_tile_to_size in
    cut_tileset.py.
    """
    rgba = frame.convert("RGBA")

    if trim_alpha:
        alpha = rgba.getchannel("A")
        bbox = alpha.getbbox()
        if bbox is not None:
            rgba = rgba.crop(bbox)

    src_w, src_h = rgba.size
    if src_w <= 0 or src_h <= 0:
        return Image.new("RGBA", (target_size, target_size), (0, 0, 0, 0))

    scale = min(target_size / src_w, target_size / src_h)
    new_w = max(1, int(round(src_w * scale)))
    new_h = max(1, int(round(src_h * scale)))

    resized = rgba.resize((new_w, new_h), Image.Resampling.NEAREST)

    canvas = Image.new("RGBA", (target_size, target_size), (0, 0, 0, 0))
    offset_x = (target_size - new_w) // 2
    offset_y = (target_size - new_h) // 2
    canvas.paste(resized, (offset_x, offset_y), resized)
    return canvas


def extract_frames(gif_path: Path,
                   color_key: Optional[Tuple[int, int, int]] = "auto",
                   color_key_tolerance: int = 0) -> List[Image.Image]:
    """Walk every frame of a GIF and return them as a list of full-frame
    RGBA Images, with disposal applied.

    `color_key`:
      * (r, g, b) tuple   — force this color to transparent.
      * "auto"            — sample the top-left pixel of frame 0 and key
                            that color, but only if the GIF has no real
                            alpha channel (i.e. every pixel is opaque).
      * None              — leave colors alone.
    """
    img = Image.open(gif_path)
    frames: List[Image.Image] = []

    # Walk frames first; ImageSequence handles disposal across frames so we
    # don't end up with leftover content from previous frames.
    for src in ImageSequence.Iterator(img):
        frames.append(src.convert("RGBA"))

    if not frames:
        return frames

    if color_key == "auto":
        # Decide whether to key: only if no frame has any transparent pixel.
        any_transparent = any(
            any(p[3] != 255 for p in f.getdata())
            for f in frames
        )
        if any_transparent:
            color_key = None
        else:
            r, g, b, _ = frames[0].getpixel((0, 0))
            color_key = (r, g, b)

    if color_key is not None:
        frames = [
            _color_key_to_alpha(f, color_key, color_key_tolerance)
            for f in frames
        ]

    return frames


def cut_frames_from_gif(gif_path: Path,
                        output_dir: Path,
                        sheet_name: Optional[str] = None,
                        frame_size: int = 40,
                        columns: Optional[int] = None,
                        trim_alpha: bool = True,
                        color_key: Optional[Tuple[int, int, int]] = "auto",
                        color_key_tolerance: int = 0,
                        write_individual_frames: bool = True) -> Path:
    """Extract every frame of `gif_path`, normalize each into a
    `frame_size x frame_size` square, and pack them into a single PNG
    sprite sheet under `output_dir`.

    Returns the path to the packed sheet.

    Args:
        gif_path: Source .gif file.
        output_dir: Where to drop the sheet (and per-frame PNGs).
        sheet_name: Output basename (without extension). Defaults to the
            GIF stem.
        frame_size: Output cell size in pixels (default 40 to match the
            engine's actor sprite layout).
        columns: How many frames per row in the sheet. None packs them in
            a single row, which is what the engine's MarioSheet loader
            expects.
        trim_alpha: If True, each frame's transparent border is cropped
            before scaling, so the visual content fills the cell.
        color_key: See extract_frames.
        write_individual_frames: Also dump each frame as its own PNG into
            `<output_dir>/<sheet_name>_frames/`.
    """
    output_dir.mkdir(parents=True, exist_ok=True)
    if sheet_name is None:
        sheet_name = gif_path.stem

    raw_frames = extract_frames(gif_path,
                                color_key=color_key,
                                color_key_tolerance=color_key_tolerance)
    if not raw_frames:
        raise ValueError(f"No frames extracted from {gif_path}")

    fitted = [_fit_frame_to_size(f, frame_size, trim_alpha) for f in raw_frames]

    if columns is None or columns <= 0:
        columns = len(fitted)
    rows = (len(fitted) + columns - 1) // columns

    sheet = Image.new(
        "RGBA",
        (columns * frame_size, rows * frame_size),
        (0, 0, 0, 0),
    )
    for i, frame in enumerate(fitted):
        cx = (i % columns) * frame_size
        cy = (i // columns) * frame_size
        sheet.paste(frame, (cx, cy), frame)

    sheet_path = output_dir / f"{sheet_name}.png"
    sheet.save(sheet_path)

    if write_individual_frames:
        frames_dir = output_dir / f"{sheet_name}_frames"
        frames_dir.mkdir(parents=True, exist_ok=True)
        for i, frame in enumerate(fitted):
            frame.save(frames_dir / f"{sheet_name}_{i:02d}.png")

    print(f"[cut_frames_from_gif] {gif_path.name}: "
          f"{len(fitted)} frames -> {sheet_path} "
          f"({sheet.width}x{sheet.height})")
    return sheet_path


if __name__ == "__main__":
    cur = Path(__file__).parent
    out = cur.parent / "Sprites"

    cut_frames_from_gif(
        gif_path=cur / "tmp/SMB3_BigKoopaTroopaGreenL.gif",
        output_dir=out,
        sheet_name="koopa",
        frame_size=40,
        columns=None,        # one row, like luigi.png
        trim_alpha=True,
        color_key="auto",    # detects magenta-style background automatically
    )

    cut_frames_from_gif(
        gif_path=cur / "tmp/enemies-5.gif",
        output_dir=out,
        sheet_name="enemies5",
        frame_size=40,
        columns=None,
        trim_alpha=True,
        color_key="auto",
    )
