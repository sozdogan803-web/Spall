"""Generates the Spall brand files (run from anywhere; needs Pillow for the .ico only):

    docs/brand/spall-icon.svg   the app icon: a steel plate seen from the front, its hole petalled
    docs/brand/spall-logo.svg   the icon with the wordmark
    docs/brand/spall-side.svg   a side view of the same event: a rod meeting a plate
    res/spall.ico               the icon at 16 to 256 px, linked into the exe by res/spall.rc
"""
import math, os, struct, io

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def star(cx, cy, outs, r_in, rot):
    """Alternating outer and inner points: one outer point per entry of `outs`."""
    n = len(outs)
    pts = []
    for i, ro in enumerate(outs):
        a = rot + 2 * math.pi * i / n
        pts.append((cx + ro * math.cos(a), cy + ro * math.sin(a)))
        a2 = a + math.pi / n
        pts.append((cx + r_in * math.cos(a2), cy + r_in * math.sin(a2)))
    return pts


# The icon as shapes on a 256 grid, so the SVG and the .ico stay the same picture.
HOLE = star(128, 128, [92, 72, 96, 76, 88, 68, 92], 36.8, -math.pi / 2)
ICON = [
    ("rrect", 0, 0, 256, 256, 56, "#161a22", None, 0),
    ("rrect", 22, 22, 212, 212, 22, "#9aa5b4", "#c9d1db", 5),
    ("circle", 52, 52, 9, "#6f7a89"), ("circle", 204, 52, 9, "#6f7a89"),
    ("circle", 52, 204, 9, "#6f7a89"), ("circle", 204, 204, 9, "#6f7a89"),
    ("poly", HOLE, "#0d1016"),
    ("circle", 128, 128, 34, "#e8590c"),
    ("circle", 128, 128, 25, "#ff8a1f"),
    ("circle", 124, 124, 13, "#ffd34d"),
]


def num(v):
    return ("%.1f" % v).rstrip("0").rstrip(".")


def svg_shape(s):
    if s[0] == "rrect":
        _, x, y, w, h, r, fill, stroke, sw = s
        out = '<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="%s"/>' % (num(x), num(y), num(w), num(h), num(r), fill)
        if stroke:
            out += ('\n  <rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="none" stroke="%s" stroke-width="%s"/>'
                    % (num(x), num(y), num(w), num(h), num(r), stroke, num(sw)))
        return out
    if s[0] == "circle":
        return '<circle cx="%s" cy="%s" r="%s" fill="%s"/>' % (num(s[1]), num(s[2]), num(s[3]), s[4])
    return '<polygon points="%s" fill="%s"/>' % (" ".join("%s,%s" % (num(x), num(y)) for x, y in s[1]), s[2])


def icon_body():
    return "\n  ".join(svg_shape(s) for s in ICON)


SIDE = """<rect width="256" height="256" rx="56" fill="#161a22"/>
  <path d="M92 26H156V90L120 128L156 166V230H92Z" fill="#aab4c2"/>
  <path d="M92 26H112V230H92Z" fill="#dfe5ee"/>
  <path d="M12 112H108L128 128L108 144H12Z" fill="#ff8a1f"/>
  <path d="M12 112H108L118 120H12Z" fill="#ffc24d"/>
  <path d="M176 92L224 64L240 142Z" fill="#cfd6e0"/>
  <path d="M224 64L240 142L214 118Z" fill="#8b96a6"/>
  <path d="M176 92L224 64L206 100Z" fill="#f4f7fb"/>
  <path d="M196 182L214 170L218 194Z" fill="#ff8a1f"/>
  <path d="M170 196L184 188L186 204Z" fill="#ffc24d"/>
  <path d="M232 40L244 34L242 50Z" fill="#ffc24d"/>"""


def write(path, text):
    with open(path, "w", newline="\n") as f:
        f.write(text)


def write_svgs():
    write(os.path.join(HERE, "spall-icon.svg"),
          '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" role="img" aria-label="Spall">\n'
          '  <title>Spall</title>\n  %s\n</svg>\n' % icon_body())
    write(os.path.join(HERE, "spall-side.svg"),
          '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" role="img" aria-label="A rod meeting a plate">\n'
          '  <title>Spall, side view</title>\n  %s\n</svg>\n' % SIDE)
    write(os.path.join(HERE, "spall-logo.svg"),
          '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 640 256" role="img" aria-label="Spall">\n'
          '  <title>Spall</title>\n'
          '  <rect width="640" height="256" rx="40" fill="#0c0e12"/>\n'
          '  <g transform="translate(20 20) scale(0.859)">\n  %s\n  </g>\n'
          '  <text x="262" y="150" font-family="Segoe UI, Helvetica Neue, Arial, sans-serif" font-size="104" '
          'font-weight="700" letter-spacing="-1" fill="#eef1f6">Spall</text>\n'
          '  <text x="266" y="192" font-family="Segoe UI, Helvetica Neue, Arial, sans-serif" font-size="17" '
          'letter-spacing="2.4" fill="#8a93a3">ARMOUR VS PROJECTILE, SIMULATED</text>\n'
          '</svg>\n' % icon_body())


def render(px):
    """The icon as an RGBA Pillow image, px by px, drawn at 8x and averaged down."""
    from PIL import Image, ImageDraw
    ss = 8
    big = px * ss
    k = big / 256.0
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for s in ICON:
        if s[0] == "rrect":
            _, x, y, w, h, r, fill, stroke, sw = s
            if stroke:   # an SVG stroke straddles the edge: outer shape in the stroke colour, then the fill inside
                h2 = sw / 2.0
                d.rounded_rectangle([(x - h2) * k, (y - h2) * k, (x + w + h2) * k - 1, (y + h + h2) * k - 1],
                                    radius=(r + h2) * k, fill=stroke)
                d.rounded_rectangle([(x + h2) * k, (y + h2) * k, (x + w - h2) * k - 1, (y + h - h2) * k - 1],
                                    radius=(r - h2) * k, fill=fill)
            else:
                d.rounded_rectangle([x * k, y * k, (x + w) * k - 1, (y + h) * k - 1], radius=r * k, fill=fill)
        elif s[0] == "circle":
            _, cx, cy, r, fill = s
            d.ellipse([(cx - r) * k, (cy - r) * k, (cx + r) * k - 1, (cy + r) * k - 1], fill=fill)
        else:
            d.polygon([(x * k, y * k) for x, y in s[1]], fill=s[2])
    return img.resize((px, px), Image.LANCZOS)


def write_ico(path, sizes=(16, 24, 32, 48, 64, 256)):
    blobs = []
    for px in sizes:
        buf = io.BytesIO()
        render(px).save(buf, format="PNG")
        blobs.append(buf.getvalue())
    out = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    for px, blob in zip(sizes, blobs):
        b = 0 if px >= 256 else px
        out += struct.pack("<BBBBHHII", b, b, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(out + b"".join(blobs))


if __name__ == "__main__":
    write_svgs()
    write_ico(os.path.join(ROOT, "res", "spall.ico"))
    print("ok")
