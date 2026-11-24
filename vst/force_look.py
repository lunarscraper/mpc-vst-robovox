#!/usr/bin/env python3
"""Force-Look (copied from Rattler 1.1; Fader FX adds slider_h_frame/look_of for fix_slider.py): redraws a built MPC skin for reading at arm's length - large type, big rounded buttons and option
segments, arc knobs, white slider caps - on a colourful page of its own (gradient, soft glow, light rays, dark panels
behind the controls), after mpc-vst-plugins' gen_vst.py has written it. Controls are drawn with a transparent ground
(the Force shows RGBA control images over the page, as Jamiro's skin does), so they sit on the art.

vst.json runs it as its "skin_post" (gen_vst.py calls `python3 force_look.py <skin dir>` with the port's vst/
folder as working directory). It only repaints images the generator made, keeping every file name and size, so
TUI.json, the touch boxes and the Q-Links stay exactly as the generator built them:

  sh_knob_r<r>.png        arc knob: 270-degree track, value arc (from 12 o'clock when r is odd = bipolar),
                          blue face, white pointer line
  sh_slider_v_<w>x<h>*.png  vertical slider: rounded track, value fill from the bottom, white cap
  sh_pill_{on,off}.png    the on/off switch
  sh_btn_*_{on,off}.png   trigger buttons: rounded, ring, the layout label in a large font
  sh_seg_<key>_<o>_*.png  option segments (enum_h/enum_v): like buttons, the chosen one bright with a white ring
  sh_po_<n>_*.png, sh_pop_<t>_<key>.png   popup options and the list panel
  sh_bg_<t>.png           page backgrounds: the art, the generator's own baked parts (popup fields, readouts,
                          arrows: every pixel that is not the flat theme_bg colour), the frames as dark panels with a
                          coloured outline, and the texts of the decoration lines (below)

Decoration lines in layout.conf are comments for the generator, so it ignores them (shadow coordinates, like
every other layout line: x 0-1280, y 86-714):

  #@title  text="SWARM" sub="RATTLER 1.4"            page title at the top left, small line under it
  #@frame  x= y= w= h= [title="..."]                  rounded outline around a group of controls
  #@text   cx= cy= label="..." [size=26] [color=RRGGBB] [align=center|left|right]

Colours: the layout's theme_bg (the flat ground the generator paints and this script replaces), theme_ink (names),
theme_ink_dim (values), theme_lcd (popup fields) and one `#@look name=RRGGBB ...` line at the top level for
everything this script draws (LOOK below lists the names; a #@text color= may name one of them, e.g. color=sub). Needs Pillow and
fonts/DejaVuSans-Bold.ttf next to this file. A missing or foreign image stops the build (non-zero exit), nothing is
written in that case.
"""
import json
import math
import os
import re
import shlex
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
Y_OFF = 86
SS = 4
FONT = os.path.join(HERE, "fonts", "DejaVuSans-Bold.ttf")

LOOK = {   # Force-Look defaults; the layout's #@look lines override them
    "bg_top": "142e86", "bg_bottom": "2147b0", "glow": "4f86f0", "ray1": "6fb3ff", "ray2": "3f78ee", "ray_alpha": "40",
    "panel": "0b1a4a", "panel_alpha": "150", "frame_text": "0f2466",
    "bg": "1b3a9c", "frame": "6fb3ff", "title": "ffffff", "sub": "9cc8ff", "text": "dbe9ff",
    "knob_face": "2c5fd6", "knob_rim": "5a8ff0", "knob_track": "14307e", "knob_arc": "9fe2ff", "pointer": "ffffff",
    "slider_track": "14307e", "slider_fill": "9fe2ff", "slider_cap": "ffffff", "slider_cap_line": "1b3a9c",
    "btn": "2b5ccc", "btn_ring": "6fa2ff", "btn_text": "ffffff", "btn_down": "9fe2ff", "btn_down_text": "0f2466",
    "seg": "234fbd", "seg_ring": "4f82e8", "seg_text": "c9ddff", "seg_on": "3f78ee", "seg_on_ring": "ffffff",
    "seg_on_text": "ffffff",
    "pill_off": "234fbd", "pill_on": "9fe2ff", "pill_dot": "ffffff",
    "list": "122c7a", "list_ring": "6fb3ff", "opt": "1b3a9c", "opt_text": "dbe9ff", "opt_on": "9fe2ff",
    "opt_on_text": "0f2466",
}


