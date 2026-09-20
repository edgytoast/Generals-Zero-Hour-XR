#!/usr/bin/env python3
"""Generate the placeholder visionOS app icon (3 stacked 1024x1024 layers).

Output: visionos/Resources/Assets.xcassets/AppIcon.solidimagestack/...
Replace with real artwork later; the layer structure stays the same.
"""
import json, os, sys
from PIL import Image, ImageDraw

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "visionos", "Resources", "Assets.xcassets")
root = os.path.normpath(root)
stack = os.path.join(root, "AppIcon.solidimagestack")
S = 1024

def write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")

def layer(name, img):
    lp = os.path.join(stack, f"{name}.solidimagestacklayer")
    write_json(os.path.join(lp, "Contents.json"), {"info": {"author": "xcode", "version": 1}})
    ip = os.path.join(lp, "Content.imageset")
    os.makedirs(ip, exist_ok=True)
    img.save(os.path.join(ip, f"{name.lower()}.png"))
    write_json(os.path.join(ip, "Contents.json"), {
        "images": [{"filename": f"{name.lower()}.png", "idiom": "vision", "scale": "2x"}],
        "info": {"author": "xcode", "version": 1},
    })

# Back: opaque olive-to-dark gradient.
back = Image.new("RGB", (S, S))
px = back.load()
for y in range(S):
    for x in range(S):
        t = y / S
        px[x, y] = (int(30 + 20 * t), int(48 + 26 * t), int(38 + 10 * t))

# Middle: checkerboard tabletop in perspective-ish band (transparent elsewhere).
mid = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(mid)
n = 8
for r in range(n):
    for c in range(n):
        if (r + c) % 2:
            continue
        y0 = 560 + r * 50
        y1 = y0 + 50
        x0 = 100 + c * 105
        d.rectangle([x0, y0, x0 + 105, y1], fill=(120, 150, 90, 235))
d.rectangle([100, 560, 940, 960], outline=(20, 24, 28, 255), width=14)

# Front: a red pyramid and a blue cube standing on the board.
front = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(front)
d.polygon([(330, 640), (150, 800), (510, 800)], fill=(232, 56, 46, 255))
d.polygon([(330, 640), (510, 800), (560, 700)], fill=(180, 36, 30, 255))
d.rectangle([600, 620, 780, 800], fill=(51, 102, 242, 255))
d.polygon([(600, 620), (780, 620), (840, 570), (660, 570)], fill=(90, 140, 255, 255))
d.polygon([(780, 620), (840, 570), (840, 750), (780, 800)], fill=(30, 66, 170, 255))
d.ellipse([440, 180, 590, 330], fill=(250, 210, 60, 255))

layer("Back", back.convert("RGBA"))
layer("Middle", mid)
layer("Front", front)
write_json(os.path.join(stack, "Contents.json"), {
    "info": {"author": "xcode", "version": 1},
    "layers": [
        {"filename": "Front.solidimagestacklayer"},
        {"filename": "Middle.solidimagestacklayer"},
        {"filename": "Back.solidimagestacklayer"},
    ],
})
write_json(os.path.join(root, "Contents.json"), {"info": {"author": "xcode", "version": 1}})
print("wrote", stack)
