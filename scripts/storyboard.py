#!/usr/bin/env python3
"""Storyboard for the highlight reel (docs/video-plan.md), from the
simulator's reel stills (scenarios/reel.sim -> build/sim-reel-*.png).

Writes docs/video/storyboard.png (every beat, in order, with its caption)
and docs/video/frame-guide.png (one 9:16 frame: where the meter's screen,
the caption and the app's buttons sit).

    make sim && python3 scripts/storyboard.py
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
OUT = os.path.join(ROOT, "docs", "video")

# (beat, still in build/ or None, caption): keep in step with docs/video-plan.md.
BEATS = [
    ("1 Hook", "sim-reel-01-ohms", "Stock: 9 s to settle.\nMine: watch."),
    ("2 Continuity", "sim-reel-02-continuity", "Instant continuity"),
    ("3 Beep styles", "sim-reel-03-beep-style", "Pick your beep"),
    ("4 Diode", "sim-reel-04-diode-led", "One beep per junction"),
    ("5 Auto-hold", "sim-reel-05-autohold", "Probe. Beep. Lift. Read."),
    ("6 Mini trend", "sim-reel-17-mini-trend", "History under the digits"),
    ("7 Graph", "sim-reel-07-graph", "Live trend graph"),
    ("8 Power", "sim-reel-19-power", "Watts, Wh and mAh\nwhile it charges"),
    ("9 Dual", "sim-reel-08-dual", "AC volts + Hz"),
    ("10 Recording", "sim-reel-10-recording", "10,668 points\nevery 15 ms"),
    ("11 Themes", "sim-reel-15s-theme-synthwave", "Five themes"),
    ("12 Install", None, "Installs over USB.\nNo screwdriver."),
    ("13 End card", "sim-reel-20-power-synthwave", "OpenOWON\nfor the XDM1241"),
]


def font(size, bold=False):
    for path in ("/System/Library/Fonts/Helvetica.ttc",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold
                 else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(path, size, index=1 if bold and path.endswith(".ttc") else 0)
        except OSError:
            continue
    return ImageFont.load_default()


def still(name):
    path = os.path.join(BUILD, name + ".png")
    if not os.path.exists(path):
        sys.exit(f"{path} missing: run `make sim` first")
    return Image.open(path).convert("RGB")


def placeholder(w, h, text):
    im = Image.new("RGB", (w, h), (24, 26, 32))
    d = ImageDraw.Draw(im)
    d.multiline_text((w // 2, h // 2), text, font=font(22, True), fill=(200, 205, 215),
                     anchor="mm", align="center")
    return im


def storyboard():
    cols, tw, th, cap = 4, 360, 240, 70
    rows = (len(BEATS) + cols - 1) // cols
    pad = 16
    W = cols * (tw + pad) + pad
    H = rows * (th + cap + pad) + pad + 50
    sheet = Image.new("RGB", (W, H), (14, 15, 19))
    d = ImageDraw.Draw(sheet)
    d.text((pad, 14), "OpenOWON highlight reel: storyboard (simulator stills; film the real meter)",
           font=font(22, True), fill=(235, 238, 245))
    for i, (beat, name, caption) in enumerate(BEATS):
        x = pad + (i % cols) * (tw + pad)
        y = 50 + pad + (i // cols) * (th + cap + pad)
        im = still(name).resize((tw, th), Image.LANCZOS) if name else \
            placeholder(tw, th, "laptop: serial installer\nmeter reboots")
        sheet.paste(im, (x, y))
        d.text((x, y + th + 6), beat, font=font(18, True), fill=(120, 200, 255))
        d.multiline_text((x, y + th + 28), caption.replace("\n", " "), font=font(17),
                         fill=(225, 228, 235))
    return sheet


def frame_guide():
    """A 1080x1920 frame, drawn at half size: the screen in the upper middle,
    the caption above it, TikTok's buttons on the right and bottom."""
    W, H = 540, 960
    im = Image.new("RGB", (W, H), (30, 30, 34))
    d = ImageDraw.Draw(im)
    sw, sh = 396, 264                       # the screen, kept left of the buttons
    scr = still("sim-reel-20-power-synthwave").resize((sw, sh), Image.LANCZOS)
    sx, sy = 28, 320
    d.rectangle((sx - 12, sy - 12, sx + sw + 12, sy + sh + 12), fill=(52, 54, 60))   # the bezel
    im.paste(scr, (sx, sy))
    d.multiline_text((sx + sw // 2, 220), "Watts, Wh and mAh\nwhile it charges", font=font(34, True),
                     fill=(255, 255, 255), anchor="mm", align="center",
                     stroke_width=3, stroke_fill=(0, 0, 0))
    over = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    o = ImageDraw.Draw(over)
    o.rectangle((0, int(H * 0.80), W, H), fill=(255, 60, 60, 70))             # captions, music
    o.rectangle((int(W * 0.85), int(H * 0.35), W, int(H * 0.80)), fill=(255, 60, 60, 70))  # buttons
    o.rectangle((0, 0, W, int(H * 0.08)), fill=(255, 160, 40, 60))            # top bar
    im = Image.alpha_composite(im.convert("RGBA"), over).convert("RGB")
    d = ImageDraw.Draw(im)
    d.text((10, int(H * 0.80) + 8), "keep clear: caption, sound, account", font=font(16), fill=(255, 220, 220))
    d.text((int(W * 0.85) + 4, int(H * 0.35) + 6), "app\nbuttons", font=font(14), fill=(255, 220, 220))
    d.text((10, 10), "keep clear: top bar", font=font(16), fill=(255, 230, 200))
    d.text((10, H - 120), "9:16, 1080x1920 (shown at half size)", font=font(16), fill=(200, 200, 205))
    return im


def main():
    os.makedirs(OUT, exist_ok=True)
    storyboard().save(os.path.join(OUT, "storyboard.png"), optimize=True)
    frame_guide().save(os.path.join(OUT, "frame-guide.png"), optimize=True)
    print("docs/video/storyboard.png, docs/video/frame-guide.png")


if __name__ == "__main__":
    main()
