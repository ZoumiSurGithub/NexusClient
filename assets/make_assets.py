import math, random, sys
from PIL import Image, ImageDraw, ImageFilter

OUT = sys.argv[1]

def lerp(a, b, t): return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))

# ---------------------------------------------------------------- Icône
def hexagon(cx, cy, r, rot=math.pi/6):
    return [(cx + r*math.cos(rot + i*math.pi/3), cy + r*math.sin(rot + i*math.pi/3)) for i in range(6)]

def make_icon(size=1024):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    c = size / 2
    # dégradé violet dans un hexagone
    grad = Image.new("RGBA", (size, size))
    gd = ImageDraw.Draw(grad)
    top, bottom = (176, 140, 255), (88, 44, 196)
    for y in range(size):
        gd.line([(0, y), (size, y)], fill=lerp(top, bottom, y / size) + (255,))
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).polygon(hexagon(c, c, size*0.47), fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    # liseré clair + ombre intérieure
    d.polygon(hexagon(c, c, size*0.47), outline=(214, 196, 255, 255), width=int(size*0.022))
    d.polygon(hexagon(c, c, size*0.40), outline=(255, 255, 255, 38), width=int(size*0.012))
    # N géométrique
    w = size*0.085
    x0, x1 = c - size*0.17, c + size*0.17
    y0, y1 = c - size*0.20, c + size*0.20
    white = (255, 255, 255, 255)
    for seg in [((x0, y1), (x0, y0)), ((x0, y0), (x1, y1)), ((x1, y1), (x1, y0))]:
        d.line(seg, fill=white, width=int(w))
    for p in [(x0, y0), (x0, y1), (x1, y0), (x1, y1)]:
        r = w/2
        d.rectangle([p[0]-r, p[1]-r, p[0]+r, p[1]+r], fill=white)
    return img

icon = make_icon()
icon.resize((512, 512), Image.LANCZOS).save(f"{OUT}/nexus_icon.png")
icon.resize((256, 256), Image.LANCZOS).save(f"{OUT}/nexus.ico",
    sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])

# ---------------------------------------------------------------- Logo titre
GLYPHS = {
    "N": ["X...X","XX..X","XX..X","X.X.X","X..XX","X..XX","X...X"],
    "E": ["XXXXX","X....","X....","XXXX.","X....","X....","XXXXX"],
    "X": ["X...X","X...X",".X.X.","..X..",".X.X.","X...X","X...X"],
    "U": ["X...X","X...X","X...X","X...X","X...X","X...X",".XXX."],
    "S": [".XXXX","X....","X....",".XXX.","....X","....X","XXXX."],
}
word = "NEXUS"
cells = set()
col = 0
for ch in word:
    g = GLYPHS[ch]
    for y, row in enumerate(g):
        for x, v in enumerate(row):
            if v == "X": cells.add((col + x, y))
    col += 6
cols, rows = col - 1, 7

W, H = 2048, 512
b = 52                  # taille d'un bloc
depth = 5               # extrusion 3D (en px * 3)
gw, gh = cols*b, rows*b
ox, oy = (W - gw)//2 - depth*3//2, (H - gh)//2 - depth*3//2

logo = Image.new("RGBA", (W, H), (0, 0, 0, 0))
d = ImageDraw.Draw(logo)
rnd = random.Random(7)

# ombre portée douce
shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
sd = ImageDraw.Draw(shadow)
for (x, y) in cells:
    sd.rectangle([ox + x*b + 26, oy + y*b + 30, ox + (x+1)*b + 26, oy + (y+1)*b + 30], fill=(10, 0, 30, 150))
shadow = shadow.filter(ImageFilter.GaussianBlur(14))
logo.alpha_composite(shadow)

# extrusion (côtés sombres)
for k in range(depth*3, 0, -1):
    shade = lerp((40, 16, 92), (66, 30, 140), 1 - k/(depth*3))
    for (x, y) in cells:
        d.rectangle([ox + x*b + k, oy + y*b + k, ox + (x+1)*b + k - 1, oy + (y+1)*b + k - 1], fill=shade + (255,))

# faces avant : dégradé vertical + texture de bloc
top, mid, bot = (226, 210, 255), (160, 118, 255), (98, 52, 214)
for (x, y) in cells:
    x0, y0 = ox + x*b, oy + y*b
    for py in range(b):
        t = (y*b + py) / gh
        c = lerp(top, mid, t*2) if t < 0.5 else lerp(mid, bot, (t-0.5)*2)
        d.line([(x0, y0 + py), (x0 + b - 1, y0 + py)], fill=c + (255,))
    # petits pixels de texture (style bloc), réguliers
    px = b // 6
    for i in range(6):
        for j in range(6):
            v = rnd.randint(-14, 14)
            if abs(v) > 8:
                bx, by = x0 + i*px, y0 + j*px
                base = logo.getpixel((bx + px//2, by + px//2))
                cc = tuple(max(0, min(255, base[k] + v)) for k in range(3)) + (255,)
                d.rectangle([bx, by, bx + px - 1, by + px - 1], fill=cc)
    # reflets / arêtes
    if (x, y-1) not in cells: d.rectangle([x0, y0, x0 + b - 1, y0 + 5], fill=(245, 238, 255, 255))
    if (x-1, y) not in cells: d.rectangle([x0, y0, x0 + 4, y0 + b - 1], fill=(236, 226, 255, 200))
    if (x, y+1) not in cells: d.rectangle([x0, y0 + b - 6, x0 + b - 1, y0 + b - 1], fill=(70, 34, 160, 255))
    if (x+1, y) not in cells: d.rectangle([x0 + b - 5, y0, x0 + b - 1, y0 + b - 1], fill=(84, 42, 180, 255))

logo.save(f"{OUT}/title.png")
logo.resize((1024, 256), Image.LANCZOS).save(f"{OUT}/title_preview.png")
print("ok")