def rgb(h, a=255):
    h = h.strip().lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


def font(px):
    return ImageFont.truetype(FONT, max(6, int(round(px))))


def slug(t):   # shadow_skin.slug()
    return "".join(c if c.isalnum() else "_" for c in t).strip("_") or "x"


# --- the layout ----------------------------------------------------------------------------------------------
def parse_fields(toks):
    out = {}
    for t in toks:
        k, _, v = t.partition("=")
        out[k] = v
    return out


def read_layout(path):
    """(top: {theme/look keys}, tabs: [{name, widgets, deco}])"""
    top, tabs = {}, []
    for raw in open(path, encoding="utf-8"):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#@"):
            toks = shlex.split(line[2:])
            kind, f = toks[0], parse_fields(toks[1:])
            if kind == "look":
                for k, v in f.items():
                    top["look_" + k] = v
            elif tabs:
                f["kind"] = kind
                tabs[-1]["deco"].append(f)
            continue
        if line.startswith("#"):
            continue
        m = re.match(r"\[tab (.+)\]$", line)
        if m:
            tabs.append({"name": m.group(1).strip(), "widgets": [], "deco": []})
            continue
        if not tabs:
            k, _, v = line.partition("=")
            top[k.strip()] = v.strip()
            continue
        if line.startswith("qlinks"):
            continue
        toks = shlex.split(line)
        w = parse_fields(toks[1:])
        w["kind"] = toks[0]
        tabs[-1]["widgets"].append(w)
    return top, tabs


def options_of(module_path):
    with open(module_path, encoding="utf-8") as f:
        mod = json.load(f)
    plist = mod.get("chain_params") or mod.get("params") or []   # module.json (Rattler) or params.json (Fader FX)
    return {p["key"]: p["options"] for p in plist if p.get("options")}


