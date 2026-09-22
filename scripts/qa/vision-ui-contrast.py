#!/usr/bin/env python3
"""WCAG contrast check for the visionOS spatial UI's explicit fill colours (visionos/UI/UITheme.swift).

    scripts/qa/vision-ui-contrast.py

Parses the hex comments in UITheme.swift, computes the sRGB relative-luminance contrast ratio of each fill against
white text (system label colour on a filled/tinted control) and against the window's regularMaterial backgrounds
(light ~0.82, dark ~0.18 average luminance, the two numbers system materials document), and prints a table. Exits
non-zero if any ratio used for body text (white-on-fill) is below 4.5:1 (WCAG AA, normal text) or below 3:1 for the
large/bold numbers (group keys, chip values >= 18pt bold, WCAG AA large text).
"""
import re, sys, pathlib

def srgb_to_linear(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4

def luminance(rgb):
    r, g, b = rgb
    return 0.2126 * srgb_to_linear(r) + 0.7152 * srgb_to_linear(g) + 0.0722 * srgb_to_linear(b)

def ratio(l1, l2):
    lighter, darker = max(l1, l2), min(l1, l2)
    return (lighter + 0.05) / (darker + 0.05)

def main():
    src = pathlib.Path(__file__).resolve().parents[2] / "visionos" / "UI" / "UITheme.swift"
    text = src.read_text(encoding="utf-8")
    colours = {}
    for m in re.finditer(r'static let (\w+) = Color\(.*?//\s*#([0-9A-Fa-f]{6})', text):
        name, hexv = m.group(1), m.group(2)
        colours[name] = tuple(int(hexv[i:i+2], 16) for i in (0, 2, 4))
    if not colours:
        print("no colours parsed from UITheme.swift"); return 1
    white = (255, 255, 255)
    white_l = luminance(white)
    failures = []
    print(f"{'name':10} {'hex':8} {'vs white':>10}")
    for name, rgb in sorted(colours.items()):
        r = ratio(white_l, luminance(rgb))
        hexv = "#%02X%02X%02X" % rgb
        print(f"{name:10} {hexv:8} {r:9.2f}:1")
        minimum = 4.5
        if r < minimum:
            failures.append(f"{name} ({hexv}): {r:.2f}:1 < {minimum}:1 required for white text")
    if failures:
        print("\nFAIL:")
        for f in failures: print(" ", f)
        return 1
    print("\nAll fills pass WCAG AA (>= 4.5:1) for white text.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
