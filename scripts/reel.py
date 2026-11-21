#!/usr/bin/env python3
"""Render the highlight reel (docs/video-plan.md) from its edit list
(docs/video/edit.toml) and the raw footage: cut, speed, frame to 9:16,
burn in the captions, join, and lay music under it.

    python3 scripts/reel.py                     # -> build/reel/reel.mp4
    python3 scripts/reel.py --footage ~/reel --music track.m4a
    python3 scripts/reel.py beeps footage/b02-continuity.mov

A beat whose clip isn't in the footage folder yet is stood in for by its
simulator still (run `make sim` first), so the whole reel renders before
anything is filmed. Needs ffmpeg and ffprobe; captions are drawn with
Pillow in the storyboard's style (scripts/storyboard.py).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import tomllib

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import storyboard  # noqa: E402  (fonts, stills, the title-card placeholder)

ROOT = storyboard.ROOT
W, H, FPS = 1080, 1920, 30
# Where things sit, from the frame guide (scripts/storyboard.py, half size x2):
# the meter's screen in the upper middle, left of the app's buttons.
SCREEN = (56, 640, 792, 528)                     # x, y, w, h for a stand-in still
CAPTION_X, CAPTION_Y = 56 + 792 // 2, 440        # caption centre
CAPTION_SIZE, CAPTION_STROKE = 68, 6
ENC = ["-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p",
       "-r", str(FPS), "-c:a", "aac", "-b:a", "192k", "-ar", "48000", "-ac", "2"]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"failed: {' '.join(cmd)}\n{r.stderr[-2000:]}")
    return r.stdout


def has_audio(path):
    return "audio" in run(["ffprobe", "-v", "error", "-show_entries", "stream=codec_type",
                           "-of", "csv=p=0", path])


def duration(path):
    return float(run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                      "-of", "csv=p=0", path]).strip())


def caption_png(text, path):
    """The caption on a transparent 1080x1920 layer: white, bold, black outline."""
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(im).multiline_text(
        (CAPTION_X, CAPTION_Y), text, font=storyboard.font(CAPTION_SIZE, True),
        fill=(255, 255, 255, 255), anchor="mm", align="center",
        stroke_width=CAPTION_STROKE, stroke_fill=(0, 0, 0, 255))
    im.save(path)


def stand_in_png(still, path):
    """A frame for a missing clip: the simulator still where the meter's
    screen will be, in a bezel, on a dark background."""
    im = Image.new("RGB", (W, H), (30, 30, 34))
    d = ImageDraw.Draw(im)
    x, y, w, h = SCREEN
    d.rectangle((x - 24, y - 24, x + w + 24, y + h + 24), fill=(52, 54, 60))
    scr = storyboard.placeholder(w, h, "laptop: serial installer\nmeter reboots") \
        if still in (None, "none") else storyboard.still(still).resize((w, h), Image.LANCZOS)
    im.paste(scr, (x, y))
    d.text((W // 2, y + h + 90), "stand-in: simulator still", font=storyboard.font(30),
           fill=(150, 150, 160), anchor="mm")
    im.save(path)


def render_part(part, beat, n_parts, cap, footage, tmp, out):
    """One segment: a clip (cut, speed, framed) or a stand-in, with the caption."""
    secs = beat.get("seconds", 4) / n_parts
    clip = os.path.join(footage, part["clip"]) if part.get("clip") else None
    if clip and os.path.exists(clip):
        t0 = float(part.get("in", 0.0))
        slow = float(part.get("slow", 1))
        t1 = float(part.get("out", t0 + secs / slow))
        dur = (t1 - t0) * slow
        zoom = float(part.get("zoom", 1.0))
        fx, fy = part.get("focus", [0.5, 0.5])
        audio = part.get("audio", slow == 1) and has_audio(clip)
        v = (f"[0:v]setpts=(PTS-STARTPTS)*{slow},"
             f"scale={int(W * zoom)}:{int(H * zoom)}:force_original_aspect_ratio=increase,"
             f"crop={W}:{H}:(iw-{W})*{fx}:(ih-{H})*{fy},fps={FPS},format=yuv420p[b];"
             f"[b][1:v]overlay=0:0[v]")
        a = "[0:a]asetpts=PTS-STARTPTS,aresample=48000[a]" if audio else \
            f"anullsrc=r=48000:cl=stereo,atrim=0:{dur}[a]"
        run(["ffmpeg", "-y", "-v", "error", "-ss", str(t0), "-t", str(t1 - t0), "-i", clip,
             "-i", cap, "-filter_complex", f"{v};{a}", "-map", "[v]", "-map", "[a]",
             "-t", f"{dur:.3f}", *ENC, out])
        return dur, part["clip"]
    dur = float(part["out"]) - float(part["in"]) if "out" in part else secs
    png = os.path.join(tmp, os.path.basename(out) + ".png")
    stand_in_png(part.get("still", beat.get("still")), png)
    run(["ffmpeg", "-y", "-v", "error", "-loop", "1", "-t", f"{dur:.3f}", "-i", png,
         "-i", cap, "-f", "lavfi", "-t", f"{dur:.3f}", "-i", "anullsrc=r=48000:cl=stereo",
         "-filter_complex", f"[0:v][1:v]overlay=0:0,fps={FPS},format=yuv420p[v]",
         "-map", "[v]", "-map", "2:a", "-t", f"{dur:.3f}", *ENC, out])
    return dur, None


def render(args):
    for tool in ("ffmpeg", "ffprobe"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found: brew install ffmpeg")
    with open(args.edit, "rb") as f:
        beats = tomllib.load(f)["beat"]
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="reel-")
    segs, quiet, t = [], [], 0.0
    for i, beat in enumerate(beats, 1):
        cap = os.path.join(tmp, f"cap{i:02d}.png")
        caption_png(beat["caption"], cap)
        parts = beat.get("part") or [{}]
        start, used = t, []
        for k, part in enumerate(parts):
            seg = os.path.join(tmp, f"seg{i:02d}{k:02d}.mp4")
            dur, clip = render_part(part, beat, len(parts), cap, args.footage, tmp, seg)
            segs.append(seg)
            t += dur
            if clip:
                used.append(clip)
        if not beat.get("music", True):
            quiet.append((start, t))
        src = ", ".join(sorted(set(used))) if used else "stand-in"
        print(f"{i:2d}  {start:5.1f}-{t:5.1f} s  {beat['caption'].splitlines()[0][:34]:34s}  {src}")
    lst = os.path.join(tmp, "list.txt")
    with open(lst, "w") as f:
        f.writelines(f"file '{s}'\n" for s in segs)
    joined = os.path.join(tmp, "joined.mp4")
    run(["ffmpeg", "-y", "-v", "error", "-f", "concat", "-safe", "0", "-i", lst, "-c", "copy", joined])
    if args.music:
        # Music under the clips' own sound, silent where a beat says so,
        # faded out over the last 1.5 s.
        off = "".join(f",volume=enable='between(t,{a:.2f},{b:.2f})':volume=0" for a, b in quiet)
        fade = max(t - 1.5, 0)
        run(["ffmpeg", "-y", "-v", "error", "-i", joined, "-i", args.music, "-filter_complex",
             f"[1:a]atrim=0:{t:.3f},asetpts=PTS-STARTPTS,volume={args.music_level}{off},"
             f"afade=t=out:st={fade:.2f}:d=1.5[m];[0:a][m]amix=inputs=2:normalize=0[a]",
             "-map", "0:v", "-map", "[a]", "-c:v", "copy", "-c:a", "aac", "-b:a", "192k",
             "-t", f"{t:.3f}", args.out])
    else:
        shutil.copy(joined, args.out)
    shutil.rmtree(tmp)
    print(f"{os.path.relpath(args.out, ROOT)}: {duration(args.out):.1f} s, {W}x{H} {FPS} fps")


def beeps(args):
    """Beep onsets in a clip's sound: energy near the beeper's pitch in 10 ms
    frames, against the clip's own median. Prints the times and an in/out."""
    import numpy as np
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", args.clip, "-ac", "1", "-ar", "48000",
                          "-f", "s16le", "-"], capture_output=True).stdout
    x = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
    if not len(x):
        sys.exit("no audio in that clip")
    n = 480                                               # 10 ms
    frames = x[: len(x) // n * n].reshape(-1, n) * np.hanning(n)
    spec = np.abs(np.fft.rfft(frames, axis=1))
    f = np.fft.rfftfreq(n, 1 / 48000)
    lo, hi = args.pitch * 0.8, args.pitch * 1.25
    band = spec[:, (f >= lo) & (f <= hi)].sum(axis=1)
    thr = np.median(band) * args.threshold + 1e-9
    on, last = [], -1.0
    for k, e in enumerate(band):
        tt = k * n / 48000
        if e > thr and (k == 0 or band[k - 1] <= thr) and tt - last > 0.08:
            on.append(tt)
            last = tt
    if not on:
        print(f"no beeps found near {args.pitch:.0f} Hz (try --pitch or --threshold)")
        return
    print("beeps at", " ".join(f"{v:.2f}" for v in on), "s")
    print(f"suggested in = {max(on[0] - 0.5, 0):.2f}, out = {on[-1] + 0.8:.2f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd")
    b = sub.add_parser("beeps", help="list a clip's beep onsets")
    b.add_argument("clip")
    b.add_argument("--pitch", type=float, default=3000.0, help="beeper pitch, Hz (default 3000)")
    b.add_argument("--threshold", type=float, default=8.0, help="x the median band energy")
    ap.add_argument("--edit", default=os.path.join(ROOT, "docs", "video", "edit.toml"))
    ap.add_argument("--footage", default=os.path.join(ROOT, "footage"))
    ap.add_argument("--music", help="a music track to lay under the reel")
    ap.add_argument("--music-level", type=float, default=0.35)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "reel", "reel.mp4"))
    args = ap.parse_args()
    beeps(args) if args.cmd == "beeps" else render(args)


if __name__ == "__main__":
    main()
