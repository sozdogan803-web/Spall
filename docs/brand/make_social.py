"""Builds the 1280x640 GitHub social preview from the icon and a real capture of the app.

Usage: python docs/brand/make_social.py <capture.png> [out.png]
The capture is a full-UI screenshot (Spall.exe --capture ... --size 1280x720); its cross-section
panel is cropped and shown beside the wordmark.
"""
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

here = pathlib.Path(__file__).parent
sys.path.insert(0, str(here))
import make_logo  # noqa: E402  (reuses the icon renderer)

W, H = 1280, 640
BG = (22, 26, 34)


def font(size, bold=False):
    for name in ("segoeuib.ttf" if bold else "segoeui.ttf", "arialbd.ttf" if bold else "arial.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def main():
    capture = pathlib.Path(sys.argv[1])
    out = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else here / "spall-social.png"
    shot = Image.open(capture).convert("RGB")
    # The cross-section panel of a 1280x720 capture.
    panel = shot.crop((400, 96, 906, 396))
    panel = panel.resize((int(panel.width * 1.15), int(panel.height * 1.15)), Image.LANCZOS)

    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    icon = make_logo.render(200)
    img.paste(icon, (72, 96), icon)
    d.text((72, 322), "Spall", font=font(96, True), fill=(232, 236, 242))
    d.text((76, 440), "Armour vs projectile terminal ballistics", font=font(26), fill=(154, 165, 180))
    d.text((76, 484), "2D plane-strain deformation on the GPU", font=font(26), fill=(154, 165, 180))
    d.text((76, 560), "Windows  ·  OpenGL 4.6  ·  MIT", font=font(24), fill=(111, 122, 137))

    px, py = 662, (H - panel.height) // 2
    d.rounded_rectangle((px - 6, py - 6, px + panel.width + 6, py + panel.height + 6), 10, fill=(60, 68, 82))
    img.paste(panel, (px, py))
    img.save(out)
    print(out, img.size)


if __name__ == "__main__":
    main()
