#!/usr/bin/env python3
"""Composes a preview of the MPC screen from the built skin's pictures and the engine's data (tests/dump.cpp).
    MV=../mpc-vst-plugins python3 tools/preview.py out.png        (run tools/make_skin.py and gen_vst.py first)
MPC draws the live texts itself; here they are approximated with DejaVu Sans."""
import os, re, subprocess, sys
from PIL import Image, ImageDraw, ImageFont
H = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
VST = os.path.join(H, "vst")
MV = os.environ.get("MV", H + "/../mpc-vst-plugins")
subprocess.run("g++ -std=c++17 -O2 -I%s/wrapper %s/tests/dump.cpp %s/src/engine.cpp -o /tmp/kd_dump -lm" % (MV, H, H), shell=True, check=True)
data = dict(l.rstrip("\n").split(" ", 1) for l in subprocess.run(["/tmp/kd_dump"], capture_output=True, text=True).stdout.splitlines() if " " in l)
SK = os.path.join(VST, "build/skin/DAVIS - VST - Spectral Sidechain/Plugin Skins")
bg = Image.open(os.path.join(VST, "build/skin/DAVIS - VST - Spectral Sidechain/Plugin Skins/sh_bg_0.png")).convert("RGBA")
Y = 86
cache = {}
def strip(n):
    if n not in cache: cache[n] = Image.open(os.path.join(VST, "images", n)).convert("RGBA")
    return cache[n]
def frame(img, k, h): return img.crop((0, k * h, img.width, (k + 1) * h))
fp = os.path.join(MV, "tools/html_art/fonts/TitilliumWeb-Bold.ttf")   # MPC draws live values in Titillium Web
fnt = lambda s: ImageFont.truetype(fp, s) if os.path.exists(fp) else ImageFont.load_default()
rng = {"freq": (0, 1000), "q": (0, 100), "ratio": (1, 21), "thresh": (-60, 0), "attack": (0, 100), "release": (0, 100), "amount": (-600, 300)}
text = {"gr": "gr_display", "link": None, "peak": "peak_display", "freq": "freq_display"}
d = ImageDraw.Draw(bg)
for line in open(os.path.join(VST, "layout.conf")).read().splitlines():
    g = lambda k: (re.search(r"\b%s=(\S+)" % k, line) or [None, None])[1]
    kind = line.split(" ", 1)[0]
    MODE = os.environ.get("KD_MODE", "RECEIVE")
    w_ = re.search(r"when=mode:(\w+)", line)
    if w_ and w_[1] != MODE: continue
    if kind == "art" and w_:
        f = re.search(r'file="([^"]+)"', line)[1]
        bg.alpha_composite(Image.open(os.path.join(VST, f)).convert("RGBA"), (0, 0)); continue
    if kind == "toggle":
        cx, cy = int(g("cx")), int(g("cy")) - Y
        nm = "sc" if g("key") == "scview" else "out"
        on = os.environ.get("KD_SCON" if nm == "sc" else "KD_OUTON")
        im = strip("%s_%s.png" % (nm, "on" if on else "off"))
        bg.alpha_composite(im, (cx - im.width // 2, cy - im.height // 2)); continue
    if kind not in ("meter", "slider_h", "knob", "readout", "button", "enum_h"): continue
    cx, cy, key = int(g("cx")), int(g("cy")) - Y, g("key")
    if kind == "meter":
        w, h, fr = int(g("w")), int(g("h")), int(g("frames"))
        k = max(0, min(fr - 1, int(float(data.get(key, 0)))))
        bg.alpha_composite(frame(strip(g("strip").split("/")[-1]), k, h), (cx - w // 2, cy - h // 2))
    elif kind == "slider_h":
        w, h = int(g("w")), int(g("h"))
        fr = int(g("frames")); bg.alpha_composite(frame(strip("slider.png"), round(float(data[key]) / 1000 * (fr - 1)), h), (cx - w // 2, cy - h // 2))
    elif kind == "knob":
        r = int(g("r")); lo, hi = rng[key]; s = 2 * r + 10
        bg.alpha_composite(frame(strip("knob_r%d.png" % r), round((float(data[key]) - lo) / (hi - lo) * 127), s), (cx - s // 2, cy - s // 2))
        d.text((cx, cy + r + 15), data[key + "_display"], fill="#26292e", font=fnt(17), anchor="mm")
    elif kind == "readout":
        t = "SC OK" if key == "link" else data.get(text.get(key, ""), "")
        d.text((cx, cy), t, fill="#" + (g("tcolor") or "26292e"), font=fnt(int(g("tsize")) - 4), anchor="mm")
    elif kind == "button":
        pass   # TO FREQ is drawn in the background; the button is a clear touch area
    elif kind == "enum_h":
        opts = re.search(r'options="([^"]*)"', line)[1].split(","); sw, sh = int(g("sw")), int(g("sh"))
        x0 = cx - sw * len(opts) // 2
        for i, o in enumerate(opts):
            im = Image.open(os.path.join(SK, "sh_seg_%s_%d_%s.png" % (key, i, "on" if i == (1 if key == "mode" and MODE == "SEND" else 0) else "off"))).convert("RGBA")
            bg.alpha_composite(im, (x0 + i * sw + sw // 2 - im.width // 2, cy - im.height // 2))
bg.convert("RGB").save(sys.argv[1] if len(sys.argv) > 1 else "/tmp/kd_preview.png")
print("preview written")
