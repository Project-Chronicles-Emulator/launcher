"""Render the launcher's existing gold compass as a Windows multi-resolution icon.

Run manually with Pillow installed; normal launcher builds use the checked-in ICO.
The compass geometry follows the symbol in launcher/web/index.html.
"""

from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
ICON_PATH = ROOT / "launcher" / "resources" / "chronicles.ico"
ICON_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
RENDER_SIZE = 1024
GOLD = "#e5bf72"
NAVY = "#091923"


def render_compass():
    image = Image.new("RGBA", (RENDER_SIZE, RENDER_SIZE))
    drawing = ImageDraw.Draw(image)
    scale = RENDER_SIZE / 64

    def points(coordinates):
        return [(round(x * scale), round(y * scale)) for x, y in coordinates]

    drawing.rounded_rectangle(
        (0, 0, RENDER_SIZE - 1, RENDER_SIZE - 1),
        radius=round(12 * scale),
        fill=NAVY,
    )
    for radius in (22, 18):
        drawing.ellipse(
            [(32 - radius) * scale] * 2 + [(32 + radius) * scale] * 2,
            outline=GOLD,
            width=round(1.4 * scale),
        )

    outlines = (
        ((32, 2), (37, 27), (62, 32), (37, 37), (32, 62), (27, 37), (2, 32), (27, 27)),
        ((15, 15), (32, 27), (49, 15), (37, 32), (49, 49), (32, 37), (15, 49), (27, 32)),
    )
    for outline in outlines:
        drawing.line(
            points((*outline, outline[0])),
            fill=GOLD,
            width=round(1.4 * scale),
            joint="curve",
        )

    return image


def main():
    ICON_PATH.parent.mkdir(parents=True, exist_ok=True)
    image = render_compass().resize((256, 256), Image.Resampling.LANCZOS)
    image.save(ICON_PATH, sizes=[(size, size) for size in ICON_SIZES])
    print(f"Wrote {ICON_PATH}")


if __name__ == "__main__":
    main()