# --- drawing -------------------------------------------------------------------------------------------------
def canvas(w, h, under=None):
    """Controls are drawn with a transparent ground (under=None), so they sit on the page art."""
    im = Image.new("RGBA", (w * SS, h * SS), rgb(under) if under else (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def done(im, w, h):
    return im.resize((w, h), Image.LANCZOS)


def knob_frame(r, t, bipolar, L):
    s = 2 * r + 10
    im, d = canvas(s, s)
    c = (s * SS - 1) / 2.0

    def pt(rad, deg):
        a = math.radians(deg)
        return (c + rad * math.sin(a), c - rad * math.cos(a))

    def disc(x, y, rad, col):
        d.ellipse((x - rad, y - rad, x + rad, y + rad), fill=col)

    width = max(5, r // 7) * SS
    tr = (r + 1) * SS

    def arc(a0, a1, col):
        if a1 - a0 < 0.6:
            return
        n = max(2, int(math.ceil(a1 - a0)))
        outer = [pt(tr + width / 2.0, a0 + (a1 - a0) * i / n) for i in range(n + 1)]
        inner = [pt(tr - width / 2.0, a1 - (a1 - a0) * i / n) for i in range(n + 1)]
        d.polygon(outer + inner, fill=col)
        for a in (a0, a1):
            disc(*pt(tr, a), width / 2.0, col)

    ang = -135.0 + 270.0 * t
    start = 0.0 if bipolar else -135.0
    arc(-135.0, 135.0, rgb(L["knob_track"]))
    arc(min(start, ang), max(start, ang), rgb(L["knob_arc"]))
    face = (r - 7) * SS
    disc(c, c, face, rgb(L["knob_rim"]))
    disc(c, c, face - 3 * SS, rgb(L["knob_face"]))
    pw = max(4, r // 9) * SS
    (x0, y0), (x1, y1) = pt(face * 0.30, ang), pt(face * 0.88, ang)
    nx, ny = (y1 - y0), -(x1 - x0)
    ln = math.hypot(nx, ny) or 1.0
    nx, ny = nx / ln * pw / 2.0, ny / ln * pw / 2.0
    ink = rgb(L["pointer"])
    d.polygon([(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)], fill=ink)
    disc(x0, y0, pw / 2.0, ink)
    disc(x1, y1, pw / 2.0, ink)
    return done(im, s, s)


def knob_strip(r, frames, L):
    s = 2 * r + 10
    out = Image.new("RGBA", (s, s * frames), (0, 0, 0, 0))
    for k in range(frames):
        out.paste(knob_frame(r, k / (frames - 1), r % 2 == 1, L), (0, k * s))
    return out


def slider_frame(w, h, t, L):
    im, d = canvas(w, h)
    cap_h = max(18, min(30, h // 7))
    tw = max(8, w // 4)
    x0 = (w - tw) / 2.0
    pad = cap_h / 2.0 + 2
    d.rounded_rectangle((x0 * SS, pad * SS, (x0 + tw) * SS, (h - pad) * SS), radius=tw / 2.0 * SS, fill=rgb(L["slider_track"]))
    travel = h - 2 * pad
    cy = pad + (1.0 - t) * travel
    if t > 0.004:
        d.rounded_rectangle((x0 * SS, cy * SS, (x0 + tw) * SS, (h - pad) * SS), radius=tw / 2.0 * SS, fill=rgb(L["slider_fill"]))
    cw = w - 4
    d.rounded_rectangle((2 * SS, (cy - cap_h / 2.0) * SS, (2 + cw) * SS, (cy + cap_h / 2.0) * SS), radius=5 * SS,
                        fill=rgb(L["slider_cap"]))
    d.rectangle((8 * SS, (cy - 1) * SS, (w - 8) * SS, (cy + 1) * SS), fill=rgb(L["slider_cap_line"]))
    return done(im, w, h)


def slider_h_frame(w, h, t, L):
    """Horizontal slider (Fader FX's FADER): rounded track, value fill from the left, a tall white cap."""
    im, d = canvas(w, h)
    cap_w = max(22, min(40, w // 24))
    th = max(10, h // 3)
    y0 = (h - th) / 2.0
    pad = cap_w / 2.0 + 2
    d.rounded_rectangle((pad * SS, y0 * SS, (w - pad) * SS, (y0 + th) * SS), radius=th / 2.0 * SS, fill=rgb(L["slider_track"]))
    travel = w - 2 * pad
    cx = pad + t * travel
    if t > 0.004:
        d.rounded_rectangle((pad * SS, y0 * SS, cx * SS, (y0 + th) * SS), radius=th / 2.0 * SS, fill=rgb(L["slider_fill"]))
    d.rounded_rectangle(((cx - cap_w / 2.0) * SS, 2 * SS, (cx + cap_w / 2.0) * SS, (h - 2) * SS), radius=7 * SS,
                        fill=rgb(L["slider_cap"]))
    d.rectangle(((cx - 1.5) * SS, 10 * SS, (cx + 1.5) * SS, (h - 10) * SS), fill=rgb(L["slider_cap_line"]))
    return done(im, w, h)


def slider_h_strip(w, h, frames, L):
    out = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for k in range(frames):
        out.paste(slider_h_frame(w, h, k / (frames - 1), L), (0, k * h))
    return out


def look_of(layout_path):
    """The colours this script draws with, for fix_slider.py (it paints the FADER after cropping its strip)."""
    top, _ = read_layout(layout_path)
    L = dict(LOOK)
    if top.get("theme_bg"):
        L["bg"] = top["theme_bg"]
    for k, v in top.items():
        if k.startswith("look_"):
            L[k[5:]] = v
    return L


def slider_strip(w, h, frames, L):
    out = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for k in range(frames):
        out.paste(slider_frame(w, h, k / (frames - 1), L), (0, k * h))
    return out


def fit_font(d, label, w, h, start):
    px = start
    while px > 10:
        f = font(px)
        b = d.textbbox((0, 0), label, font=f)
        if b[2] - b[0] <= w and b[3] - b[1] <= h:
            return f, b
        px -= 1
    f = font(10)
    return f, d.textbbox((0, 0), label, font=f)


def box(w, h, fill, ring, ring_w, label, ink, L, radius=None, text_px=None):
    im, d = canvas(w, h)
    rad = radius if radius is not None else min(14, h // 3)
    d.rounded_rectangle((1 * SS, 1 * SS, (w - 1) * SS - 1, (h - 1) * SS - 1), radius=rad * SS, fill=rgb(ring))
    d.rounded_rectangle(((1 + ring_w) * SS, (1 + ring_w) * SS, (w - 1 - ring_w) * SS - 1, (h - 1 - ring_w) * SS - 1),
                        radius=max(2, rad - ring_w) * SS, fill=rgb(fill))
    im = done(im, w, h)
    if label.strip():
        d = ImageDraw.Draw(im)
        start = text_px or min(h * 0.46, 34)
        f, b = fit_font(d, label.strip(), w - 16, h - 8, start)
        d.text(((w - (b[2] - b[0])) / 2.0 - b[0], (h - (b[3] - b[1])) / 2.0 - b[1]), label.strip(), font=f, fill=rgb(ink))
    return im


def pill(w, h, on, L):
    im, d = canvas(w, h)
    d.rounded_rectangle((1 * SS, 1 * SS, (w - 1) * SS, (h - 1) * SS), radius=(h - 2) / 2.0 * SS,
                        fill=rgb(L["pill_on"] if on else L["pill_off"]))
    r = (h - 8) / 2.0
    cx = (w - 4 - r) if on else (4 + r)
    d.ellipse(((cx - r) * SS, (h / 2.0 - r) * SS, (cx + r) * SS, (h / 2.0 + r) * SS), fill=rgb(L["pill_dot"]))
    return done(im, w, h)


def page_art(W, H, L):
    """The page ground: a vertical gradient, a soft glow in the middle and faint light rays (all pages alike)."""
    from PIL import ImageFilter
    top, bot = rgb(L["bg_top"]), rgb(L["bg_bottom"])
    g = Image.new("RGBA", (1, H))
    for y in range(H):
        t = y / (H - 1.0)
        g.putpixel((0, y), tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)) + (255,))
    art = g.resize((W, H))
    rays = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(rays)
    cx, cy, R = W / 2.0, H * 0.48, W * 1.2
    n = 56
    for k in range(n):
        a0 = 2 * math.pi * k / n
        a1 = a0 + 2 * math.pi / n * 0.45
        col = rgb(L["ray1"] if k % 2 else L["ray2"], int(L.get("ray_alpha", "60")))
        d.polygon([(cx, cy), (cx + R * math.cos(a0), cy + R * math.sin(a0)), (cx + R * math.cos(a1), cy + R * math.sin(a1))], fill=col)
    rays = rays.filter(ImageFilter.GaussianBlur(3))
    art.alpha_composite(rays)
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((cx - 330, cy - 230, cx + 330, cy + 230), fill=rgb(L["glow"], 150))
    art.alpha_composite(glow.filter(ImageFilter.GaussianBlur(110)))
    return art


def page_overlay(bg, deco, L):
    """A page: the art ground, the generator's own baked parts (popup fields, readouts, arrows - everything that
    is not the flat theme_bg colour), dark panels with a coloured outline for the frames, and the texts."""
    W, H = bg.size
    plate = rgb(L["bg"])[:3]
    out = page_art(W, H, L)
    panels = Image.new("RGBA", (W * 2, H * 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(panels)
    for f in deco:
        if f["kind"] == "frame":
            x, y, w, h = (int(f[k]) for k in ("x", "y", "w", "h"))
            y -= Y_OFF
            d.rounded_rectangle((x * 2, y * 2, (x + w) * 2, (y + h) * 2), radius=30, fill=rgb(L["panel"], int(L.get("panel_alpha", "150"))),
                                outline=rgb(L["frame"]), width=5)
    out.alpha_composite(panels.resize((W, H), Image.LANCZOS))
    src = bg.convert("RGB")
    mask = Image.new("L", (W, H), 0)
    sp, mp = src.load(), mask.load()
    for y in range(H):
        for x in range(W):
            p = sp[x, y]
            if abs(p[0] - plate[0]) + abs(p[1] - plate[1]) + abs(p[2] - plate[2]) > 30:
                mp[x, y] = 255
    out.paste(src, (0, 0), mask)
    d = ImageDraw.Draw(out)
    for f in deco:
        if f["kind"] == "frame" and f.get("title"):
            x, y = int(f["x"]), int(f["y"]) - Y_OFF
            fo = font(21)
            tb = d.textbbox((0, 0), f["title"], font=fo)
            tw, th = tb[2] - tb[0], tb[3] - tb[1]
            d.rounded_rectangle((x + 20, y - 15, x + 40 + tw, y + 15), radius=13, fill=rgb(L["frame"]))
            d.text((x + 30 - tb[0], y - th / 2.0 - tb[1]), f["title"], font=fo, fill=rgb(L["frame_text"]))
        elif f["kind"] == "title":
            fo = font(int(f.get("size", 40)))
            tb = d.textbbox((0, 0), f["text"], font=fo)
            base = 52   # baseline of the title row (the frames start at 70)
            d.text((26, base - tb[3]), f["text"], font=fo, fill=rgb(L["title"]))
            if f.get("sub"):
                fs = font(21)
                sb = d.textbbox((0, 0), f["sub"], font=fs)
                d.text((26 + tb[2] + 20, base - sb[3]), f["sub"], font=fs, fill=rgb(L["sub"]))
        elif f["kind"] == "text":
            fo = font(float(f.get("size", 28)))
            lab = f["label"]
            tb = d.textbbox((0, 0), lab, font=fo)
            tw, th = tb[2] - tb[0], tb[3] - tb[1]
            cx, cy = int(f["cx"]), int(f["cy"]) - Y_OFF
            al = f.get("align", "center")
            x = cx - tw / 2.0 if al == "center" else (cx if al == "left" else cx - tw)
            d.text((x - tb[0], cy - th / 2.0 - tb[1]), lab, font=fo, fill=rgb(L.get(f.get("color"), f.get("color")) or L["text"]))
    return out.convert("RGB")


# --- the polish ----------------------------------------------------------------------------------------------
def walk_defs(tui):
    """{definition key: [(component name, component data)]}"""
    out = {}
    for e in tui["pageData"]["componentDefinitions"]["localComponentDefinitions"]:
        out[e["key"]] = [(c["componentData"]["name"], c["componentData"]["data"]) for c in e["value"].get("componentsData", [])]
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: force_look.py <skin folder>")
    skin = sys.argv[1]
    if not os.path.isfile(os.path.join(skin, "TUI.json")):
        skin = os.path.join(skin, "Plugin Skins")
    tui_path = os.path.join(skin, "TUI.json")
    if not os.path.isfile(tui_path):
        sys.exit("force_look: no TUI.json under %s" % sys.argv[1])
    with open(os.path.join(HERE, "vst.json"), encoding="utf-8") as f:
        cfg = json.load(f)
    top, tabs = read_layout(os.path.join(HERE, cfg["layout"]))
    opts = options_of(os.path.join(HERE, cfg.get("module") or cfg["params"]))
    L = dict(LOOK)
    if top.get("theme_bg"):
        L["bg"] = top["theme_bg"]
    for k, v in top.items():
        if k.startswith("look_"):
            L[k[5:]] = v
    tui = json.load(open(tui_path, encoding="utf-8"))
    defs = walk_defs(tui)
    jobs, errors = {}, []

    def size_of(name):
        p = os.path.join(skin, name)
        if not os.path.isfile(p):
            errors.append("%s: missing" % name)
            return None
        with Image.open(p) as im:
            return im.size

    def add(name, fn):
        sz = size_of(name)
        if sz:
            jobs[name] = (fn, sz)

    buttons = {}   # stem -> (label off, label on)
    for t in tabs:
        for w in t["widgets"]:
            if w["kind"] == "button":
                lo = w.get("label", "")
                ln = w.get("label_on") or lo
                buttons["sh_btn_%s_%s" % (w["key"], slug(lo + ("_" + ln if ln != lo else "")))] = (lo, ln)
    enum_opts = {}
    for t in tabs:
        for w in t["widgets"]:
            if w["kind"] in ("enum_h", "enum_v", "popup"):
                enum_opts[w["key"]] = w["options"].split(",") if w.get("options") else opts.get(w["key"], [])

    for key, comps in defs.items():
        for cname, data in comps:
            fs = data.get("filmStrip", "")
            if fs.startswith("sh_knob_r"):
                m = re.match(r"sh_knob_r(\d+)\.png$", fs)
                if not m:
                    errors.append("%s: a knob look this script doesn't know" % fs)
                    continue
                r = int(m.group(1))
                add(fs, lambda sz, r=r: knob_strip(r, sz[1] // sz[0], L))
            elif fs.startswith("sh_slider_v_"):
                m = re.match(r"sh_slider_v_(\d+)x(\d+)(?:_f\d+)?\.png$", fs)
                if not m:
                    errors.append("%s: a slider this script doesn't know" % fs)
                    continue
                sw, sh = int(m.group(1)), int(m.group(2))
                add(fs, lambda sz, sw=sw, sh=sh: slider_strip(sw, sh, sz[1] // sh, L))
            on, off = data.get("onImage", ""), data.get("offImage", "")
            if on.startswith("sh_pill_"):
                add("sh_pill_on.png", lambda sz: pill(sz[0], sz[1], True, L))
                add("sh_pill_off.png", lambda sz: pill(sz[0], sz[1], False, L))
            elif on.startswith("sh_btn_"):
                stem = on[:-len("_on.png")]
                if stem not in buttons:
                    errors.append("%s: no button in the layout makes it" % on)
                    continue
                lo, ln = buttons[stem]
                add(off, lambda sz, lo=lo: box(sz[0], sz[1], L["btn"], L["btn_ring"], 3, lo, L["btn_text"], L))
                add(on, lambda sz, ln=ln: box(sz[0], sz[1], L["btn_down"], "ffffff", 3, ln, L["btn_down_text"], L))
            elif on.startswith("sh_seg_"):
                m = re.match(r"shSeg_(.+)_(\d+)$", key)
                if not m or m.group(1) not in enum_opts:
                    errors.append("%s: no option group in the layout makes it" % on)
                    continue
                lab = enum_opts[m.group(1)][int(m.group(2))]
                add(off, lambda sz, lab=lab: box(sz[0], sz[1], L["seg"], L["seg_ring"], 2, lab, L["seg_text"], L, radius=10))
                add(on, lambda sz, lab=lab: box(sz[0], sz[1], L["seg_on"], L["seg_on_ring"], 3, lab, L["seg_on_text"], L, radius=10))
            elif on.startswith("sh_po_") or on.startswith("sh_popopt_"):   # the latter: older generators name them so
                m = re.match(r"shPopOpt_\d+_(.+)_(\d+)$", key)
                if not m or m.group(1) not in enum_opts:
                    errors.append("%s: no popup in the layout makes it" % on)
                    continue
                lab = enum_opts[m.group(1)][int(m.group(2))]
                add(off, lambda sz, lab=lab: box(sz[0], sz[1], L["opt"], L["opt"], 0, lab, L["opt_text"], L, radius=6, text_px=32))
                add(on, lambda sz, lab=lab: box(sz[0], sz[1], L["opt_on"], L["opt_on"], 0, lab, L["opt_on_text"], L, radius=6, text_px=32))
            img = data.get("image", "")
            if img.startswith("sh_pop_"):
                add(img, lambda sz: box(sz[0], sz[1], L["list"], L["list_ring"], 3, "", L["text"], L, radius=12))

    for t, tab in enumerate(tabs):
        name = "sh_bg_%d.png" % t
        if tab["deco"]:
            sz = size_of(name)
            if sz:
                jobs[name] = (lambda sz, name=name, deco=tab["deco"]: page_overlay(Image.open(os.path.join(skin, name)), deco, L), sz)

    if errors:
        sys.exit("force_look: refused, nothing written:\n  " + "\n  ".join(sorted(set(errors))))
    for name, (fn, sz) in sorted(jobs.items()):
        im = fn(sz)
        if im.size != sz:
            sys.exit("force_look: %s came out %dx%d, the generator made %dx%d" % ((name,) + im.size + sz))
        im.save(os.path.join(skin, name))
    print("force_look: %d images redrawn" % len(jobs))


if __name__ == "__main__":
    main()
