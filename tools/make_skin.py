#!/usr/bin/env python3
"""Writes DAVIS Spectral Sidechain's parameter list, presets, vst.json, background art, filmstrip images and layout.conf into vst/.

    python3 tools/make_skin.py        # needs rsvg-convert (librsvg2-bin) for the PNG filmstrips

Skin coordinates are 1280 x 628; layout.conf's y is the skin's y + 86 (Y below).
Engine keys (src/engine.cpp): c0..c111 the band's curve at full Amount, h0, h2 .. h110 the live ring (the gain applied now),
peak / peak_display the SC's strongest frequency, snap puts Freq there. A touch on a c<n> column moves the band.
The top band (LOGO_BOX) is left free for the user's logo: put it at vst/artwork/logo.png (or .svg) and rerun.
"""
import json
import math
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
VST = os.path.join(HERE, "..", "vst")
IMG = os.path.join(VST, "images")
Y = 86

NCOL, COL_W = 112, 6
RING_EVERY = 2
PLOT_X, PLOT_Y, PLOT_H = 92, 134, 200
CURVE_STEPS, RING_STEPS, LIVE_STEPS, SPEC_STEPS = 60, 30, 30, 30
OUT_BTN = (686, 122, 104, 42)                            # OUT, the graph screen's top right corner (skin x, y, w, h)
SC_BTN = (OUT_BTN[0] - 104 - 12, OUT_BTN[1], 104, 42)    # SC view, just left of OUT: shows the kick's spectrum behind the curves
F_LO, F_HI = 15.0, 20000.0
FRAMES = 128
KNOB_R = 27
KNOB_DY = -11                                            # knob centre below the cell centre
LIVE_W = COL_W * RING_EVERY
KICK_EVERY = int(os.environ.get("KD_BAR_EVERY", "2"))   # SC view and OUT: one bar per N columns (the engine's KICK_EVERY / SPEC_EVERY)
KICK_W = COL_W * KICK_EVERY
RING_W, RING_H = 28, PLOT_H + 28                     # the ring meters sit on every 2nd column (a 12 px grid)
SC, IN, ORANGE, RED = "#4fb89d", "#8a7bd0", "#f0a030", "#b8432f"
PAGE, CELL, DARK, INK_D = "#c9c8bf", "#d6d5cd", "#14171c", "#26292e"


def meter(key, strip, cx, cy, w, h, frames, Ysk):
    return 'meter cx=%d cy=%d w=%d h=%d key=%s strip=images/%s frames=%d' % (cx, cy, w, h, key, strip, frames)


