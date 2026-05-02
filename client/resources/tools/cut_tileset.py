
from pathlib import Path
from PIL import Image

def fit_tile_to_size(tile_image: Image.Image, target_size: int) -> Image.Image:
    # Work in RGBA so transparency-aware trimming works consistently.
    rgba = tile_image.convert("RGBA")

    # Trim empty transparent border, if present.
    alpha = rgba.getchannel("A")
    bbox = alpha.getbbox()
    if bbox is not None:
        rgba = rgba.crop(bbox)

    src_w, src_h = rgba.size
    if src_w <= 0 or src_h <= 0:
        return Image.new("RGBA", (target_size, target_size), (0, 0, 0, 0))

    # Preserve aspect ratio and fit within target_size x target_size.
    scale = min(target_size / src_w, target_size / src_h)
    new_w = max(1, int(round(src_w * scale)))
    new_h = max(1, int(round(src_h * scale)))

    # Use nearest-neighbor for crisp pixel-art tiles.
    resized = rgba.resize((new_w, new_h), Image.Resampling.NEAREST)

    canvas = Image.new("RGBA", (target_size, target_size), (0, 0, 0, 0))
    offset_x = (target_size - new_w) // 2
    offset_y = (target_size - new_h) // 2
    canvas.paste(resized, (offset_x, offset_y), resized)
    return canvas


def cut_tileset(tileset_path: Path, tile_size: int, output_dir: Path, output_tile_size: int = 40):
    # Open the tileset image
    tileset_image = Image.open(tileset_path)

    # Calculate the number of tiles in both dimensions
    tiles_x = tileset_image.width // tile_size
    tiles_y = tileset_image.height // tile_size

    # Create the output directory if it doesn't exist
    output_dir.mkdir(parents=True, exist_ok=True)

    # Loop through each tile and save it as a separate image
    for y in range(tiles_y):
        for x in range(tiles_x):
            # Calculate the bounding box for the current tile
            left = x * tile_size
            upper = y * tile_size
            right = left + tile_size
            lower = upper + tile_size

            # Crop the tile from the tileset image
            tile_image = tileset_image.crop((left, upper, right, lower))

            # Normalize each tile into a fixed output_tile_size square.
            tile_image = fit_tile_to_size(tile_image, output_tile_size)

            # Save the tile image to the output directory
            tile_filename = f"tile_{y}_{x}.png"
            tile_image.save(output_dir / tile_filename)
if __name__ == "__main__":
    cur = Path(__file__).parent
    tileset_path = cur / "tmp/luigi.png"
    tile_size = 40  # Size of each tile in pixels
    output_dir = cur / "luigi"

    cut_tileset(tileset_path, tile_size, output_dir, output_tile_size=40)