# ---------------------------------------------------------------- parameters
def params():
    P = [
        {"key": "mode", "name": "MODE", "options": ["RECEIVE", "SEND"], "default": "RECEIVE", "qlink_ticks": 1},
        {"key": "chan", "name": "Chan", "options": ["A", "B", "C", "D"], "default": "A", "qlink_ticks": 3},
        {"key": "freq", "name": "FREQ", "min": 0, "max": 1000, "default": 193, "dynamic_display": True},
        {"key": "q", "name": "Q", "min": 0, "max": 100, "default": 55, "dynamic_display": True},
        {"key": "ratio", "name": "RATIO", "min": 1, "max": 21, "default": 4, "dynamic_display": True},
        {"key": "thresh", "name": "THRESH", "min": -60, "max": 0, "default": -20, "dynamic_display": True},
        {"key": "attack", "name": "ATK", "min": 0, "max": 100, "default": 18, "dynamic_display": True},
        {"key": "release", "name": "REL", "min": 0, "max": 100, "default": 81, "dynamic_display": True},
        {"key": "amount", "name": "AMOUNT", "min": -600, "max": 300, "default": -90, "dynamic_display": True},
        {"key": "gr", "name": "GR", "min": -300, "max": 300, "default": 0, "dynamic_display": True},
        {"key": "link", "name": "SC", "options": ["NO SC", "SC OK"], "default": "NO SC"},
        {"key": "noop", "name": "-", "momentary": True},
    ]
    TAIL = [{"key": "peak", "name": "SC Peak", "min": 0, "max": 1000, "default": 0, "dynamic_display": True},
            {"key": "snap", "name": "To Freq", "momentary": True}]
    P += [{"key": "c%d" % i, "name": "Curve %d" % (i + 1), "min": 0, "max": CURVE_STEPS, "default": CURVE_STEPS // 2} for i in range(NCOL)]
    P += [{"key": "h%d" % i, "name": "Ring %d" % (i + 1), "min": 0, "max": RING_STEPS + 1, "default": 0} for i in range(0, NCOL, RING_EVERY)]
    LIVE = [{"key": "l%d" % i, "name": "Live %d" % (i + 1), "min": 0, "max": LIVE_STEPS, "default": LIVE_STEPS // 2} for i in range(0, NCOL, RING_EVERY)]
    OUT = [{"key": "out", "name": "Out", "options": ["OFF", "ON"], "default": "OFF"}]
    OUT += [{"key": "s%d" % i, "name": "Spec %d" % (i + 1), "min": 0, "max": SPEC_STEPS, "default": 0} for i in range(0, NCOL, KICK_EVERY)]
    OUT += [{"key": "scview", "name": "SC view", "options": ["OFF", "ON"], "default": "OFF"}]
    OUT += [{"key": "k%d" % i, "name": "Kick %d" % (i + 1), "min": 0, "max": SPEC_STEPS, "default": 0} for i in range(0, NCOL, KICK_EVERY)]
    return P + TAIL + LIVE + OUT       # new params only at the end: saved projects bind by index


def hz_pos(hz):
    return round(1000 * math.log(hz / F_LO) / math.log(F_HI / F_LO))


def q_pos(q):
    return round(100 * math.log(q / 0.1) / math.log(8 / 0.1))


def ms_pos(ms):
    return round(100 * math.log(ms) / math.log(500))


def presets():
    def p(name, hz, q, amt, thr, ratio, atk, rel):
        return {"name": name, "values": {"freq": hz_pos(hz), "q": q_pos(q), "amount": round(amt * 10), "thresh": thr, "ratio": ratio,
                                          "attack": ms_pos(atk), "release": ms_pos(rel)}}
    return [p("Sub Duck", 55, 1.2, -9, -20, 4, 3, 150), p("Tight Low End", 80, 0.8, -6, -18, 3, 2, 110),
            p("Wide Pump", 90, 0.4, -4.5, -24, 2, 5, 260), p("Notch Out", 60, 4, -60, -20, 21, 1, 90),
            p("De-Ess By SC", 6500, 2.5, -8, -26, 6, 1, 60), p("Mid Lift", 120, 1.5, 3, -20, 4, 4, 180)]


VST_JSON = {
    "name": "Spectral Sidechain", "vendor": "DAVIS", "uid": "DSpS", "version": 1050, "so": "spectral_sidechain.so", "params": "params.json",
    "effect": True,
    "build": {"root": "..", "sources": ["src/engine.cpp"], "cflags": ["-O2"], "libs": ["-lm"]},
    "layout": "layout.conf", "art": "html", "presets": "presets.json",
    "defines": {"HAS_DISPLAY_REV": 1, "DISPLAY_REV_NO_UPDATE": 1, "DISPLAY_REV_THREAD": 1}, "targets": ["armv7"],
}


# ---------------------------------------------------------------- images
def render(items):
    for path, svg, w, h in items:
        tmp = path + ".svg"
        open(tmp, "w").write(svg)
        subprocess.run(["rsvg-convert", "-w", str(w), "-h", str(h), "-o", path, tmp], check=True)
        os.remove(tmp)


CURVE = os.environ.get("KD_CURVE", "#ffd60a")                # the band curve's colour (yellow); KD_CURVE=#rrggbb to try another


def curve_strip(w, h):
    """the band's curve, one 6 px column: CURVE_STEPS + 1 frames (0 = -30 dB at the bottom .. top = +30 dB, 1 dB a step):
    a thin red line at the level and a faint fill to 0 dB (a stepped line: light on the MPC)"""
    n = CURVE_STEPS + 1
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">' % (w, h * n)]
    for k in range(n):
        top = k * h
        y = top + 2.5 + (h - 5) * (1 - k / (n - 1))
        mid = top + h / 2
        if CURVE == "none":
            continue                                             # KD_CURVE=none: no line, for the previews' masks
        if abs(y - mid) > 1:
            a, b = sorted((mid, y))
            o.append('<rect x="0" y="%.2f" width="%d" height="%.2f" fill="%s" opacity=".10"/>' % (a, w, b - a, CURVE))
        o.append('<rect x="-.6" y="%.2f" width="%g" height="2" rx="1" fill="%s"/>' % (y - 1, w + 1.2, CURVE))
    o.append("</svg>")
    return "".join(o), w, h * n


def live_strip():
    """the live curve: what the plugin is doing right now, in semi-transparent red: the bell's shape (the Q) scaled to the
    gain applied. LIVE_STEPS + 1 frames, frame k = (k - LIVE_STEPS/2) * 2 dB, the middle one = flat (nothing drawn)"""
    w, h, n = LIVE_W, PLOT_H, LIVE_STEPS + 1
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">' % (w, h * n)]
    for k in range(n):
        top = k * h
        y = top + 2.5 + (h - 5) * (1 - k / (n - 1))
        mid = top + h / 2
        if k != LIVE_STEPS // 2:
            a, b = sorted((mid, y))
            o.append('<rect x="0" y="%.2f" width="%d" height="%.2f" fill="%s" opacity=".70"/>' % (a, w, b - a, RED))
            o.append('<rect x="0" y="%.2f" width="%d" height="1.6" fill="%s" opacity=".95"/>' % (y - .8, w, RED))
    o.append("</svg>")
    return "".join(o), w, h * n


def spec_strip():
    """the output's spectrum (OUT), one 12 px column: SPEC_STEPS + 1 frames, 0 = nothing; frame k = -66 + 2k dBFS over the
    graph's height (its own scale, not the +-30 dB of the curves): a soft green fill from the bottom and a light line"""
    w, h, n = KICK_W, PLOT_H, SPEC_STEPS + 1
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">' % (w, h * n)]
    for k in range(1, n):
        top = k * h
        y = top + 2.5 + (h - 5) * (1 - k / (n - 1))
        o.append('<rect x="1" y="%.2f" width="%d" height="%.2f" fill="#97C459" opacity=".45"/>' % (y, w - 2, top + h - y))
        o.append('<rect x="1" y="%.2f" width="%d" height="2" fill="#C0DD97" opacity=".90"/>' % (y - 1, w - 2))
    o.append("</svg>")
    return "".join(o), w, h * n


def kick_strip():
    """the SC's spectrum (SC view), one 12 px column, the same frames as OUT: a grey fill from the bottom, drawn behind
    everything, so the kick's body reads as the ground the band works on. Frame 0 = nothing (silence)"""
    w, h, n = KICK_W, PLOT_H, SPEC_STEPS + 1
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">' % (w, h * n)]
    for k in range(1, n):
        top = k * h
        y = top + 2.5 + (h - 5) * (1 - k / (n - 1))
        o.append('<rect x="1" y="%.2f" width="%d" height="%.2f" fill="#888780" opacity=".42"/>' % (y, w - 2, top + h - y))
        o.append('<rect x="1" y="%.2f" width="%d" height="2" fill="#B4B2A9" opacity=".60"/>' % (y - 1, w - 2))
    o.append("</svg>")
    return "".join(o), w, h * n


def out_btn(on):
    """OUT: a small pill, green and lit when on, dark with a grey label when off"""
    w, h = OUT_BTN[2], OUT_BTN[3]
    fill, edge, ink = ("#3B6D11", "#639922", "#EAF3DE") if on else ("#1d2a16", "#3B6D11", "#97C459")
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d"><rect x=".5" y=".5" width="%d" height="%d" rx="11" fill="%s" stroke="%s"/>'
            '<text x="%g" y="%g" text-anchor="middle" font-family="Jost" font-weight="600" font-size="21" fill="%s">OUT</text></svg>'
            % (w, h, w - 1, h - 1, fill, edge, w / 2, h / 2 + 7, ink)), w, h


def sc_btn(on):
    """SC view: a small pill like OUT, grey and lit when the kick's spectrum is on, dark with a grey label when off"""
    w, h = SC_BTN[2], SC_BTN[3]
    fill, edge, ink = ("#5F5E5A", "#888780", "#F1EFE8") if on else ("#2a2a28", "#5F5E5A", "#B4B2A9")
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d"><rect x=".5" y=".5" width="%d" height="%d" rx="11" fill="%s" stroke="%s"/>'
            '<text x="%g" y="%g" text-anchor="middle" font-family="Jost" font-weight="600" font-size="21" fill="%s">SC</text></svg>'
            % (w, h, w - 1, h - 1, fill, edge, w / 2, h / 2 + 7, ink)), w, h


def dim_png():
    """SEND mode: every control but RECEIVE/SEND and A-D is hidden (when=mode:RECEIVE) and its plate washed out to the
    page colour; a red frame goes round SEND and A-D, the five buttons that matter there"""
    W, H = 1280, 628
    bx, by = STATUS_BOX[0], STATUS_BOX[1]
    plates = [(GRAPH_BOX, .70), (SL_BOX, .82)]
    plates += [((cx - CELL_W // 2, cy - CELL_H // 2, CELL_W, CELL_H), .82) for cx in COLS_X for cy in ROWS_Y]   # each knob plate on its own: the gaps and shadows stay
    plates += [
              ((bx, by + 70, STATUS_BOX[2], STATUS_BOX[3] - 70), .82)]
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">' % (W, H)]
    for (x, y, w, h), a in plates:
        o.append('<rect x="%d" y="%d" width="%d" height="%d" rx="%d" fill="%s" fill-opacity="%g"/>' % (x, y, w, h, 14 if w == CELL_W else 16, PAGE, a))
    x0, x1 = bx + 120 - 6, bx + 320 + 2 * 42 + 6          # SEND (right half of the mode pair) .. D
    y0, y1 = by + 40 - 22 - 6, by + 40 + 22 + 6
    o.append('<rect x="%d" y="%d" width="%d" height="%d" rx="10" fill="none" stroke="%s" stroke-width="3"/>' % (x0, y0, x1 - x0, y1 - y0, RED))
    o.append("</svg>")
    return "".join(o), W, H


def knob_strip(r):
    """a soft dome in the panel's cream (no metal): a wide shadow down and right, a faint rim, a short red pointer near the edge.
    FRAMES positions over 270 degrees; the scale ticks around it are in the background"""
    s_ = 2 * r + 26                                        # the widget box: 2(r+8)+10, with room for the soft shadow
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d"><defs>' % (s_, s_ * FRAMES, s_, s_ * FRAMES),
         '<radialGradient id="dome" cx=".4" cy=".35" r=".75"><stop stop-color="#f4f3ee"/><stop offset=".7" stop-color="#e6e4dc"/><stop offset="1" stop-color="#cfcdc5"/></radialGradient>'
         '<linearGradient id="rim" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#fbfaf6"/><stop offset="1" stop-color="#a9a7a0"/></linearGradient>'
         '<filter id="ks" x="-40%%" y="-40%%" width="180%%" height="180%%"><feGaussianBlur stdDeviation="4"/></filter></defs>']
    pt = lambda cx, cy, a, rad: (cx + rad * math.sin(math.radians(a)), cy - rad * math.cos(math.radians(a)))
    for k in range(FRAMES):
        cx, cy = s_ / 2, s_ / 2 + k * s_
        a1 = -135 + 270 * k / (FRAMES - 1)
        o.append('<circle cx="%g" cy="%g" r="%g" fill="#5c5a54" opacity=".45" filter="url(#ks)"/>' % (cx + 3, cy + 5, r))
        o.append('<circle cx="%g" cy="%g" r="%g" fill="url(#rim)"/>' % (cx, cy, r))
        o.append('<circle cx="%g" cy="%g" r="%g" fill="url(#dome)"/>' % (cx, cy, r - 2.2))
        p0, p1 = pt(cx, cy, a1, r * .58), pt(cx, cy, a1, r * .84)
        o.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" stroke="%s" stroke-width="5" stroke-linecap="round"/>' % (p0[0], p0[1], p1[0], p1[1], RED))
    o.append("</svg>")
    return "".join(o), s_, s_ * FRAMES


def knob_ticks(cx, cy, r):
    """the scale around a knob: 31 marks over 270 degrees, every other one longer"""
    o = []
    for i in range(31):
        a = math.radians(-135 + 270 * i / 30)
        r0, r1 = r + 6, r + (13 if i % 2 == 0 else 10)
        o.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" stroke="#8c8a83" stroke-width="%s" stroke-linecap="round"/>' % (
            cx + r0 * math.sin(a), cy - r0 * math.cos(a), cx + r1 * math.sin(a), cy - r1 * math.cos(a), "2" if i % 2 == 0 else "1.5"))
    return "".join(o)


SL_W, SL_H = 700, 128
SL_FRAMES = 96                                           # 96 x 128 px = 12288, the tallest strip MPC draws right
HANDLE_W = 64


def slider_strip():
    """the Freq fader's thumb, big for a finger: a cream disc nearly the capsule's height, lit from the top left, its
    shadow down and to the right; frame k = k / (SL_FRAMES - 1) along"""
    d = SL_H - 22
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d"><defs>'
         '<linearGradient id="th" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#f6f5f1"/><stop offset="1" stop-color="#e2e0d8"/></linearGradient>'
         '<linearGradient id="tr" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#ffffff"/><stop offset=".5" stop-color="#dcdad2"/><stop offset="1" stop-color="#a5a39c"/></linearGradient>'
         '<filter id="ts" x="-40%%" y="-40%%" width="180%%" height="180%%"><feGaussianBlur stdDeviation="4"/></filter></defs>' % (SL_W, SL_H * SL_FRAMES)]
    for k in range(SL_FRAMES):
        cx = d / 2 + 12 + (SL_W - d - 24) * k / (SL_FRAMES - 1)
        cy = k * SL_H + SL_H / 2 - 4
        o.append('<circle cx="%.1f" cy="%.1f" r="%g" fill="#2e2c28" opacity=".45" filter="url(#ts)"/>' % (cx + 5, cy + 7, d / 2 - 1))
        o.append('<circle cx="%.1f" cy="%.1f" r="%g" fill="url(#tr)"/>' % (cx, cy, d / 2))
        o.append('<circle cx="%.1f" cy="%.1f" r="%g" fill="url(#th)"/>' % (cx, cy, d / 2 - 2.5))
    o.append("</svg>")
    return "".join(o), SL_W, SL_H * SL_FRAMES


def ring_strip():
    """the live ring: a thin vertical line through the graph and an orange ring at the gain applied now
    (frame 0 = none, frame k = level k-1 of RING_STEPS)"""
    w, h, n = RING_W, RING_H, RING_STEPS + 2
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">' % (w, h * n)]
    for k in range(1, n):
        cy = k * h + 14 + 2.5 + (PLOT_H - 5) * (1 - (k - 1) / RING_STEPS)
        o.append('<rect x="%g" y="%g" width="1.2" height="%d" fill="#cfd6de" opacity=".35"/>' % (w / 2 - .6, k * h + 14, PLOT_H))
        o.append('<circle cx="%g" cy="%.1f" r="5.2" fill="%s" stroke="#14171c" stroke-opacity=".55" stroke-width="1.4"/>' % (w / 2, cy, ORANGE))
    o.append("</svg>")
    return "".join(o), w, h * n


# ---------------------------------------------------------------- background (skin coordinates)
def plot_x(hz):
    return PLOT_X + NCOL * COL_W * math.log(hz / F_LO) / math.log(F_HI / F_LO)


def plot_y(db):
    return PLOT_Y + 2.5 + (PLOT_H - 5) * (30 - db) / 60


LOGO_BOX = (40, 10, 1200, 96)                            # x, y, w, h: free for the user's logo
GRAPH_BOX = (40, 116, 760, 256)                          # the dark graph panel
SL_BOX = (40, 382, 760, 236)                             # the Freq cell
SL_CX, SL_CY = SL_BOX[0] + SL_BOX[2] // 2, SL_BOX[1] + 136
TITLE_CY = SL_BOX[1] + 36
COLS_X = (885, 1030, 1175)                               # the knob cells' centres
ROWS_Y = (201, 381)
CELL_W, CELL_H = 130, 170
STATUS_BOX = (820, 476, 420, 142)                        # mode, channel, GR, SC
KNOBS = [("Q", "q", "0.1", "8", 0, 0), ("Ratio", "ratio", "1:1", "21:1", 1, 0), ("Thresh", "thresh", "-60 dB", "0 dB", 2, 0),
         ("Atk", "attack", "1 ms", "500 ms", 0, 1), ("Rel", "release", "1 ms", "500 ms", 1, 1), ("Amount", "amount", "-60 dB", "+30 dB", 2, 1)]


def txt(x, y, t, size=14, fill="#26292e", anchor="start", weight=400):
    # an inline style: the renderer's stylesheet sets every text's font and weight, and a style attribute is the one thing it does not override
    return '<text x="%g" y="%g" style="font-family:Jost,sans-serif;font-size:%gpx;font-weight:%d;fill:%s" text-anchor="%s">%s</text>' % (x, y, size, weight, fill, anchor, t)


def logo_file():
    for n in ("logo.svg", "logo.png"):
        if os.path.exists(os.path.join(VST, "artwork", n)):
            return n
    return None


DEFS = """<defs>
<filter id="brush" x="0" y="0" width="100%" height="100%">
  <feTurbulence type="fractalNoise" baseFrequency="0.9 0.012" numOctaves="2" seed="7" result="n"/>
  <feColorMatrix in="n" type="matrix" values="0 0 0 0 0.5  0 0 0 0 0.5  0 0 0 0 0.5  0 0 0 0.10 0"/>
</filter>
<linearGradient id="plate" x2="0" y2="1"><stop stop-color="#d5d3cb"/><stop offset=".5" stop-color="#c9c7bf"/><stop offset="1" stop-color="#bebcb4"/></linearGradient>
<linearGradient id="cell" x2="0" y2="1"><stop stop-color="#e3e1d9"/><stop offset="1" stop-color="#cfcdc5"/></linearGradient>
<linearGradient id="bezel" x2="0" y2="1"><stop stop-color="#3a3c40"/><stop offset=".5" stop-color="#1b1d20"/><stop offset="1" stop-color="#0a0b0c"/></linearGradient>
<linearGradient id="screen" x2="0" y2="1"><stop stop-color="#151a20"/><stop offset="1" stop-color="#0d1014"/></linearGradient>
<linearGradient id="glass" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#fff" stop-opacity=".07"/><stop offset=".45" stop-color="#fff" stop-opacity=".02"/><stop offset=".46" stop-color="#fff" stop-opacity="0"/></linearGradient>
<linearGradient id="groove" x2="0" y2="1"><stop stop-color="#1c1d1f"/><stop offset="1" stop-color="#4a4c50"/></linearGradient>
<linearGradient id="green" x2="0" y2="1"><stop stop-color="#cd5b45"/><stop offset="1" stop-color="#a5382a"/></linearGradient>
<linearGradient id="pill" x2="0" y2="1"><stop stop-color="#efeee8"/><stop offset=".6" stop-color="#e4e2da"/><stop offset="1" stop-color="#d3d1c9"/></linearGradient>
<filter id="soft" x="-5%" y="-20%" width="110%" height="140%"><feGaussianBlur stdDeviation="3"/></filter>
<filter id="tiny" x="-2%" y="-50%" width="104%" height="200%"><feGaussianBlur stdDeviation="1"/></filter>
<linearGradient id="cutv" x2="0" y2="1"><stop stop-color="#8d8b84"/><stop offset=".5" stop-color="#c3c1b9"/><stop offset="1" stop-color="#f6f5f0"/></linearGradient>
<linearGradient id="bed" x2="0" y2="1"><stop stop-color="#d6d4cc"/><stop offset="1" stop-color="#e2e0d8"/></linearGradient>
<linearGradient id="rimv" x2="0" y2="1"><stop stop-color="#faf9f5"/><stop offset=".55" stop-color="#d9d7cf"/><stop offset="1" stop-color="#a9a7a0"/></linearGradient>
<filter id="soft6" x="-5%" y="-30%" width="110%" height="160%"><feGaussianBlur stdDeviation="6"/></filter>
<radialGradient id="screwg" cx=".35" cy=".3" r=".8"><stop stop-color="#f2f1ec"/><stop offset="1" stop-color="#7d7c76"/></radialGradient>
<filter id="lift" x="-10%" y="-10%" width="120%" height="125%"><feDropShadow dx="0" dy="3" stdDeviation="3" flood-color="#000" flood-opacity=".28"/></filter>
<filter id="inset" x="-5%" y="-5%" width="110%" height="110%"><feDropShadow dx="0" dy="-1" stdDeviation="0.6" flood-color="#fff" flood-opacity=".6"/></filter>
</defs>"""


def cell(x, y, w, h, r=14):
    """a raised plate: soft shadow under, a light top edge, a darker bottom edge, brushed"""
    return ('<rect x="%d" y="%d" width="%d" height="%d" rx="%d" fill="url(#cell)" filter="url(#lift)"/>'
            '<rect x="%d" y="%d" width="%d" height="%d" rx="%d" fill="#000" filter="url(#brush)"/>'
            '<rect x="%.1f" y="%.1f" width="%d" height="%d" rx="%d" fill="none" stroke="#fff" stroke-opacity=".75" stroke-width="1.2"/>'
            '<rect x="%.1f" y="%.1f" width="%d" height="%d" rx="%d" fill="none" stroke="#8c8a83" stroke-opacity=".55" stroke-width="1"/>') % (
        x, y, w, h, r, x, y, w, h, r, x + .6, y + .6, w - 1.2, h - 1.2, r, x + .5, y + .5, w - 1, h - 1, r)


def screw(cx, cy):
    return ('<circle cx="%g" cy="%g" r="10" fill="#8f8d86"/><circle cx="%g" cy="%g" r="8.5" fill="url(#screwg)"/>'
            '<path d="M%g %gh10M%g %gv10" stroke="#55534e" stroke-width="2.2" stroke-linecap="round"/>') % (
        cx, cy + 1, cx, cy, cx - 5, cy, cx, cy - 5)


SNAP = (SL_BOX[0] + 500, TITLE_CY, 150, 50)                # cx, cy, w, h of TO FREQ (drawn in the background; a clear touch area on top)


def snap_art():
    cx, cy, w, h = SNAP
    x, y = cx - w / 2, cy - h / 2
    return ('<rect x="%g" y="%g" width="%d" height="%d" rx="8" fill="#6f2117" filter="url(#lift)"/>'
            '<rect x="%g" y="%g" width="%d" height="%d" rx="8" fill="url(#green)"/>'
            '<rect x="%g" y="%g" width="%d" height="%d" rx="7.5" fill="none" stroke="#fff" stroke-opacity=".35" stroke-width="1.2"/>'
            '%s') % (x, y + 2, w, h, x, y, w, h - 2, x + .6, y + .6, w - 1.2, h - 3.2,
                     txt(cx, cy + 6, "TO FREQ", 18, "#ffffff", "middle", 600))


def press_png():
    cx, cy, w, h = SNAP
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d"><rect x="0" y="0" width="%d" height="%d" rx="8" fill="#000" opacity=".28"/></svg>' % (w, h, w, h - 2), w, h)


# the graph's touch plates (skin x0, y0, x1, y1): one left of OUT, full height, one under OUT; together they cover the graph
# screen but OUT itself, so nothing overlaps
_GX0, _GY0, _GX1, _GY1 = GRAPH_BOX[0] + 8, GRAPH_BOX[1] + 4, GRAPH_BOX[0] + GRAPH_BOX[2] - 4, GRAPH_BOX[1] + GRAPH_BOX[3] - 4
SHIELDS = [(_GX0, _GY0, SC_BTN[0] - 2, _GY1), (SC_BTN[0] - 2, SC_BTN[1] + SC_BTN[3] + 2, _GX1, _GY1)]


def shield_png(i):
    x0, y0, x1, y1 = SHIELDS[i]
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d"/>' % (x1 - x0, y1 - y0), x1 - x0, y1 - y0)


def clear_png():
    cx, cy, w, h = SNAP
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d"/>' % (w, h), w, h)


def background():
    W = NCOL * COL_W
    gx, gy, gw, gh = GRAPH_BOX
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="628" viewBox="0 0 1280 628">', DEFS,
         # the brushed plate, a bevelled edge and four screws
         '<rect width="1280" height="628" fill="url(#plate)"/>',
         '<rect width="1280" height="628" fill="#000" filter="url(#brush)"/>',
         '<rect x="1" y="1" width="1278" height="626" rx="6" fill="none" stroke="#fff" stroke-opacity=".55" stroke-width="2"/>',
         '<rect x="3.5" y="3.5" width="1273" height="621" rx="6" fill="none" stroke="#7d7b74" stroke-opacity=".6" stroke-width="1.5"/>']
    for cx, cy in ((18, 18), (1262, 18), (18, 610), (1262, 610)):
        o.append(screw(cx, cy))
    if not logo_file():
        lx, ly, lw, lh = LOGO_BOX
        o.append(txt(lx + 24, ly + lh / 2 + 15, "DAVIS", 46, RED, "start", 900))
        o.append(txt(lx + 196, ly + lh / 2 + 15, "Spectral Sidechain", 46, RED, "start", 400))
        o.append(txt(lx + lw - 24, ly + lh / 2 + 8, "one-band sidechain dynamic EQ", 17, "#5d5c56", "end", 400))
    # the graph: a dark screen in a black bezel, under glass
    o.append('<rect x="%d" y="%d" width="%d" height="%d" rx="12" fill="url(#bezel)" filter="url(#lift)"/>' % (gx - 4, gy - 4, gw + 8, gh + 8))
    o.append('<rect x="%d" y="%d" width="%d" height="%d" rx="8" fill="url(#screen)" stroke="#000" stroke-width="1.5"/>' % (gx + 6, gy + 6, gw - 12, gh - 12))
    for db in (-30, -20, -10, 0, 10, 20, 30):
        y = plot_y(db)
        o.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s" stroke-width="%s"/>' % (PLOT_X, y, PLOT_X + W, y, "#5a5f66" if db == 0 else "#262b31", "2" if db == 0 else "1"))
        o.append(txt(PLOT_X - 8, y + 5, "%d" % db, 15, "#8a939c", "end"))
    for hz in (20, 30, 40, 50, 60, 80, 100, 200, 300, 400, 500, 600, 800, 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 20000):
        x = plot_x(hz)
        o.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="#262b31" stroke-width="1"/>' % (x, PLOT_Y, x, PLOT_Y + PLOT_H))
    for hz, lab in ((20, "20"), (30, "30"), (50, "50"), (100, "100"), (200, "200"), (500, "500"), (1000, "1k"), (2000, "2k"), (5000, "5k"), (10000, "10k"), (20000, "20k")):
        o.append(txt(plot_x(hz), PLOT_Y + PLOT_H + 24, lab, 15, "#8a939c", "middle"))
    o.append('<rect x="%d" y="%d" width="%d" height="%d" rx="8" fill="url(#glass)"/>' % (gx + 6, gy + 6, gw - 12, gh - 12))
    # Freq cell: a raised plate with a deep groove for the big fader
    sx, sy, sw, sh = SL_BOX
    o.append(cell(sx, sy, sw, sh, 16))
    o.append(txt(sx + 24, TITLE_CY + 8, "Freq", 22, "#26292e", weight=700))
    o.append(snap_art())
    # a sunken capsule the thumb rides in, with a thin grey line along it
    px, pw, ph = SL_CX - SL_W // 2 - 6, SL_W + 12, SL_H - 4
    py = SL_CY - ph / 2 - 4
    # the capsule is cut into the plate, lit from above: the cut's upper wall is in shade and throws a soft shadow onto
    # the bed, the lower lip catches the light. Nothing stands above the plate
    o.append('<rect x="%g" y="%g" width="%d" height="%d" rx="%g" fill="url(#cutv)"/>' % (px - 3, py - 3, pw + 6, ph + 6, ph / 2 + 3))
    o.append('<rect x="%g" y="%g" width="%d" height="%d" rx="%g" fill="url(#bed)"/>' % (px, py, pw, ph, ph / 2))
    o.append('<clipPath id="pc"><rect x="%g" y="%g" width="%d" height="%d" rx="%g"/></clipPath>' % (px, py, pw, ph, ph / 2))
    o.append('<g clip-path="url(#pc)"><rect x="%g" y="%g" width="%d" height="%d" rx="%g" fill="none" stroke="#000" stroke-opacity=".38" stroke-width="16" filter="url(#soft)"/></g>' % (
        px - 4, py - 9, pw + 8, ph + 4, ph / 2))
    o.append('<path d="M%g %g h%d" stroke="#fff" stroke-opacity=".85" stroke-width="2" stroke-linecap="round"/>' % (px + ph / 2, py + ph + 3.5, pw - ph))
    tx0, tw = SL_CX - SL_W // 2 + 44, SL_W - 88
    o.append('<rect x="%d" y="%g" width="%d" height="16" rx="8" fill="#a29f98"/>' % (tx0, SL_CY - 12, tw))
    o.append('<clipPath id="gc"><rect x="%d" y="%g" width="%d" height="16" rx="8"/></clipPath>' % (tx0, SL_CY - 12, tw))
    o.append('<g clip-path="url(#gc)"><rect x="%d" y="%g" width="%d" height="16" rx="8" fill="none" stroke="#3e3c37" stroke-opacity=".55" stroke-width="5" filter="url(#tiny)"/></g>' % (tx0 - 2, SL_CY - 16, tw + 4))
    o.append('<path d="M%d %g h%d" stroke="#fff" stroke-opacity=".8" stroke-width="1.5" stroke-linecap="round"/>' % (tx0 + 8, SL_CY + 5, tw - 16))
    o.append(txt(SL_CX - SL_W // 2 + 20, SL_CY + 84, "15 Hz", 15, "#5d5c56"))
    o.append(txt(SL_CX + SL_W // 2 - 20, SL_CY + 84, "20 kHz", 15, "#5d5c56", "end"))
    for lab, key, lo, hi, col, row in KNOBS:
        cx, cy2 = COLS_X[col], ROWS_Y[row]
        o.append(cell(cx - CELL_W // 2, cy2 - CELL_H // 2, CELL_W, CELL_H))
        o.append(knob_ticks(cx, cy2 + KNOB_DY, KNOB_R))
        o.append(txt(cx - CELL_W // 2 + 14, cy2 - CELL_H // 2 + 26, lab, 18, "#26292e", weight=500))
        o.append(txt(cx - CELL_W // 2 + 14, cy2 + CELL_H // 2 - 14, lo, 13, "#5d5c56"))
        o.append(txt(cx + CELL_W // 2 - 14, cy2 + CELL_H // 2 - 14, hi, 13, "#5d5c56", "end"))
    bx, by, bw, bh = STATUS_BOX
    o.append(cell(bx, by, bw, bh))
    o.append(txt(bx + 18, by + 112, "GR", 16, "#5d5c56", weight=700))
    o.append(txt(bx + 220, by + 112, "SC", 16, "#5d5c56", weight=700))
    o.append("</svg>")
    return "\n".join(o)


# ---------------------------------------------------------------- layout.conf
def layout():
    L = ["# DAVIS Spectral Sidechain. Written by tools/make_skin.py: edit the design there and rerun it, not this file.",
         "# A free band on top for the logo; the graph (the band's curve, the live ring), a big Freq slider, six knobs, mode/channel/GR/SC.",
         "# Layout y = skin y + 86.", "",
         "theme_ink=26292e", "theme_ink_dim=6b6a63", "theme_accent=26292e", "theme_accent_hi=3a3f46", "theme_line=a9a8a0",
         "theme_box=d6d5cd", "theme_lcd=d6d5cd", "theme_seg_active=26292e", "theme_seg_inactive=c9c8bf", "theme_seg_active_tx=f2f1ee",
         "art_css=skin.css", "", "[tab SPECTRAL SIDECHAIN]",
         # the user's own finished background (artwork/bg_user.png, 1280 x 628) replaces the drawn one when it is there
         'art file="artwork/%s"' % ("bg_user.png" if os.path.exists(os.path.join(VST, "artwork", "bg_user.png")) else "bg.svg")]
    lf = logo_file()
    if lf:
        lx, ly, lw, lh = LOGO_BOX
        L.append('art file="artwork/%s" x=%d y=%d w=%d h=%d fit=contain' % (lf, lx, ly + Y, lw, lh))
    bx, by, bw, bh = STATUS_BOX
    L += ["", "# mode and channel, GR and SC link",
          'enum_h cx=%d cy=%d label="" key=mode options="RECEIVE,SEND" sw=110 sh=44' % (bx + 120, by + 40 + Y),
          'enum_h cx=%d cy=%d label="" key=chan options="A,B,C,D" sw=42 sh=44' % (bx + 320, by + 40 + Y),
          'readout cx=%d cy=%d w=150 h=36 box=0 tsize=26 tcolor=26292e label="GR" key=gr' % (bx + 120, by + 106 + Y),
          'readout cx=%d cy=%d w=130 h=36 box=0 tsize=22 tcolor=26292e label="SC" key=link' % (bx + 320, by + 106 + Y), ""]
    cy = PLOT_Y + PLOT_H // 2 + Y
    colx = lambda c: PLOT_X + c * COL_W + COL_W // 2
    L += ["# SC view: the kick's spectrum, the ground behind everything (only while the SC button is on: the engine draws nothing else)"]
    for c in range(0, NCOL, KICK_EVERY):
        L.append(meter("k%d" % c, "kick_col.png", PLOT_X + c * COL_W + KICK_W // 2, cy, KICK_W, PLOT_H, SPEC_STEPS + 1, Y))
    L += [""]
    L += ["# OUT: the output's spectrum, under everything else, and its switch in the graph's corner"]
    for c in range(0, NCOL, KICK_EVERY):
        L.append(meter("s%d" % c, "spec_col.png", PLOT_X + c * COL_W + KICK_W // 2, cy, KICK_W, PLOT_H, SPEC_STEPS + 1, Y))
    L += [""]
    L += ["# the live curve (semi-transparent red): the bell's shape at the gain applied now, under everything else"]
    for c in range(0, NCOL, RING_EVERY):
        L.append(meter("l%d" % c, "live_col.png", PLOT_X + c * COL_W + COL_W, cy, LIVE_W, PLOT_H, LIVE_STEPS + 1, Y))
    L += ["", "# the live dot: the gain applied right now, on the band's column (under the curve, so the curve takes the touch)"]
    for c in range(0, NCOL, RING_EVERY):
        L.append(meter("h%d" % c, "ring.png", colx(c), cy, RING_W, RING_H, RING_STEPS + 2, Y))
    L += ["", "# the band's curve at full Amount, 1 dB a step. A touch on a column moves the band: across = Freq, height = Amount"]
    for c in range(0, NCOL, RING_EVERY):    # the band's curve: one 12 px column per 2 (the engine's CURVE_EVERY), half the repaints
        L.append(meter("c%d" % c, "curve_col.png", PLOT_X + c * COL_W + COL_W, cy, LIVE_W, PLOT_H, CURVE_STEPS + 1, Y))
    ox, oy, ow, oh = OUT_BTN
    L += ["", "# the graph is for looking only (Freq slider and knobs set the band): a clear plate over it takes any touch (noop)",
          ] + ['button cx=%d cy=%d w=%d h=%d label="" key=noop img=images/shield%d.png img_on=images/shield%d.png' % (
              (x0 + x1) // 2, (y0 + y1) // 2 + Y, x1 - x0, y1 - y0, i, i) for i, (x0, y0, x1, y1) in enumerate(SHIELDS)]
    sx_, sy_, sw_, sh_ = SC_BTN
    L += ["", "# SC view and OUT, over everything in the graph's corner",
          'toggle cx=%d cy=%d label="" key=scview ns=0 w=%d h=%d img=images/sc_off.png img_on=images/sc_on.png' % (sx_ + sw_ // 2, sy_ + sh_ // 2 + Y, sw_, sh_),
          'toggle cx=%d cy=%d label="" key=out ns=0 w=%d h=%d img=images/out_off.png img_on=images/out_on.png' % (ox + ow // 2, oy + oh // 2 + Y, ow, oh)]
    sx, sy, sw, sh = SL_BOX
    L += ["", "# Freq: the value at the right of the title row, a big slider below (the band is set by the slider and knobs only)",
          'readout cx=%d cy=%d w=180 h=40 box=0 tsize=26 tcolor=26292e label="Freq" key=freq' % (sx + sw - 110, TITLE_CY + Y),
          'slider_h cx=%d cy=%d w=%d h=%d ns=0 vs=0 strip=images/slider.png frames=%d label="Freq" key=freq' % (SL_CX, SL_CY + Y, SL_W, SL_H, SL_FRAMES), "",
          "# knobs: name and range are in the background, the live value under the knob"]
    for lab, key, lo, hi, col, row in KNOBS:
        L.append('knob cx=%d cy=%d r=%d bw=110 ns=0 vs=20 ink=26292e ink_dim=26292e strip=images/knob_r%d.png frames=%d label="%s" key=%s' % (
            COLS_X[col], ROWS_Y[row] + KNOB_DY + Y, KNOB_R + 8, KNOB_R + 8, FRAMES, lab, key))   # the widget's box is 2r+8: r+8 leaves room for the dome's shadow
    L += ["", "# SEND: the other controls are hidden (when=mode:RECEIVE), their plates washed out, a red frame round SEND and A-D", 'art file="artwork/dim.png" x=0 y=%d w=1280 h=628 when=mode:SEND' % Y]
    L += ["", 'qlinks "MAIN" = freq,q,ratio,thresh,attack,release,amount,mode']
    # SEND: only RECEIVE/SEND and A-D stay; every other control is hidden and can't be touched
    L = [l + " when=mode:RECEIVE" if l.split(" ", 1)[0] in ("meter", "knob", "slider_h", "readout", "button", "toggle") else l for l in L]
    return "\n".join(L) + "\n"


def main():
    os.makedirs(os.path.join(VST, "artwork"), exist_ok=True)
    os.makedirs(IMG, exist_ok=True)
    for f in os.listdir(IMG):
        os.remove(os.path.join(IMG, f))
    json.dump({"params": params()}, open(os.path.join(VST, "params.json"), "w"), indent=1)
    json.dump({"presets": presets()}, open(os.path.join(VST, "presets.json"), "w"), indent=1)
    json.dump(VST_JSON, open(os.path.join(VST, "vst.json"), "w"), indent=2)
    open(os.path.join(VST, "artwork", "bg.svg"), "w").write(background() + "\n")
    items = []
    for name, svg in (("curve_col", curve_strip(LIVE_W, PLOT_H)), ("ring", ring_strip()), ("live_col", live_strip()), ("spec_col", spec_strip()), ("kick_col", kick_strip()), ("out_on", out_btn(True)), ("out_off", out_btn(False)), ("sc_on", sc_btn(True)), ("sc_off", sc_btn(False)),
                      ("slider", slider_strip()), ("clear", clear_png()), ("shield0", shield_png(0)), ("shield1", shield_png(1)), ("press", press_png()), ("knob_r%d" % (KNOB_R + 8), knob_strip(KNOB_R))):
        items.append((os.path.join(IMG, name + ".png"), *svg))
    items.append((os.path.join(VST, "artwork", "dim.png"), *dim_png()))
    render(items)
    open(os.path.join(VST, "layout.conf"), "w").write(layout())
    print("wrote params (%d), layout.conf, %d images" % (len(params()), len(items)))


if __name__ == "__main__":
    main()
