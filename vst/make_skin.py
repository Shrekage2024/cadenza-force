#!/usr/bin/env python3
"""Generate Cadenza's skin inputs: params.json, layout.conf and images/ (needs Pillow), and build/keyflags.h (src/engine.c's key flags).

The parameter order is the VST index that saved projects and the skin bind to: append only, never reorder.
src/engine.c parses these key names, so keep them in step with it.
Run: python3 vst/make_skin.py   (build.sh runs it in the toolchain's mpc-vst-html-art image)
"""
import collections
import json
import os
import re

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
IMG = os.path.join(HERE, "images")

LANES = [  # the skin's lanes: name, colour (lane 4, Melody 2, is retired in 0.7 and leaves the skin)
    ("CHORDS", (242, 163, 58)),
    ("BASS", (110, 155, 245)),
    ("MELODY", (60, 198, 168)),
]
UNUSED = "(unused) "   # a retired param's name prefix, so MPC's lists show it as retired
PARAM_LANES = ["CHORDS", "BASS", "MELODY 1", "MELODY 2"]   # the four lanes params.json was built from: append only, so they stay
STEPS, CHORD_TILES = 16, 8

# geometry (layout coordinates: the plugin area spans y 86..714, so layout y = plugin y + 86). Touch targets are at least
# 64 px and 8 px apart, steps 78 tall; every 64 px target is a list cell, a stepper, a popup or a sized image button
L, R = 16, 1264                 # page margins
GX = 148                        # PLAY: the step grid's left edge, right of the 120 px lane names
CELL_GAP, CELL_H = 6, 78
CELL_W = (R - GX - (STEPS - 1) * CELL_GAP) // STEPS          # 64
GW = STEPS * CELL_W + (STEPS - 1) * CELL_GAP
HEAD_Y, LINE_Y, BAR_Y, PH_Y, PH_H, ROW0, ROW_PITCH = 94, 164, 194, 268, 10, 286, 86   # LINE_Y: PLAY's 24 px text line
BAR_W, VCHORD_W = 690, 186      # the bar strip: 8 cells of 81, then the view stepper and vchord ("F#dimadd9  iio" at 22 px)
KNOB_Y = ROW0 + 3 * ROW_PITCH + 92

PROGRESSIONS = [  # src/theory.c THEORY_PRESETS, in order
    "1-6-3-7 Epic", "1-5-6-4 Pop", "6-4-1-5 Sad Pop", "1-6-4-5 50s", "1-4-5-1 Classic", "1-4-1-5 Blues",
    "1-7-6-5 Andalusian", "1-7-6-7 Rock", "4-5-3-6 Royal Road", "1-4-6-5 Lift", "2-5-1-1 Jazz", "1-3-4-4 Dream",
    "1-4 Vamp", "1-2 Float", "1 Drone", "Canon (8)"]

CELL_OFF, CELL_LINE = (33, 36, 41), (44, 48, 56)
CHORD_ON = (196, 122, 28)
AMBER, MUTED = (242, 163, 58), (180, 72, 60)
TILE_OFF, TILE_ON = (34, 37, 43), (42, 33, 22)
KEY_W, KEY_B = (226, 222, 214), (44, 48, 56)

# list cell and button sizes (w, h) that get their own tile_off / tile_on pictures, so no outline is stretched: cells() adds
# each size it lays out (0.12), so the layout runs before the pictures; TILES holds the sizes only an img= line names
TILES = {(120, CELL_H)}   # PLAY's lane names (mute)
HITS, HITS_ON = set(), set()   # 0.18 the hit plates' sizes, and those lit amber (hit_plate adds each it lays out)
KEY_WW, KEY_WH, KEY_BW, KEY_BH, KEY_GAP = 78, 78, 64, 64, 8   # white keys, black keys, the gap between white keys


def tile(w, h, fill, line=None, width=1):
    im = Image.new("RGB", (w, h), fill)
    if line:
        ImageDraw.Draw(im).rectangle([0, 0, w - 1, h - 1], outline=line, width=width)
    return im


def save(im, name):
    im.save(os.path.join(IMG, name))


def make_images():
    os.makedirs(IMG, exist_ok=True)
    for f in os.listdir(IMG):   # a picture the layout no longer uses would still ship
        if f.endswith(".png"):
            os.remove(os.path.join(IMG, f))
    save(tile(CELL_W, CELL_H, CELL_OFF, CELL_LINE), "cell_off.png")
    for n, (_, colour) in enumerate(LANES, 1):
        save(tile(CELL_W, CELL_H, colour), "cell_l%d_on.png" % n)
        save(tile(4, CELL_H, colour), "lane_%d.png" % n)   # the lane's colour bar beside its name on PLAY
    for w, h in sorted(TILES):   # idle, and selected (amber outline)
        save(tile(w, h, TILE_OFF, CELL_LINE), "tile_off_%dx%d.png" % (w, h))
        save(tile(w, h, TILE_ON, AMBER, 3), "tile_on_%dx%d.png" % (w, h))
    save(tile(120, CELL_H, MUTED), "mute_on.png")
    save(tile(KEY_WW, KEY_WH, KEY_W, CELL_LINE), "key_w_off.png")
    save(tile(KEY_WW, KEY_WH, AMBER), "key_w_on.png")
    save(tile(KEY_BW, KEY_BH, KEY_B, CELL_LINE), "key_b_off.png")
    save(tile(KEY_BW, KEY_BH, CHORD_ON), "key_b_on.png")
    chords_images()


def rounded(w, h, fill, line=None, width=1, radius=8, mode="RGB", page=(15, 16, 18)):
    """A rounded box on the page colour (or transparent, mode RGBA with page None)"""
    im = Image.new(mode, (w, h), page if page else (0, 0, 0, 0))
    ImageDraw.Draw(im).rounded_rectangle([0, 0, w - 1, h - 1], radius, fill=fill, outline=line, width=width)
    return im


def arrow(w, h, fill, line, ink, right, width=1):
    """A page arrow cell: a tile with a triangle pointing left or right"""
    im = tile(w, h, fill, line, width)
    cx, cy, s = w // 2, h // 2, 10
    pts = [(cx - s, cy - s), (cx + s, cy), (cx - s, cy + s)] if right else [(cx + s, cy - s), (cx - s, cy), (cx + s, cy + s)]
    ImageDraw.Draw(im).polygon(pts, fill=ink)
    return im


def chords_images():
    """0.17 the CHORDS page's own pictures: the hit plates over the fields, the keyboard's keys. 0.24: the sound and PHRASE pages'
    (sp_images), the centre chord's frame, the browsers' full-width panel"""
    panel, field = (22, 24, 27), (29, 32, 37)
    drift_edges()
    save(rounded(R - L, CM_H, panel, CELL_LINE, radius=10), "panel_%dx%d.png" % (R - L, CM_H))
    rhythm_images(panel, field)
    sp_images(panel, field)
    melody_images(panel, field)
    jam_images_22()
    for w, h in sorted(HITS):   # alpha 1: MPC keeps a tap on an image it would not show (the toolchain's hit plate)
        save(Image.new("RGBA", (w, h), (0, 0, 0, 1)), "hit_%dx%d.png" % (w, h))
    for w, h in sorted(HITS_ON):
        save(rounded(w, h, (242, 163, 58, 40), AMBER + (255,), 3, mode="RGBA", page=None), "hit_on_%dx%d.png" % (w, h))
    for name_, (w, h), off in (("kw", (KW_W, CK_H), (216, 212, 204)), ("kb", (KB_W, KB_H), (34, 37, 43)), ("ke", (KE_W, CK_H), (26, 28, 32))):
        save(tile(w, h, off), "key_%s_off.png" % name_)
        save(tile(w, h, AMBER), "key_%s_on.png" % name_)
    for name_, (w, h) in (("kw", (KW_W, CK_H)), ("kb", (KB_W, KB_H)), ("ke", (KE_W, CK_H))):   # 0.19 the Bass lane's KEYS
        save(tile(w, h, LANES[1][1]), "key_u%s_on.png" % name_)


def drift_edges():
    """0.24 the CHORDS centre's frame in its drift colour (cdrift 0..3): the frame's four edges, each a 4-frame filmstrip (a meter on cdrift,
    over the centre's box), so the four colours cost four thin strips instead of four mode images (0.17's drift_N, 0.64 MB). A strip wider
    than tall has its frames side by side (the toolchain reads it so: skin_assets.strip_layout)"""
    w, h = SR_MW, SR_H
    strips = {k: Image.new("RGBA", (ew * 4, eh) if ew > eh else (ew, eh * 4), (0, 0, 0, 0)) for k, (_, _, ew, eh) in DRIFT_EDGES.items()}
    for level, colour in enumerate(DRIFT_COLOURS):
        im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(im).rounded_rectangle([0, 0, w - 1, h - 1], 12, outline=colour + (255,), width=3)
        for k, (x, y, ew, eh) in DRIFT_EDGES.items():
            strips[k].paste(im.crop((x, y, x + ew, y + eh)), (level * ew, 0) if ew > eh else (0, level * eh))
    for k, (_, _, ew, eh) in DRIFT_EDGES.items():
        save(strips[k], "drift_%s_%dx%d.png" % (k, ew, eh))


def rhythm_images(panel, field):
    """0.18 the RHYTHMS panel (amber, as the canvas), COPY TO...'s wells and the 64 px arrows. 0.24: the editor and COPY TO... panels
    of PHRASE, framed in the lane's colour (CHORDS amber, BASS its blue)"""
    w = R - L
    save(rounded(w, CM_H, panel, AMBER, 2, radius=10), "rpanel_%dx%d.png" % (w, CM_H))
    save(rounded(w, CM_H, panel, LANES[1][1], 2, radius=10), "upanel_%dx%d.png" % (w, CM_H))
    ww = RCB_W + 8 - RCP_GAP   # COPY TO...'s inner cells: a well, dark on a locked bar
    save(tile(ww, RCP_H, field), "well_%dx%d_off.png" % (ww, RCP_H))
    save(tile(ww, RCP_H, (15, 16, 18), CELL_LINE), "well_%dx%d_on.png" % (ww, RCP_H))
    for side, right in (("l", False), ("r", True)):
        save(arrow(64, 64, TILE_OFF, CELL_LINE, (233, 230, 223), right), "arrow64_%s_off.png" % side)
        save(arrow(64, 64, TILE_ON, AMBER, AMBER, right, 3), "arrow64_%s_on.png" % side)


def sp_images(panel, field):
    """0.24 the sound and PHRASE pages' pictures: the two panels, a left-panel field's box, the centre's box, NOW PLAYING's boxes, the
    legend's gradient, the dot filmstrips (CHORDS and BASS: the centre's, the editor's, the strip's) and the steps' marks"""
    for w in (SP_LW, SP_RW):
        save(rounded(w, SP_H, panel, CELL_LINE, radius=10), "panel_%dx%d.png" % (w, SP_H))
    save(rounded(SF_W, 64, field, CELL_LINE, page=panel), "sfield_%dx64.png" % SF_W)
    save(rounded(SR_MW, SR_H, field, CELL_LINE, radius=12, page=panel), "scentre_%dx%d.png" % (SR_MW, SR_H))
    for k, now in (("c", "cjnow"), ("u", "ujnow")):
        save(rounded(SN_W, CS_H, *JAM_NOW_22[now]), "snow_%s_%dx%d.png" % (k, SN_W, CS_H))
    save(legend_bar(140, 8), "legend_140x8.png")
    for size, tag in ((PD_C, "c"), (PD_E, "e"), (PD_S, "s")):
        chords_dot_strip(size[0], size[1], "cqdots_%s.png" % tag)
        bass_dot_strip(size[0], size[1], size[2], "uqdots_%s.png" % tag)
    im = Image.new("RGBA", (PE_SW * 4, MARK_H), (0, 0, 0, 0))   # the steps' marks, side by side (wider than tall: drift_edges): 0 none,
    d = ImageDraw.Draw(im)                                       # 1 a chord starts (amber), 2 a hit the knobs add (dashed), 3 both
    for f, colour in ((1, AMBER), (2, (0x6b, 0x71, 0x7a)), (3, AMBER)):
        left = f * PE_SW
        if f == 1:
            d.rectangle([left + 2, 1, left + PE_SW - 3, MARK_H - 1], fill=colour)
        else:
            for x in range(2, PE_SW - 2, 10):
                d.rectangle([left + x, 1, left + min(x + 5, PE_SW - 3), MARK_H - 1], fill=colour)
    save(im, "marks_%dx%d.png" % (PE_SW, MARK_H))


def legend_bar(w, h):
    """0.24 the legend's gradient (the boards' #3fbf6f, #9ccf4f 30%, #e3b23c 55%, #e8833a 75%, #d9534f), rounded, on the page colour"""
    stops = list(zip((0.0, 0.3, 0.55, 0.75, 1.0), DOT_TENSION))
    im = Image.new("RGB", (w, h), (15, 16, 18))
    bar = Image.new("RGB", (w, h))
    for x in range(w):
        t = x / (w - 1.0)
        k = max(i for i, (s, _) in enumerate(stops) if s <= t or i == 0)
        k = min(k, len(stops) - 2)
        (s0, c0), (s1, c1) = stops[k], stops[k + 1]
        f = (t - s0) / (s1 - s0)
        ImageDraw.Draw(bar).line([(x, 0), (x, h - 1)], fill=tuple(round(a + (b - a) * f) for a, b in zip(c0, c1)))
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w - 1, h - 1], h // 2, fill=255)
    im.paste(bar, (0, 0), mask)
    return im


def chords_dot_strip(w, h, name):
    """0.24 a CHORDS dot filmstrip (src/engine.c chords_frame), exactly as the Task 5 probe drew it (probe/dots_chords/make_dots.py strip(),
    pixel-checked on the Force): 0 a rest (the step's baseline); 1 + height x 5 + colour a hit, a column from the baseline (GHOST 0.3 of the
    span, HIT 0.6, ACCENT all) in the chord's drift colour; 16 + colour a hold, a low line; 21 + height x 5 + colour a hit that starts a chord,
    the column and a 4 px amber tick at the frame's top, 2 px clear over an ACCENT"""
    img = Image.new("RGBA", (w, h * 36), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    tick_y, top_y, bot_y = 1, 7, h - 5
    span, line = bot_y - top_y, max(2, h // 24)

    def column(top, height, colour):
        y = top + bot_y - round(span * (0.3, 0.6, 1.0)[height])
        d.rounded_rectangle([1, y, w - 2, top + bot_y], radius=max(1, min(w // 4, 3)), fill=DOT_TENSION[colour])

    for f in range(36):
        top = f * h
        d.rectangle([0, top + h - 3, w - 1, top + h - 2], fill=DOT_BASE)
        if 1 <= f <= 15:
            column(top, *divmod(f - 1, DOT_COLOURS))
        elif 16 <= f <= 20:
            y = top + bot_y - round(span * 0.3) // 2
            d.rectangle([0, y, w - 1, y + line - 1], fill=DOT_TENSION[f - 16])
        elif f >= 21:
            column(top, *divmod(f - 21, DOT_COLOURS))
            d.rectangle([1, top + tick_y, w - 2, top + tick_y + 3], fill=AMBER)
    save(img, name)


def bass_dot_strip(w, h, dot_h, name):
    """0.24 a BASS dot filmstrip (src/engine.c bass_frame): MELODY's rule (dot_strip) over the bass range, its top 7 rows kept for the
    chord-change tick: 0 a rest; 1 + level x 5 + colour a note; 61 + level a held step; 73 + level x 5 + colour a note that starts a chord,
    with the 4 px amber tick at the frame's top (as CHORDS's)"""
    frames = 1 + DOT_LEVELS * DOT_COLOURS + DOT_LEVELS + DOT_LEVELS * DOT_COLOURS
    img = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad, top_y = max(2, dot_h // 2), 7
    span = h - top_y - pad - dot_h

    def note(top, level, colour):
        y = top + top_y + span - round(span * level / (DOT_LEVELS - 1))
        d.rounded_rectangle([1, y, w - 2, y + dot_h], radius=max(1, dot_h // 3), fill=DOT_TENSION[colour])
        return y

    for f in range(frames):
        top = f * h
        d.rectangle([0, top + h - 3, w - 1, top + h - 2], fill=DOT_BASE)
        n = f - 1
        if 0 <= n < DOT_LEVELS * DOT_COLOURS:
            note(top, *divmod(n, DOT_COLOURS))
        elif 0 <= n - DOT_LEVELS * DOT_COLOURS < DOT_LEVELS:
            level = n - DOT_LEVELS * DOT_COLOURS
            y = top + top_y + span - round(span * level / (DOT_LEVELS - 1)) + dot_h // 2 - 1
            d.rectangle([0, y, w - 1, y + max(2, dot_h // 3)], fill=DOT_HOLD)
        elif f > 0:
            note(top, *divmod(n - DOT_LEVELS * DOT_COLOURS - DOT_LEVELS, DOT_COLOURS))
            d.rectangle([1, top + 1, w - 2, top + 4], fill=AMBER)
    save(img, name)


# 0.13 stepped knobs (plan decision 5): key, name, the plain step names (dynamic_display gives the live text), the default
# step (the one nearest the old param's default). COLOUR's 17 steps depend on the progression (src/seq_core.c
# seq_part_colour_steps); past the last one they clamp.
STEPPED = [("k_harm", "Harmony Step", ["Common", "Colourful", "Rare", "Rare + borrow"], 0),
           ("k_col", "Chords Colour Step", ["Triads"] + ["Colour %d" % i for i in range(1, 17)], 0),
           ("k_spr", "Chords Spread Step", ["Close", "Open", "Wide"], 0)] + \
          [(key, name, ["None", "A little", "Some", "Lots", "All"], 0)
           for key, name in (("k_bmov", "Bass Movement Step"), ("k_bsyn", "Bass Sync Step"), ("k_boct", "Bass Octaves Step"))] + \
          [("k_breg", "Bass Register Step", ["Lowest", "Low", "Natural", "High", "Highest"], 2),
           ("k_mrng", "Melody Range Step", ["Narrow", "Full"], 1), ("k_mlea", "Melody Leaps Step", ["Steps", "Leaps"], 0),
           ("k_msyn", "Melody Sync Step", ["On the beat", "Off the beat"], 0), ("k_mten", "Melody Tension Step", ["Inside", "Outside"], 0),
           ("k_mans", "Melody Answer Step", ["Echo", "Flip", "Shift"], 1), ("k_mreg", "Melody Register Step", ["Low", "Middle", "High"], 1)]
STEPPED_OLD = ("harmony", "ccol", "cspr", "bmov", "bsync", "boct", "breg", "mrng", "mlea", "msyn", "mten", "mans", "mreg")
PHRASING = [("mpf", "Melody Form", 4), ("mpb", "Melody Breath", 4), ("mpc", "Melody Cadence", 3), ("mpl", "Melody Fill", 2)]   # src/melody.c PH_NAMES
DYNAMIC = ("ccol", "cx_1", "cx_2", "cx_3", "swing", "gate", "dyn", "cgate", "cdyn", "cstrum", "cpush", "bpush", "bgate", "bacc", "mgat")
# 0.17 the CHORDS page's stepped knobs 2-5 (src/seq_core.h VK_*; the text is the engine's, dynamic_display): key, name, steps,
# the default step (Pop Piano's 0)
VKNOBS = [("k_vcol", "Colour", ["Power", "Triad", "7th", "9th", "11th", "13th"], 1),
          ("k_vinv", "Inversion", ["-2", "-1", "Root", "+1", "+2", "+3"], 2),
          ("k_vspr", "Spread", ["Cluster", "Close", "Open", "Split", "Wide"], 1),
          ("k_vstr", "Strum", ["Down %d" % i for i in range(8, 0, -1)] + ["Block"] + ["Up %d" % i for i in range(1, 9)], 8)]
# 0.17: what the CHORDS page replaces (IDEAS and SHAPE; their cells md_, ds_, fd_, ia_, pn_, mv_, vo_, oc_ too) still answers, unpolled
RETIRED_17 = ("itag", "lnote", "cnp_1", "cshtag", "k_harm", "k_col", "k_spr", "cstrum")
# 0.17 readouts whose text changes only with a tap (the wrapper's UpdateDisplay after it re-reads them): unpolled
QUIET_17 = ("ptl", "btl", "bvf_1", "bvf_2", "bvf_3", "bvf_4")


def chords_page_params():
    """0.17 (VST-15), appended after the 0.16 block in this order: the CHORDS page's view (engine-set), mode cells, knobs 2-5, the
    chord strip (tint=1 cells and their tints, engine-set), the left panel, the centre, the chord picker, the two browsers and the
    keyboard (src/engine.c TAPS and the 0.17 block of its header comment). The browser's DONE is bdo_1: bdn_1 is BASS FIX's"""
    # 0.18 appended Bar, Copy and Rhythms (src/seq_core.h CVIEW_BAR, CVIEW_COPY, CVIEW_RBROWSE; an option's position is its value)
    p = [{"key": "cview", "name": "Chords View", "options": ["Chords", "Pick", "Progressions", "Voicing", "Fix", "Rhythm", "Bar", "Copy",
                                                             "Rhythms", "Jam"], "default": 0}]   # 0.22 appended Jam (CVIEW_JAM)

    def strings(spec):   # (prefix, name, n): n cells prefix_1..n, or a readout `prefix` for n 0
        for prefix, name, n in spec:
            if not n:
                p.append({"key": prefix, "name": name, "display": "string"})
            p.extend({"key": "%s_%d" % (prefix, i), "name": "%s %d" % (name, i), "display": "string"} for i in range(1, n + 1))

    strings([("cvm", "Chords Page Mode", 3)])
    for key, name, opts, default in VKNOBS:
        p.append({"key": key, "name": name, "options": opts, "default": default, "qlink_ticks": 3, "dynamic_display": True})
    strings([("cst", "Chord Strip", 4)])
    p += [{"key": "tint_%d" % i, "name": "Chord Strip Tint %d" % i, "min": 0, "max": 5, "default": 5, "display": "int"} for i in range(1, 5)]
    strings([("cpg", "Strip Page", 2), ("cpage", "Strip Pages", 0), ("cund", "Chords Undo", 1), ("cbtp", "Back To Playing", 1),
             ("ccount", "Bar Count", 0), ("cfol", "Follow State", 0), ("cpr", "Progression", 3), ("cvo", "Voicing Style", 3),
             ("crk", "Reset Knobs", 1), ("ccn", "Centre Chord", 0), ("ccd", "Centre Degree", 0), ("cdr", "Centre Drift", 0),
             ("cct", "Centre Tap", 1)])
    p.append({"key": "cdrift", "name": "Centre Drift Level", "options": ["0", "1", "2", "3"], "default": 0})
    strings([("ptl", "Picker Title", 0), ("pty", "Picker Type", 5), ("plk", "Picker Lock", 1), ("pcn", "Picker Cancel", 1),
             ("pdn", "Picker Done", 1), ("bpr", "Progression Tile", 16), ("bgn", "Voicing Genre", 13), ("bvs", "Voicing Tile", 4),
             ("bvf", "Voicing Feel", 4), ("bcn", "Browser Cancel", 1), ("bdo", "Browser Done", 1), ("btl", "Browser Title", 0),
             ("kyw", "Keys White", 35), ("kyb", "Keys Black", 25), ("kye", "Keys Edge", 2)])
    # Task 9: the genre list's last row (5 a row; a list's cells are uniform and start at _1): two blank, inert cells
    p += [{"key": "bgn_%d" % i, "name": "Voicing Genre %d" % i, "display": "string"} for i in (14, 15)]
    for q in p:
        if q["key"] in QUIET_17:
            q["poll"] = False
    return p


# 0.18 what the RHYTHM page replaces (0.16's chord rhythm cells, NEW RHYTHM, 0.17's knobs 2-5) and NEW IDEA (dropped) still answer,
# unpolled; the slots k_s2..k_s5 take knobs 2-5's place
RETIRED_18 = ("cnr_1", "k_vcol", "k_vinv", "k_vspr", "k_vstr", "new_1")
# 0.18 readouts whose text changes only with a tap or a view change (display_rev): unpolled
QUIET_18 = ("rnote", "rmsg", "rfll", "rcnote", "rctl")


def rhythm_page_params():
    """0.18 the RHYTHM page, appended after the 0.17 block in this order: the 4 mode-following knobs (CHORDS COLOUR..STRUM, RHYTHM
    SYNC..FILL: src/engine.c KS_CHORDS; the text and the name MPC shows are the engine's), the step row, the paints, the action row, the RHYTHM field, the strip's
    rhythm pictures, COPY TO...'s bar cells and actions, the RHYTHMS browser's chips, tiles and each tile's picture and feel, then the
    readouts (src/engine.c TAPS and the 0.18 block of its header comment), then rview (R29) and rcp_ (R31)"""
    p = [{"key": "k_s%d" % (i + 2), "name": "Knob %d" % (i + 2), "options": opts, "default": default, "qlink_ticks": 3,
          "dynamic_display": True, "dynamic_name": True} for i, (key, name, opts, default) in enumerate(VKNOBS)]
    cells = [("rst", "Rhythm Step", 16), ("rpt", "Rhythm Paint", 5), ("rac", "Rhythm Action", 6), ("rfl", "Rhythm Field", 3),
             ("csr", "Strip Rhythm", 4), ("rcb", "Copy Bar", 16), ("rcx", "Copy Action", 4), ("rbg", "Rhythm Feel", 6),
             ("rbt", "Rhythm Tile", 4), ("rbp", "Rhythm Tile Picture", 4), ("rbf", "Rhythm Tile Feel", 4)]
    for prefix, name, n in cells:
        p += [{"key": "%s_%d" % (prefix, i), "name": "%s %d" % (name, i), "display": "string"} for i in range(1, n + 1)]
    for key, name in (("rtt", "Rhythm Title"), ("rnote", "Rhythm Note"), ("rmsg", "Rhythm Message"), ("rfll", "Rhythm Field Label"),
                      ("rcnote", "Copy Note"), ("rctl", "Copy Title")):
        p.append({"key": key, "name": name, "display": "string"})
    # R29, appended: engine-set, "Edit" on the Rhythm and Bar views, so the skin draws the two as one mode image ("-" elsewhere)
    p.append({"key": "rview", "name": "Rhythm Edit View", "options": ["-", "Edit"], "default": 0})
    # R31, appended: COPY TO...'s bars' chords and rhythm (cells inside the rcb_ cells; polled: lit marks a locked bar)
    p += [{"key": "rcp_%d" % i, "name": "Copy Bar Picture %d" % i, "display": "string"} for i in range(1, 17)]
    for q in p:
        if q["key"] in QUIET_18 or re.match(r"(rbp|rbf|rcx)_\d+$", q["key"]):   # tap-only texts (as bvf_), never lit
            q["poll"] = False
    return p


# 0.19 what the BASS page replaces (the BASS tab's IDEAS, SHAPE and FIX A NOTE) still answers, a no-op on a drawn Bass, unpolled
RETIRED_19 = ("bmode", "bitag", "bvw", "bfn", "k_bmov", "k_bsyn", "k_boct", "k_breg")
RETIRED_19_CELLS = r"(bmd|bds|bfd|bia|bpn|bmv|bsy|boc|bap|bst|bnk|bno|brs|bdn|bnb|bba)_\d+$"
# 0.19 readouts whose text changes only with a tap or a view change (display_rev): unpolled
QUIET_19 = ("uett", "uent", "umsg", "ubtl")
# 0.19 readouts the skin never places (ucw: uct_1 shows the same text), unpolled; the engine still answers them
UNPLACED_19 = ("ucw",)
# 0.19 the BASS tab's knobs 2-5 (src/engine.c KS_BASS; the text and the name MPC shows are the engine's): key, options, the BASS
# mode's default (Pop Root: MOVEMENT, OCTAVES, APPROACH None, REGISTER Natural). k_b2 has SYNC's 6 steps (RHYTHM mode)
BKNOBS = [("k_b2", ["-2", "-1", "0", "+1", "+2", "+3"], 0), ("k_b3", ["None", "A little", "Some", "Lots", "All"], 0),
          ("k_b4", ["None", "Scale step", "Half below", "Half above", "Surround"], 0),
          ("k_b5", ["Lowest", "Low", "Natural", "High", "Highest"], 2)]


def bass_page_params():
    """0.19 the BASS page, appended after the 0.18 block in this order: the BASS tab's view (engine-set, as cview), the 4
    mode-following knobs, the page's cells (mode, strip, left panel, centre, STYLE browser, bar editor) and its readouts
    (src/engine.c TAPS and the 0.19 block of its header comment)"""
    p = [{"key": "bview", "name": "Bass View", "options": ["Bass", "Edit", "Style", "Rhythm", "Bar", "Copy", "Rhythms", "Jam"],
          "default": 0}]   # 0.22 appended Jam (BVIEW_JAM)
    p += [{"key": key, "name": "Bass Knob %d" % (i + 2), "options": opts, "default": default, "qlink_ticks": 3,
           "dynamic_display": True, "dynamic_name": True} for i, (key, opts, default) in enumerate(BKNOBS)]
    def cells(spec):   # (prefix, name, first, last)
        for prefix, name, first, last in spec:
            p.extend({"key": "%s_%d" % (prefix, i), "name": "%s %d" % (name, i), "display": "string"} for i in range(first, last + 1))

    cells([("ubm", "Bass Page Mode", 1, 2), ("ust", "Bass Strip", 1, 4), ("usr", "Bass Strip Line", 1, 4), ("upg", "Bass Strip Page", 1, 2), ("und", "Bass Undo", 1, 1),
           ("ubtp", "Bass Back To Playing", 1, 1), ("usy", "Bass Style", 1, 3), ("upd", "Bass Pedal", 1, 3), ("uoc", "Bass Octave Cell", 1, 3),
           ("urk", "Bass Reset Knobs", 1, 1), ("uct", "Bass Centre Tap", 1, 1), ("ugn", "Bass Style Genre", 1, 12), ("uti", "Bass Style Tile", 1, 4),
           ("utp", "Bass Style Tile Picture", 1, 4), ("ubc", "Bass Style Cancel", 1, 1), ("ubd", "Bass Style Done", 1, 1),
           ("uea", "Bass Edit Action", 1, 5), ("ues", "Bass Edit Step", 1, 16), ("unk", "Bass Edit Note", 1, 12), ("uno", "Bass Edit Octave", 1, 4),
           ("uer", "Bass Edit Rhythm", 1, 1), ("ukw", "Bass Keys White", 1, 35), ("ukb", "Bass Keys Black", 1, 25), ("uke", "Bass Keys Edge", 1, 2)])
    for key, name in (("ucnt", "Bass Bar Count"), ("ufol", "Bass Follow State"), ("upage", "Bass Strip Pages"), ("ucw", "Bass Centre Line"),
                      ("ucn", "Bass Centre Notes"), ("uwh", "Bass Centre Bar"), ("utn", "Bass Centre Tension"), ("ustn", "Bass Style Name"),
                      ("ustg", "Bass Style Genre Name"), ("uett", "Bass Edit Title"), ("uent", "Bass Edit Note Line"),
                      ("umsg", "Bass Message"), ("ubtl", "Bass Style Title")):
        p.append({"key": key, "name": name, "display": "string"})
    for q in p:
        if q["key"] in QUIET_19 + UNPLACED_19 or re.match(r"utp_\d+$", q["key"]):   # tap-only texts (as rbp_), never lit
            q["poll"] = False
    return p


# 0.19 the BASS RHYTHM page's readouts whose text changes only with a tap or a view change (display_rev): unpolled, as QUIET_18
QUIET_19Y = ("ynote", "ymsg", "yfll", "ycnote", "yctl")


def bass_rhythm_params():
    """0.19 the BASS RHYTHM page (the 0.18 RHYTHM page's cells for the Bass lane, y for r), appended after the BASS page's block in
    this order: the step row, the paints, the action row, the RHYTHM field, the strip's rhythm pictures and bar cells, COPY TO...'s
    bars, their pictures and actions, the RHYTHMS browser's chips, tiles and each tile's picture and feel, then the readouts
    (src/engine.c TAPS and the 0.19 block of its header comment); then yview (Task 9) and the STYLE browser's uts_ (final review)"""
    p = []
    cells = [("yst", "Bass Rhythm Step", 16), ("ypt", "Bass Rhythm Paint", 5), ("yac", "Bass Rhythm Action", 6),
             ("yfl", "Bass Rhythm Field", 3), ("ysr", "Bass Strip Rhythm", 4), ("ysc", "Bass Rhythm Strip", 4),
             ("ycb", "Bass Copy Bar", 16), ("ycp", "Bass Copy Bar Picture", 16), ("ycx", "Bass Copy Action", 4),
             ("ybg", "Bass Rhythm Feel", 6), ("ybt", "Bass Rhythm Tile", 4), ("ybp", "Bass Rhythm Tile Picture", 4),
             ("ybf", "Bass Rhythm Tile Feel", 4)]
    for prefix, name, n in cells:
        p += [{"key": "%s_%d" % (prefix, i), "name": "%s %d" % (name, i), "display": "string"} for i in range(1, n + 1)]
    for key, name in (("ytt", "Bass Rhythm Title"), ("ynote", "Bass Rhythm Note"), ("ymsg", "Bass Rhythm Message"),
                      ("yfll", "Bass Rhythm Field Label"), ("ycnote", "Bass Copy Note"), ("yctl", "Bass Copy Title")):
        p.append({"key": key, "name": name, "display": "string"})
    # Task 9, appended: engine-set, the BASS tab's skin groups (as rview, R29): Bass on its BASS page's views (Bass, Edit, Style),
    # Edit on BASS RHYTHM's Rhythm and Bar (one picture for both), Other on its Copy and Rhythms
    p.append({"key": "yview", "name": "Bass Skin View", "options": ["Bass", "Edit", "Other", "Jam"], "default": 0})   # 0.22 appended Jam
    for q in p:
        if q["key"] in QUIET_19Y or re.match(r"(ybp|ybf|ycx)_\d+$", q["key"]):   # tap-only texts (as rbp_, rbf_, rcx_), never lit
            q["poll"] = False
    # final review, appended: the STYLE browser's tile summaries (src/engine.c uts_; tap-only texts, as utp_, never lit)
    p += [{"key": "uts_%d" % i, "name": "Bass Style Tile Summary %d" % i, "display": "string", "poll": False} for i in range(1, 5)]
    return p


# 0.20 what the MELODY JAM and PHRASE pages replace (RECIPE, IDEAS, SHAPE, FIX NOTE, the phrasing cells, FIX by bar) still answers, a
# no-op on a JAM Melody lane, unpolled; style_3 (spec: it stays retired) a no-op there too
RETIRED_20 = ("mmode", "mcap", "mbars", "mkst", "mitag", "mvw", "mphint", "mfn", "k_mrng", "k_mlea", "k_msyn", "k_mten", "k_mans", "k_mreg")
RETIRED_20_CELLS = r"(mmd|mty|mo1|mo2|mo3|mol|mph|mds|mfd|mia|mpn|mmv|mrk|mst|mnk|mno|mrt|mdn|mpf|mpb|mpc|mpl|mpr|mnb|mba)_\d+$"
RETIRED_20_ALSO = ("style_3",)
# 0.20 the JAM page's readouts whose text changes only with a tap or a block switch (display_rev): unpolled
QUIET_20 = ("jnow", "jnext", "jmsg")
# 0.20 the MELODY tab's knobs 2-5 (src/engine.c KS_MEL; the text and the name MPC shows are the engine's): key, options, the JAM
# mode's default. 0.21 (VST-28) they are JAM's DENSITY, RANGE, REGISTER, VARIATION and PHRASE's ANSWER, STRAY, OCTAVE, CADENCE; only the
# defaults change (params.json is positional: the options keep 0.20's names), each the option the JAM default reports through R4
# (DENSITY AS DRAWN 3 of 6, RANGE AS DRAWN 1, REGISTER Middle 2 of 4, VARIATION OFF 0). MKNOBS_21 the knobs 6-8, appended at the end
MKNOBS = [("k_j2", ["LOOP", "1+1", "2+2", "3+1", "AAB", "SENT"], 3), ("k_j3", ["None", "1/8", "Beat", "Half"], 1),
          ("k_j4", ["OFF", "OPEN", "SOFT", "HOME"], 2), ("k_j5", ["REST", "PICKUP", "STEP-IN", "RUN"], 0)]
# 0.21 the knobs the slots replace on the MELODY tab (PUSH, GATE, DYNAMICS): off the skin, unpolled; they still answer (a 0.20 project's
# Q-Link map turns them: jpush and mgat set what PUSH and GATE show, jdyn nothing)
UNPLACED_21 = ("jpush", "mgat", "jdyn")
# 0.21 knobs 6-8: JAM SWING (6), GATE (11 steps of 10), TIME (Amendment 1, ACCENT's place: 3 on 5) / PHRASE BREATH, FILL, PUSH (5 steps
# of 25); defaults SWING 50%, GATE 50, TIME NORMAL (R4: 2 of 5; only k_j8's default changed from ACCENT LIGHT's 1)
MKNOBS_21 = [("k_j6", ["50%", "54%", "58%", "62%", "66%", "70%"], 0), ("k_j7", ["%d" % (10 * i) for i in range(11)], 5),
             ("k_j8", ["0", "25", "50", "75", "100"], 2)]


def mknob_params(knobs, first):
    """The MELODY tab's mode-following knobs (MKNOBS, MKNOBS_21), Force knob `first` the first"""
    return [{"key": key, "name": "Melody Knob %d" % (i + first), "options": opts, "default": default, "qlink_ticks": 3,
             "dynamic_display": True, "dynamic_name": True} for i, (key, opts, default) in enumerate(knobs)]


def melody_page_params():
    """0.20 the MELODY tab's JAM page, appended after the 0.19 block (uts_4) in this order: the tab's view (engine-set, as bview) and
    its skin groups (engine-set, as yview), the 4 mode-following knobs, PUSH and DYNAMICS, the JAM cells (mode, blocks column by column,
    the phrase's bars, SWITCH AT), the KEYS, the JAM readouts, then the step dots (the strip's 4 bars and the centre bar: whole numbers
    0..72, the dot filmstrips' frames; reported on display_rev). src/engine.c TAPS and the 0.20 block of its header comment"""
    p = [{"key": "mview", "name": "Melody View", "options": ["Jam", "Phrase", "Pick", "Edit"], "default": 0},
         {"key": "mgroup", "name": "Melody Skin View", "options": ["Jam", "Phrase", "Edit"], "default": 0}]
    p += mknob_params(MKNOBS, 2)
    p += [{"key": key, "name": name, "min": 0, "max": 100, "default": default, "display": "int", "dynamic_display": True}
          for key, name, default in (("jpush", "Melody Push", 0), ("jdyn", "Melody Dynamics", 50))]
    for prefix, name, n in (("jmd", "Melody Page Mode", 2), ("jbk", "Melody Block", 30), ("jph", "Melody Phrase Bar", 4),
                            ("jsw", "Melody Switch At", 1), ("jkw", "Melody Keys White", 35), ("jkb", "Melody Keys Black", 25),
                            ("jke", "Melody Keys Edge", 2)):
        p += [{"key": "%s_%d" % (prefix, i), "name": "%s %d" % (name, i), "display": "string"} for i in range(1, n + 1)]
    for key, name in (("jnow", "Melody Now Playing"), ("jnext", "Melody Next"), ("jcnt", "Melody Bar Count"), ("jrole", "Melody Bar Role"),
                      ("jmsg", "Melody Message")):
        p.append({"key": key, "name": name, "display": "string"})
    for prefix, name, n in (("zs", "Melody Strip Dot", 64), ("zc", "Melody Centre Dot", 16)):
        p += [{"key": "%s_%d" % (prefix, i), "name": "%s %d" % (name, i), "min": 0, "max": 72, "default": 0, "display": "int"}
              for i in range(1, n + 1)]
    for q in p:
        if q["key"] in QUIET_20 or re.match(r"z[sc]_\d+$", q["key"]):
            q["poll"] = False
    return p


# 0.20 the PHRASE page's readouts whose text changes only with a tap, a knob turn or a part switch (display_rev): unpolled
QUIET_20W = ("wfmt", "want", "wsum", "wpnt", "wett", "went", "wmsg")


def phrase_page_params():
    """0.20 the MELODY tab's PHRASE page, appended after the JAM page's block (zc_16) in this order: the strip (its 4 bars, the page
    arrows, UNDO, BACK TO PLAYING and its readouts), the left panel (FORM, ANSWER, ANSWER PLAYS, the JAM knobs' summary), the centre,
    the ANSWER BLOCK picker and the bar editor (src/engine.c TAPS and the 0.20 block of its header comment)"""
    p = []

    def cells(spec):   # (prefix, name, n): n cells prefix_1..n, or a readout `prefix` for n 0
        for prefix, name, n in spec:
            if not n:
                p.append({"key": prefix, "name": name, "display": "string"})
            p.extend({"key": "%s_%d" % (prefix, i), "name": "%s %d" % (name, i), "display": "string"} for i in range(1, n + 1))

    cells([("wst", "Phrase Strip", 4), ("wpg", "Phrase Strip Page", 2), ("wund", "Phrase Undo", 1), ("wbtp", "Phrase Back To Playing", 1),
           ("wpage", "Phrase Strip Bars", 0), ("wpgn", "Phrase Strip Page Number", 0), ("wfol", "Phrase Follow State", 0),
           ("wfm", "Phrase Form", 6), ("wfmt", "Phrase Form Line", 0), ("wan", "Phrase Answer", 6), ("want", "Phrase Answer Line", 0),
           ("wsw", "Phrase Answer Plays", 3), ("wsum", "Phrase Jam Knobs", 0),
           ("wct", "Phrase Centre Tap", 1), ("wcwh", "Phrase Centre Bar", 0), ("wcrl", "Phrase Centre Role", 0), ("wcend", "Phrase Centre End", 0),
           ("wpk", "Phrase Pick Block", 30), ("wpc", "Phrase Pick Cancel", 1), ("wpd", "Phrase Pick Done", 1), ("wpnt", "Phrase Pick Note", 0),
           ("wea", "Phrase Edit Action", 5), ("wes", "Phrase Edit Step", 16), ("wnk", "Phrase Edit Note", 12), ("wno", "Phrase Edit Octave", 5),
           ("wrh", "Phrase Edit Rest Hold", 2), ("wett", "Phrase Edit Title", 0), ("went", "Phrase Edit Note Line", 0),
           ("wmsg", "Phrase Message", 0)])
    for q in p:
        if q["key"] in QUIET_20W:
            q["poll"] = False
    return p


# 0.22 (VST-29) the CHORDS and BASS JAM pages' readouts whose text changes only with a tap, a knob turn or a block switch (display_rev):
# unpolled, as QUIET_20
QUIET_22 = ("cjnow", "cjnext", "cjmsg", "ujnow", "ujnext", "ujmsg")
# 0.22 knobs 6 and 8 on the CHORDS and BASS tabs (src/engine.c KS_CHORDS, KS_BASS): own page and RHYTHM PUSH, DYNAMICS (5 steps of 25), JAM
# SWING (6), TIME (3 on 5). Defaults (R4): PUSH 0 = SWING 50% (0), DYNAMICS 50 = TIME NORMAL (2)
# 0.22 knobs 2-5's defaults on JAM (DENSITY 50 -> R4 3 of 6, COLOUR Triad 1, SPREAD as before, VARIATION OFF 0; BASS DENSITY 3, MOVEMENT
# None, OCTAVES None, VARIATION OFF): only the default fields change
JAM_DEFAULTS_22 = {"k_s2": 3, "k_s3": 1, "k_s5": 0, "k_b2": 3, "k_b5": 0}
KNOBS_22 = [("k_s6", "Chords Knob 6", ["50%", "54%", "58%", "62%", "66%", "70%"], 0), ("k_s8", "Chords Knob 8", ["0", "25", "50", "75", "100"], 2),
            ("k_b6", "Bass Knob 6", ["50%", "54%", "58%", "62%", "66%", "70%"], 0), ("k_b8", "Bass Knob 8", ["0", "25", "50", "75", "100"], 2)]


def jam_22_params():
    """0.22 (VST-29) the CHORDS and BASS JAM pages, appended after k_j8 in this order: knob 1's number slots (k_s1, k_b1: COMPLEXITY on the
    own page and RHYTHM, JAM's row 20 x level + 10; default 10, level 1, what a new instance on JAM reads) and the knob 6 and 8 slots
    (KNOBS_22), the mode cells JAM (cvm_4, ubm_3), then per lane (CHORDS cj, BASS uj) the JAM cells: the 30 blocks column by column, their
    meters (0 plain, 1 playing, 2 NEXT), the playing phrase's 4 bars, their meters (0 plain, 1 playing, 2 locked, 3 locked and playing),
    SWITCH AT, and the readouts NOW PLAYING, NEXT, the message line (src/engine.c TAPS and the 0.22 block of its header comment). The
    meters are whole numbers reported on display_rev (unpolled, as zs_)"""
    def slot(key, name, opts, default):
        return {"key": key, "name": name, "options": opts, "default": default, "qlink_ticks": 3, "dynamic_display": True, "dynamic_name": True}

    def number(key, name):
        return {"key": key, "name": name, "min": 0, "max": 100, "default": 10, "display": "int", "dynamic_display": True, "dynamic_name": True}

    k = {key: slot(key, name, opts, default) for key, name, opts, default in KNOBS_22}
    p = [number("k_s1", "Chords Knob 1"), k["k_s6"], k["k_s8"], number("k_b1", "Bass Knob 1"), k["k_b6"], k["k_b8"],
         {"key": "cvm_4", "name": "Chords Page Mode 4", "display": "string"}, {"key": "ubm_3", "name": "Bass Page Mode 3", "display": "string"}]
    for pre, lane in (("cj", "Chords"), ("uj", "Bass")):
        p += [{"key": "%sb_%d" % (pre, i), "name": "%s Jam Block %d" % (lane, i), "display": "string"} for i in range(1, 31)]
        p += [{"key": "%sm_%d" % (pre, i), "name": "%s Jam Block Meter %d" % (lane, i), "min": 0, "max": 2, "default": 0, "display": "int"}
              for i in range(1, 31)]
        p += [{"key": "%sp_%d" % (pre, i), "name": "%s Jam Bar %d" % (lane, i), "display": "string"} for i in range(1, 5)]
        p += [{"key": "%sq_%d" % (pre, i), "name": "%s Jam Bar Meter %d" % (lane, i), "min": 0, "max": 3, "default": 0, "display": "int"}
              for i in range(1, 5)]
        p.append({"key": pre + "sw_1", "name": lane + " Jam Switch At", "display": "string"})
        p += [{"key": pre + key, "name": "%s Jam %s" % (lane, name), "display": "string"}
              for key, name in (("now", "Now Playing"), ("next", "Next"), ("msg", "Message"))]
    for q in p:
        if q["key"] in QUIET_22 or re.match(r"[cu]j[mq]_\d+$", q["key"]):
            q["poll"] = False
    return p


# 0.24 (VST-31) the three-page model: knob 7's slots (src/engine.c KS_CHORDS, KS_BASS: GATE in 11 steps of 10 on JAM and the CHORDS / BASS
# page, FILL on PHRASE; default GATE 50, what a new instance on JAM reads), the sound pages' readouts whose text changes once a bar or with
# a tap (display_rev): unpolled, as QUIET_22, and their bars' chord lines (display-only cells, made once a bar: unpolled, as zs_)
KNOBS_24 = [("k_s7", "Chords Knob 7"), ("k_b7", "Bass Knob 7")]
QUIET_24 = ("csnow", "cswh", "csnt", "cswd", "usnow", "uswh", "usnt", "uswd")
# 0.24 what PHRASE and the sound pages replace still answers, unpolled: the CHORDS tab's FIX mode cell and NAMES (on GLOBAL now), its strip,
# paging, old centre texts, chord picker and FIX cells; the BASS tab's strip, paging, old centre texts and bar editor (EDIT THIS BAR'S RHYTHM
# too). cct_1 and uct_1 stay: the new centre's taps
RETIRED_24 = ("cvm_3", "cnm_1", "cpage", "cfixnote", "ccn", "ccd", "cdr", "upage", "uwh", "ucn", "utn", "cfview")   # Task 8: FIX's stepper
RETIRED_24_CELLS = r"(cst|tint|cpg|cund|cbtp|pty|plk|pcn|pdn|fc|fb|fo|fbs|fbar|dn|ust|usr|upg|und|ubtp|uea|ues|unk|uno|uer)_\d+$"
# 0.24 the retired cells that acted: a set does nothing (KF_RETIRED_24; cvm_3 is in its family's flags, set_mode leaves it; cnm_1 still sets
# NAMES); FIX's 0.12 slots (fs_, fsl_) too; after 0.24 IDEAS's cells on the three tabs (0.17/0.19/0.20 off the skin): IDEAS left the code
# 0.24.1 FIX left the code: BASS FIX A NOTE's and MELODY FIX NOTE's cells and their FIX by bar strip and NEW BAR / RESET BAR (0.19/0.20 off the skin).
# 0.24.1 every other cell family off the skin whose taps acted: 0.9's mode cells and SHAPE (md_ rh_ vo_ oc_, 0.12's crh_ cnp_ cnr_), the BASS tab's
# mode and SHAPE (bmd_ bsy_ boc_ bap_), MELODY's RECIPE, SHAPE and phrasing (mmd_ mty_ mo1_..mo3_ mol_ mph_ mrk_ mpf_ mpb_ mpc_ mpl_ mpr_), the
# 0.18 / 0.19 RHYTHM pages' cells PHRASE's own replace (RETIRED_24P_CELLS, rac_ rcx_ yac_ ycx_ too), usr_; and FIX's bar stepper cfview.
# new_1, gl_ (the parity scenario taps them) and cnm_1 (NAMES) still act
RETIRED_24_TAPS = (r"((cst|cpg|cund|cbtp|pty|plk|pcn|pdn|fc|fb|fo|fs|fsl|fbs|fbar|dn|ust|upg|und|ubtp|uea|ues|unk|uno|uer|[bm]?(ds|fd|ia|pn|mv)|"
                   r"bst|bnk|bno|brs|bdn|bnb|bba|mst|mnk|mno|mrt|mdn|mnb|mba|md|rh|vo|oc|crh|cnp|cnr|bmd|bsy|boc|bap|mmd|mty|mo[123]|mol|mph|mrk|"
                   r"mp[fbclr]|[ry](st|pt|ac|fl|cb|cp|cx)|csr|ysr|ysc|usr)_\d+|cfview)$")


def phrase_24_params():
    """0.24 (VST-31), appended after the 0.22 JAM block in this order: knob 7's slots (KNOBS_24), the CHORDS page's part OCTAVE cells
    (cpo_1..3: -1, 0, +1), GLOBAL's NAMES pair (gnm_1..2: DEGREES, NAMES), then per sound page (CHORDS cs, BASS us) the status row: NOW
    PLAYING, the playing phrase's 4 bar cells ("BAR 6") and their chord lines ("VI · C"), EDIT BARS; and the centre's readouts: where
    ("PLAYING · BAR 6 · tap to edit on PHRASE"), its notes, its word line (src/engine.c TAPS and the 0.24 block of its header
    comment). The centre's name is cct_1's / uct_1's text"""
    p = [{"key": key, "name": name, "options": ["%d" % (10 * i) for i in range(11)], "default": 5, "qlink_ticks": 3,
          "dynamic_display": True, "dynamic_name": True} for key, name in KNOBS_24]

    def cells(prefix, name, n):
        return [{"key": "%s_%d" % (prefix, i), "name": "%s %d" % (name, i), "display": "string"} for i in range(1, n + 1)]

    p += cells("cpo", "Chords Octave Cell", 3) + cells("gnm", "Chord Names", 2)
    for pre, lane in (("cs", "Chords"), ("us", "Bass")):
        p.append({"key": pre + "now", "name": lane + " Now Playing", "display": "string"})
        p += cells(pre + "b", lane + " Status Bar", 4) + cells(pre + "c", lane + " Status Bar Chord", 4) + cells(pre + "e", lane + " Edit Bars", 1)
        p += [{"key": pre + key, "name": "%s Centre %s" % (lane, name), "display": "string"}
              for key, name in (("wh", "Where"), ("nt", "Notes"), ("wd", "Word"))]
    for q in p:
        if q["key"] in QUIET_24 or re.match(r"[cu]sc_\d+$", q["key"]):
            q["poll"] = False
    return p


# 0.24 (Task 7) the PHRASE page of CHORDS (cq...) and BASS (uq...), one family each per cell (src/engine.c ph_*): (key after the lane's
# prefix, name, cells; 0 a readout). The strip (4 bars and their chord lines, the page arrows, UNDO, BACK TO PLAYING, the page readouts), the
# left panel (RHYTHM, LOCK WITH, ANTICIPATE and their lines), the centre (its tap, where, chords, end, the legend, the message line), the bar
# editor (title, actions, paint row, steps and their third lines, the picker: CHORDS chords, borrows, TYPE and THIS HIT ONLY, OCTAVE; BASS
# the piano, OCTAVE, FOLLOW CHORD), COPY TO... (bars, their inner lines, its actions and readouts), then the ints: the step marks and the dots
PHRASE_24 = {
    "cq": [("s", "Strip", 4), ("h", "Strip Chords", 4), ("g", "Strip Page", 2), ("u", "Undo", 1), ("b", "Back To Playing", 1),
           ("page", "Strip Bars", 0), ("pgn", "Strip Page Number", 0), ("f", "Rhythm", 3), ("fl", "Rhythm Line", 0), ("w", "Lock With", 3),
           ("wl", "Lock With Line", 0), ("a", "Anticipate", 4), ("al", "Anticipate Line", 0), ("t", "Centre Tap", 1), ("cw", "Centre Bar", 0),
           ("cc", "Centre Chords", 0), ("ce", "Centre End", 0), ("lg", "Legend", 0), ("msg", "Message", 0), ("tt", "Edit Title", 0),
           ("e", "Edit Action", 5), ("p", "Edit Paint", 7), ("n", "Edit Step", 16), ("l", "Edit Step Line", 16), ("k", "Edit Chord", 7),
           ("r", "Edit Borrow", 3), ("y", "Edit Type", 2), ("o", "Edit Octave", 5), ("c", "Copy Bar", 16), ("i", "Copy Bar Line", 16),
           ("x", "Copy Action", 6), ("ctl", "Copy Title", 0), ("cn", "Copy Note", 0)],
    "uq": [("s", "Strip", 4), ("h", "Strip Chords", 4), ("g", "Strip Page", 2), ("u", "Undo", 1), ("b", "Back To Playing", 1),
           ("page", "Strip Bars", 0), ("pgn", "Strip Page Number", 0), ("f", "Rhythm", 3), ("fl", "Rhythm Line", 0), ("w", "Lock With", 3),
           ("wl", "Lock With Line", 0), ("a", "Anticipate", 4), ("al", "Anticipate Line", 0), ("t", "Centre Tap", 1), ("cw", "Centre Bar", 0),
           ("cc", "Centre Chords", 0), ("ce", "Centre End", 0), ("lg", "Legend", 0), ("msg", "Message", 0), ("tt", "Edit Title", 0),
           ("e", "Edit Action", 5), ("p", "Edit Paint", 7), ("n", "Edit Step", 16), ("l", "Edit Step Line", 16), ("k", "Edit Note", 12),
           ("o", "Edit Octave", 4), ("y", "Edit Follow Chord", 1), ("c", "Copy Bar", 16), ("i", "Copy Bar Line", 16), ("x", "Copy Action", 6),
           ("ctl", "Copy Title", 0), ("cn", "Copy Note", 0)]}
# 0.24 the dots' frames (src/engine.c chords_frame, bass_frame): CHORDS 36 (rest, 3 heights x 5 drift colours, 5 holds, 15 ticked hits), BASS
# 133 (MELODY's 73 over E1..E4, then 60 ticked hits); the step marks 0..3 (bit 0 a chord starts there, bit 1 a hit the knobs add)
PHRASE_24_DOTS = {"cq": 35, "uq": 132}
# 0.24 PHRASE's texts that change with a tap, a knob turn, a part switch or once a bar (the engine's cache, display_rev): unpolled; the
# display-only cells too (the strip's chord lines, the steps' third lines, COPY TO...'s inner lines, as csc_) and the cells that never light
# (the page arrows, UNDO, BACK TO PLAYING, the centre's tap, the editor's actions, TYPE / THIS HIT ONLY / FOLLOW CHORD, COPY TO...'s actions:
# as rcx_). Polled: the cells that light (the strip, LOCK WITH, ANTICIPATE, the paints, the steps, the picker, COPY TO...'s bars) and the
# RHYTHM field's image buttons (as rfl_). (0.24 release fix, device test) MPC reads a never-lit tile's 0 back and sends 1 at every touch; the
# wrapper's list-tile echo guard (vst2_wrap.c set_param_now) would drop the repeats of an unpolled one, so vst.json sets
# ECHO_GUARD_POLLED_ONLY (local toolchain branch chordseq/on-no-update), which skips the guard for unpolled cells
QUIET_24P = ("page", "pgn", "fl", "wl", "al", "cw", "cc", "ce", "lg", "msg", "tt", "ctl", "cn")
UNLIT_24P = r"([hilmgubtyxe]|z[sc])_\d+$"


# 0.24 (Task 7) the 0.18 / 0.19 RHYTHM pages' cells PHRASE's own replace (its step row, paints, field, pictures, BASS RHYTHM's strip, COPY TO...'s
# bars and the titles): unpolled, they still answer (the RHYTHMS browser's stay: PHRASE opens it; the action rows rac_, yac_ stay
# polled); 0.24.1 a set of any does nothing (RETIRED_24_TAPS)
RETIRED_24P = ("rtt", "ytt")
RETIRED_24P_CELLS = r"(rst|rpt|rfl|csr|rcb|rcp|yst|ypt|yfl|ysr|ysc|ycb|ycp)_\d+$"


def phrase_cells_24_params():
    """0.24 (Task 7, VST-31) the PHRASE pages of CHORDS and BASS, appended after the sound pages' block (uswd) in this order: per lane (CHORDS
    cq..., BASS uq...) the PHRASE_24 families, the 16 step marks (Pm_1..16) and the dots (Pzs_1..64 the strip page's 4 bars, Pzc_1..16 the
    centre bar's, the open bar's while editing: the editor shares them, lever 1)"""
    p = []
    for pre, lane in (("cq", "Chords"), ("uq", "Bass")):
        for key, name, n in PHRASE_24[pre]:
            full = "%s Phrase %s" % (lane, name)
            if not n:
                p.append({"key": pre + key, "name": full, "display": "string"})
            p += [{"key": "%s%s_%d" % (pre, key, i), "name": "%s %d" % (full, i), "display": "string"} for i in range(1, n + 1)]
        p += [{"key": "%sm_%d" % (pre, i), "name": "%s Phrase Step Mark %d" % (lane, i), "min": 0, "max": 3, "default": 0, "display": "int"}
              for i in range(1, 17)]
        for k, name, n in (("zs", "Strip Dot", 64), ("zc", "Centre Dot", 16)):
            p += [{"key": "%s%s_%d" % (pre, k, i), "name": "%s Phrase %s %d" % (lane, name, i), "min": 0, "max": PHRASE_24_DOTS[pre],
                   "default": 0, "display": "int"} for i in range(1, n + 1)]
    for q in p:
        k = q["key"][2:]
        if k in QUIET_24P or re.match(UNLIT_24P, k):
            q["poll"] = False
    return p


def sound_dots_24_params():
    """0.24 (Task 8 fix, VST-31), appended after the PHRASE pages' block (uqzc_16): the BASS sound page's centre bar's 16 dots (src/engine.c
    sound_cells_now: bass_frame's frames 0..132, as uqzc_; reported on display_rev, unpolled as zs_)"""
    return [{"key": "usz_%d" % i, "name": "Bass Centre Dot %d" % i, "min": 0, "max": PHRASE_24_DOTS["uq"], "default": 0, "display": "int",
             "poll": False} for i in range(1, 17)]


# 0.19.1 vst/build/keyflags.h (src/engine.c parse_key, txn_key, set_param). A set that acts closes an open picker, browser, RHYTHM bar,
# STYLE browser or bar editor as DONE first (ruling 24) unless its key keeps the open one's tab's (the CHORDS tab's while none is open).
# A tab keeps its pages' keys (the CHORDS page's cells and options: 0.17 left out its readouts and tints, which never act) and these
# (a name ending in "_" is a cell family). Both tabs: the part cells (pt_'s switch cancels in the core), the chunk, INIT and ALL NOTES
# OFF (no edit), and each tab's KEYS (display only)
KEEP_BOTH = ("pt_", "state", "init", "panic", "kyw_", "kyb_", "kye_", "ukw_", "ukb_", "uke_", "jkw_", "jkb_", "jke_")   # 0.20: every tab's
# the CHORDS tab: FIX's chord cells, NAMES, the ring's knobs 1 and 6-8, 0.9's mode cells and cmode (they change the view, which
# cancels) and FIX's engine-set layout params
KEEP_CHORDS = ("fc_", "fb_", "cnm_", "cx_1", "cpush", "cgate", "cdyn", "md_", "cmode", "cslots", "cbar")
# the BASS tab: its knobs 1 and 6-8, and 0.13's stepped Bass knobs (retired)
KEEP_BASS = ("cx_2", "bpush", "bgate", "bacc", "k_bmov", "k_bsyn", "k_boct", "k_breg")
# 0.22 the CHORDS and BASS JAM pages: their knob slots, the mode cells and every cj... / uj... key (JAM_CELLS_22)
KEEP_CHORDS += ("k_s1", "k_s6", "k_s8", "cvm_", "cjb_", "cjm_", "cjp_", "cjq_", "cjsw_", "cjnow", "cjnext", "cjmsg")
KEEP_BASS += ("k_b1", "k_b6", "k_b8", "ubm_", "ujb_", "ujm_", "ujp_", "ujq_", "ujsw_", "ujnow", "ujnext", "ujmsg")
# 0.24 knob 7's slots, the CHORDS part OCTAVE cells and every cs... / us... key of the sound pages' status row and centre (phrase_24_params);
# GLOBAL's NAMES pair is display only, so it keeps every tab's (KEEP_BOTH)
KEEP_CHORDS += ("k_s7", "cpo_", "csnow", "csb_", "csc_", "cse_", "cswh", "csnt", "cswd")
KEEP_BASS += ("k_b7", "usnow", "usb_", "usc_", "use_", "uswh", "usnt", "uswd", "usz_")   # Task 8 fix: usz_ the centre's dots
KEEP_BOTH += ("gnm_",)
# 0.24 (Task 7) every cq... key is the CHORDS PHRASE page's, every uq... key the BASS PHRASE page's (PHRASE_24)
CHORDS_PHRASE_24, BASS_PHRASE_24 = r"cq", r"uq"
# 0.19 a set does nothing on a DRAWN Bass lane: RETIRED_19's keys that act (its readouts never do) and cells, and the older Bass
# controls the BASS page replaces too (0.5's Bass style, 0.10's SHAPE knobs, unpolled since 0.13)
RETIRED_19_ALSO = ("style_2", "bmov", "bsync", "boct", "breg")
# 0.20 the MELODY tab: every j..., w..., z... and k_j... key (its pages' cells, knobs, readouts and dots), its knobs 1 and 7 and its
# engine-set views; a set of RETIRED_20's keys that act (its cells, RETIRED_20_ALSO) does nothing on a JAM Melody lane
KEEP_MEL = ("cx_3", "mgat", "mview", "mgroup")
MEL_FAMILIES = r"([jwz]|k_j)"
KF_NAMES = (("KF_KEEP_CHORDS", 1), ("KF_KEEP_BASS", 2), ("KF_RETIRED_BASS", 4), ("KF_KEEP_MEL", 8), ("KF_RETIRED_MEL", 16),
            ("KF_RETIRED_24", 32))   # 0.24 RETIRED_24_TAPS


def key_flags(params):
    """0.19.1 {key: flags} for every param key with any (and "state", the chunk): KF_NAMES' bits"""
    def acts(q):   # a cell (a numbered string) or an option; a readout's set or a tint's never acts
        return "options" in q or (q.get("display") == "string" and re.search(r"_\d+$", q["key"]) is not None)

    def named(names, key):
        return any(key == n or (n.endswith("_") and re.match(re.escape(n) + r"\d+$", key)) for n in names)

    chords = {q["key"] for q in chords_page_params() if acts(q)} | {q["key"] for q in rhythm_page_params()}
    chords |= {q["key"] for q in params if re.match(CHORDS_PHRASE_24, q["key"])}   # 0.24 Task 7
    bass = {q["key"] for q in bass_page_params() + bass_rhythm_params()}
    bass |= {q["key"] for q in params if re.match(BASS_PHRASE_24, q["key"])}   # 0.24 Task 7
    retired = {q["key"] for q in params if (q["key"] in RETIRED_19 and acts(q)) or re.match(RETIRED_19_CELLS, q["key"])}
    mel = {q["key"] for q in params if re.match(MEL_FAMILIES, q["key"])}
    mretired = {q["key"] for q in params if (q["key"] in RETIRED_20 and acts(q)) or re.match(RETIRED_20_CELLS, q["key"])}
    keys = [q["key"] for q in params]
    keys += [n for n in KEEP_BOTH + KEEP_CHORDS + KEEP_BASS + RETIRED_19_ALSO + KEEP_MEL if not n.endswith("_") and n not in keys]
    out = collections.OrderedDict()
    for k in keys:
        f = (1 if k in chords or named(KEEP_BOTH + KEEP_CHORDS, k) else 0) | (2 if k in bass or named(KEEP_BOTH + KEEP_BASS, k) else 0)
        f |= 8 if k in mel or named(KEEP_BOTH + KEEP_MEL, k) else 0
        f |= 16 if k in mretired or named(RETIRED_20_ALSO, k) else 0
        f |= 32 if re.match(RETIRED_24_TAPS, k) else 0
        out[k] = f | (4 if k in retired or named(RETIRED_19_ALSO, k) else 0)
    return out


def write_keyflags(params):
    """0.19.1 vst/build/keyflags.h: key_flags as one table sorted by strcmp, a numbered cell family as its prefix ("cst_") when its
    cells agree (src/engine.c reads a TAPS entry's flags once), any other key whole"""
    flags, cells = key_flags(params), {q["key"] for q in params if q.get("display") == "string"}
    family = collections.OrderedDict()
    for k in flags:
        m = re.match(r"(.+_)\d+$", k)
        family.setdefault(m.group(1) if m else k, []).append(k)
    rows = []
    for name, ks in family.items():
        fs = {flags[k] for k in ks}
        if len(fs) > 1 and cells & set(ks):
            raise SystemExit("keyflags: the cells %s* differ" % name)
        rows += [(name, fs.pop())] if len(fs) == 1 and name.endswith("_") else [(k, flags[k]) for k in ks]
    rows = sorted((k, f) for k, f in rows if f)
    if any(re.match(r"(l\d+|ph|chord|mute)_$", k) for k, _ in rows):   # parse_key reads no flags for these kinds
        raise SystemExit("keyflags: the grid, playhead, chord and mute keys keep nothing")
    o = ["/* vst/build/keyflags.h: generated by vst/make_skin.py (KEEP_BOTH ..., key_flags), do not edit. Each key's flags for",
         " * src/engine.c (parse_key, txn_key, set_param), sorted by strcmp: an entry ending in '_' is a cell family (its keys end in the",
         " * cell's number), any other a whole key; a key with no entry has none */",
         "#pragma once",
         "#include <string.h>",
         "#define KEYFLAGS_NPARAMS %d   /* the param count it was made for: params.h's NPARAMS (a stale header fails to build) */"
         % len(params),
         "enum { %s };" % ", ".join("%s = %d" % nv for nv in KF_NAMES),
         "typedef struct { const char *prefix; unsigned char flags; } keyflag_t;",
         "static const keyflag_t KEYFLAGS[] = {"]
    o += ['    {"%s", %s},' % (k, " | ".join(n for n, v in KF_NAMES if f & v)) for k, f in rows]
    o += ["};",
          "/* The entry for key's first n characters (a whole key, or a cell family's prefix), 0 none: a binary search */",
          "static inline int keyflags_find(const char *key, size_t n) {",
          "    size_t lo = 0, hi = sizeof KEYFLAGS / sizeof KEYFLAGS[0];",
          "    while (lo < hi) {",
          "        size_t mid = lo + (hi - lo) / 2;",
          "        int c = strncmp(KEYFLAGS[mid].prefix, key, n);",
          "        if (!c && KEYFLAGS[mid].prefix[n]) c = 1;   /* longer: after key's n characters */",
          "        if (!c) return KEYFLAGS[mid].flags;",
          "        if (c < 0) lo = mid + 1;",
          "        else hi = mid;",
          "    }",
          "    return 0;",
          "}",
          "/* A key's flags: its own entry, else its cell family's (\"cst_\" for \"cst_3\") */",
          "static inline int keyflags_of(const char *key) {",
          "    size_t n = strlen(key), d = n;",
          "    int f = keyflags_find(key, n);",
          "    while (d > 0 && key[d - 1] >= '0' && key[d - 1] <= '9') d--;",
          "    return f || d == n || d < 2 || key[d - 1] != '_' ? f : keyflags_find(key, d);",
          "}",
          ""]
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    open(os.path.join(HERE, "build", "keyflags.h"), "w").write("\n".join(o))
    return len(rows)


def make_params():
    p = [{"key": "pos", "name": "Position", "display": "string"}]
    # chord tiles 1..4 are 0.1 params; tiles 5..8 were appended in 0.4 (below), so keep the two loops separate: positions are saved
    p += [{"key": "chord_%d" % b, "name": "Chord %d" % b, "display": "string"} for b in range(1, 5)]
    p += [{"key": "ph_%d" % s, "name": "Playhead %d" % s, "display": "string"} for s in range(1, STEPS + 1)]
    for n, name in enumerate(PARAM_LANES, 1):
        p += [{"key": "l%d_%d" % (n, s), "name": "%s %d" % (name.title(), s), "display": "string"} for s in range(1, STEPS + 1)]
    p += [{"key": "mute_%d" % n, "name": "Mute", "options": ["Off", "On"], "default": 0} for n in range(1, len(PARAM_LANES) + 1)]
    for q in p:   # 0.7 retired Melody 2 (lane 4): its cells stay (positions are saved) but are named so and not polled
        if q["key"].startswith("l4_"):
            q.update({"name": UNUSED + q["name"], "poll": False})
    p[-1]["name"] = UNUSED + "Mute"
    p.append({"key": "panic", "name": "All Notes Off", "momentary": True})
    # 0.3, appended (saved projects store values by position)
    p.append({"key": "length", "name": "Length", "options": ["1 Bar", "2 Bars", "4 Bars", "8 Bars", "16 Bars"], "default": 2})
    p.append({"key": "chord_len", "name": "Chord", "options": ["2 Bars", "1 Bar", "1/2 Bar", "1/4 Bar"], "default": 1})
    p.append({"key": "view", "name": "View", "options": ["Bar %d" % b for b in range(1, 17)], "default": 0})
    # 0.4, appended
    p += [{"key": "chord_%d" % b, "name": "Chord %d" % b, "display": "string"} for b in range(5, CHORD_TILES + 1)]
    p.append({"key": "title", "name": "Title", "display": "string"})
    p.append({"key": "key", "name": "Key", "options": ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"], "default": 4})
    p.append({"key": "scale", "name": "Scale", "options": ["Major", "Minor", "Dorian", "Mixolydian", "Phrygian", "Lydian", "Harm. minor"],
             "default": 1})   # 0.7 appended the last three (an option's position is its saved value)
    p.append({"key": "prog", "name": "Progression", "options": PROGRESSIONS + ["Custom"], "default": 0})
    p.append({"key": "voicing", "name": "Voicing", "options": ["Close", "Smooth"], "default": 1})
    # 0.5, appended: stepper arrows (the wrapper nudges the target, no engine code), then the generator controls.
    # KEY first: the toolchain's host test (tools/host_test.c) checks only the first prev/next pair, expecting every option
    # of the target to be settable, and prog's last option (Custom) is never set
    for target, name in (("key", "Key"), ("prog", "Progression"), ("view", "View")):
        p.append({"key": target + "_prev", "name": name + " Prev", "momentary": True, "step_of": target, "step_delta": -1})
        p.append({"key": target + "_next", "name": name + " Next", "momentary": True, "step_of": target, "step_delta": 1})
    # 0.12 appended the last six chord rhythms (an option's position is its saved value)
    p.append({"key": "style_1", "name": "Chords Style", "options": ["Manual", "Sustain", "Pulse", "Stabs", "Offbeat", "Charleston",
                                                                    "Ballad", "Funk", "Half-time", "Push"], "default": 2})
    p.append({"key": "style_2", "name": "Bass Style", "options": ["Manual", "Root", "Pump", "Walk"], "default": 1})
    p += [{"key": "cx_%d" % n, "name": "%s Complexity" % PARAM_LANES[n - 1].title(), "min": 0, "max": 100, "default": 30, "display": "int"}
          for n in (1, 2)]
    p += [{"key": "gen_%d" % n, "name": "%s Gen" % PARAM_LANES[n - 1].title(), "momentary": True} for n in (1, 2)]
    p.append({"key": "harmony", "name": "Harmony", "min": 0, "max": 100, "default": 30, "display": "int"})
    p.append({"key": "gen_prog", "name": "Gen Progression", "momentary": True})
    p.append({"key": "undo", "name": "Undo", "momentary": True})
    # 0.6, appended: the Melody lanes' generator controls
    # (0.7 renamed them: Melody 1 is "Melody", Melody 2 is retired)
    p.append({"key": "style_3", "name": "Melody Style", "options": ["Manual", "Hook", "Arp"], "default": 1})
    p.append({"key": "style_4", "name": UNUSED + "Melody 2 Style", "options": ["Manual", "Answer", "Arp"], "default": 1})
    p += [{"key": "cx_%d" % n, "name": name + " Complexity", "min": 0, "max": 100, "default": 30, "display": "int"}
          for n, name in ((3, "Melody"), (4, UNUSED + "Melody 2"))]
    p += [{"key": "gen_%d" % n, "name": name + " Gen", "momentary": True} for n, name in ((3, "Melody"), (4, UNUSED + "Melody 2"))]
    # 0.7 (+62), appended: lane channels, the output stage, readouts, then the tap cells ("display": "string" list cells the
    # engine treats as selectors, toggles and actions: src/engine.c TAPS)
    lanes3 = ("Chords", "Bass", "Melody")
    p += [{"key": "ch_%d" % n, "name": "%s Channel" % lanes3[n - 1], "options": [str(c) for c in range(1, 17)], "default": n - 1}
          for n in (1, 2, 3)]
    for n in (1, 2, 3):
        p.append({"key": "ch_%d_prev" % n, "name": "%s Ch Prev" % lanes3[n - 1], "momentary": True, "step_of": "ch_%d" % n, "step_delta": -1})
        p.append({"key": "ch_%d_next" % n, "name": "%s Ch Next" % lanes3[n - 1], "momentary": True, "step_of": "ch_%d" % n, "step_delta": 1})
    for key, name, default in (("swing", "Swing", 0), ("gate", "Gate", 50), ("dyn", "Dynamics", 50)):
        p.append({"key": key, "name": name, "min": 0, "max": 100, "default": default, "display": "int"})
    p.append({"key": "bpm", "name": "Tempo", "display": "string"})
    p.append({"key": "vchord", "name": "Bar Chord", "display": "string"})
    cells = [("mt", "Mute Tap", 3), ("bar", "Bar", 8), ("fol", "Follow", 1), ("act", "Action", 2), ("kw", "White Key", 7),
             ("kb", "Black Key", 5), ("sc", "Scale Tile", 8), ("ln", "Length Tile", 5), ("cl", "Chord Tile", 4), ("new", "New Idea", 1),
             ("gl", "Lane Gen", 4)]
    for prefix, name, n in cells:
        p += [{"key": "%s_%d" % (prefix, i), "name": "%s %d" % (name, i), "display": "string"} for i in range(1, n + 1)]
    for q in p:   # kb_3..5 (F#, G#, A#) are kb2_1..3 on the skin; gl_ only captions the GEN image buttons (static text)
        if q["key"] in ("kb_3", "kb_4", "kb_5") or q["key"].startswith("gl_"):
            q["poll"] = False
    # 0.7 (+3), appended: F#, G#, A# as their own list, so the skin can place them over F-G, G-A, A-B (a list starts at _1)
    p += [{"key": "kb2_%d" % i, "name": "Black Key %d" % (i + 2), "display": "string"} for i in range(1, 4)]
    # 0.8, appended: the part cells (screen order Intro, Verse, Pre, Chorus, Bridge, Outro), then the playing part's name
    p += [{"key": "pt_%d" % i, "name": "Part %d" % i, "display": "string"} for i in range(1, 7)]
    p.append({"key": "part", "name": "Part", "display": "string"})
    # 0.9, appended: the CHORDS tab's mode (when= panels hang on it) and its three tap cells
    p.append({"key": "cmode", "name": "Chords Mode", "options": ["Ideas", "Shape", "Fix", "Jam"], "default": 0})   # 0.22 appended Jam
    p += [{"key": "md_%d" % i, "name": "Chords Mode %d" % i, "display": "string"} for i in range(1, 4)]
    # 0.9 (+65), appended: the CHORDS tab's IDEAS, FIX and SHAPE cells and readouts (src/engine.c TAPS), then the SHAPE knobs
    chords = [("ds", "Idea Distance", 3), ("fd", "Idea Field", 9), ("ia", "Idea Action", 3), ("pn", "Idea Pin", 4), ("mv", "Chord Move", 6),
              ("itag", "Idea Tag", 0), ("lnote", "Chord Length Note", 0), ("fs", "Fix Slot", 8), ("fc", "Fix Chord", 7),
              ("fb", "Fix Borrow", 3), ("fo", "Fix Colour", 5), ("dn", "Fix Done", 1), ("rh", "Chords Rhythm", 3),
              ("vo", "Chords Voicing", 2), ("oc", "Chords Octave", 3)]
    for prefix, name, n in chords:
        if not n:   # a readout
            p.append({"key": prefix, "name": name, "display": "string"})
        p += [{"key": "%s_%d" % (prefix, i), "name": "%s %d" % (name, i), "display": "string"} for i in range(1, n + 1)]
    for key, name, default in (("ccol", "Chords Colour", 0), ("cspr", "Chords Spread", 50), ("cpush", "Chords Push", 0),
                               ("cgate", "Chords Gate", 50), ("cstrum", "Chords Strum", 0), ("cdyn", "Chords Dynamics", 50)):
        p.append({"key": key, "name": name, "min": 0, "max": 100, "default": default, "display": "int"})
    # 0.10 (+81), appended: the BASS tab's mode, its IDEAS, SHAPE and FIX A NOTE cells and readouts (src/engine.c TAPS), then
    # the SHAPE knobs (knob 1 is cx_2)
    p.append({"key": "bmode", "name": "Bass Mode", "options": ["Ideas", "Shape", "Fix"], "default": 0})
    bass = [("bmd", "Bass Mode", 3), ("bds", "Bass Idea Distance", 3), ("bfd", "Bass Idea Field", 9), ("bia", "Bass Idea Action", 3),
            ("bpn", "Bass Idea Pin", 4), ("bmv", "Bass Move", 6), ("bitag", "Bass Idea Tag", 0), ("bsy", "Bass Starts From", 4),
            ("boc", "Bass Octave", 3), ("bap", "Bass Approach", 2), ("bst", "Bass Fix Step", 16), ("bnk", "Bass Fix Note", 12),
            ("bno", "Bass Fix Octave", 4), ("brs", "Bass Fix Rest", 1), ("bdn", "Bass Fix Done", 1), ("bvw", "Bass Fix Bar", 0)]
    for prefix, name, n in bass:
        if not n:   # a readout
            p.append({"key": prefix, "name": name, "display": "string"})
        p += [{"key": "%s_%d" % (prefix, i), "name": "%s %d" % (name, i), "display": "string"} for i in range(1, n + 1)]
    for key, name, default in (("bmov", "Bass Movement", 0), ("bsync", "Bass Sync", 0), ("boct", "Bass Octaves", 0),
                               ("breg", "Bass Register", 50), ("bpush", "Bass Push", 0), ("bgate", "Bass Gate", 50),
                               ("bacc", "Bass Accent", 50)):
        p.append({"key": key, "name": name, "min": 0, "max": 100, "default": default, "display": "int"})
    # 0.11 (+100), appended: the MELODY tab's mode, its RECIPE, IDEAS, SHAPE and FIX NOTE cells and readouts (src/engine.c
    # TAPS), then the SHAPE knobs 2-8 (knob 1 is cx_3), at the Hook's starts (a new Melody is a classic Hook)
    p.append({"key": "mmode", "name": "Melody Mode", "options": ["Recipe", "Ideas", "Shape", "Fix"], "default": 0})
    melody = [("mmd", "Melody Mode", 4), ("mty", "Melody Type", 7), ("mcap", "Melody Type Hint", 0), ("mo1", "Melody Option 1", 4),
              ("mo2", "Melody Option 2", 4), ("mo3", "Melody Option 3", 4), ("mol", "Melody Option Row", 3), ("mph", "Melody Phrase", 2),
              ("mbars", "Melody Phrase Bars", 0), ("mkst", "Melody Knob Starts", 0), ("mds", "Melody Idea Distance", 3),
              ("mfd", "Melody Idea Field", 9), ("mia", "Melody Idea Action", 3), ("mpn", "Melody Idea Pin", 4), ("mmv", "Melody Move", 4),
              ("mitag", "Melody Idea Tag", 0), ("mrk", "Melody Reset Knobs", 1), ("mst", "Melody Fix Step", 16),
              ("mnk", "Melody Fix Note", 12), ("mno", "Melody Fix Octave", 5), ("mrt", "Melody Fix Rest", 1), ("mdn", "Melody Fix Done", 1),
              ("mvw", "Melody Fix Bar", 0)]
    for prefix, name, n in melody:
        if not n:   # a readout
            p.append({"key": prefix, "name": name, "display": "string"})
        p += [{"key": "%s_%d" % (prefix, i), "name": "%s %d" % (name, i), "display": "string"} for i in range(1, n + 1)]
    for key, name, default in (("mrng", "Melody Range", 45), ("mlea", "Melody Leaps", 30), ("msyn", "Melody Sync", 35),
                               ("mten", "Melody Tension", 25), ("mans", "Melody Answer", 50), ("mreg", "Melody Register", 55),
                               ("mgat", "Melody Gate", 70)):
        p.append({"key": key, "name": name, "min": 0, "max": 100, "default": default, "display": "int"})
    # 0.12 (+17), appended: CHORDS NAMES, NEW PROGRESSION, the nine chord rhythms, NEW RHYTHM (src/engine.c TAPS), FIX's slot
    # count (engine-set, "-" off FIX; the two FIX layouts hang on it), then the 4-slot FIX layout's slots (fs_1..4 again)
    for prefix, name, n in (("cnm", "Chords Names", 1), ("cnp", "New Progression", 1), ("crh", "Chord Rhythm", 9), ("cnr", "New Rhythm", 1)):
        p += [{"key": "%s_%d" % (prefix, i), "name": "%s %d" % (name, i), "display": "string"} for i in range(1, n + 1)]
    p.append({"key": "cslots", "name": "Fix Slots", "options": ["-", "4", "8"], "default": 0})   # "-" off FIX (engine-set)
    p += [{"key": "fsl_%d" % i, "name": "Fix Slot Large %d" % i, "display": "string"} for i in range(1, 5)]
    # 0.13 (+14), appended: the stepped knobs (decision 5; src/engine.c KSTEPS maps each step to its old param's value, and
    # the text is the engine's, dynamic_display), then CHORDS SHAPE's feedback readout
    for key, name, opts, default in STEPPED:
        p.append({"key": key, "name": name, "options": opts, "default": default, "qlink_ticks": 3, "dynamic_display": True})
    p.append({"key": "cshtag", "name": "Shape Feedback", "display": "string"})
    # 0.14 (+25), appended: CHORDS FIX by bar: the slots per bar (engine-set, "-" off FIX; the bar's slot layouts hang on it),
    # the bar's slots, the bar strip, the loop's note, then FIX's own bar stepper over the shared view (whole chords: 2 bars a
    # step at 2-bar chords; the text is the engine's, "BAR 7", "BARS 7-8")
    p.append({"key": "cbar", "name": "Fix Bar Slots", "options": ["-", "1", "2", "4", "Jam"], "default": 0})   # 0.22 appended Jam
    p += [{"key": "fbs_%d" % i, "name": "Fix Bar Slot %d" % i, "display": "string"} for i in range(1, 5)]
    p += [{"key": "fbar_%d" % i, "name": "Fix Bar %d" % i, "display": "string"} for i in range(1, 17)]
    p.append({"key": "cfixnote", "name": "Fix Loop Note", "display": "string"})
    p.append({"key": "cfview", "name": "Fix View", "options": ["Bar %d" % b for b in range(1, 17)], "default": 0, "dynamic_display": True})
    p.append({"key": "cfview_prev", "name": "Fix View Prev", "momentary": True, "step_of": "cfview", "step_delta": -1})
    p.append({"key": "cfview_next", "name": "Fix View Next", "momentary": True, "step_of": "cfview", "step_delta": 1})
    # 0.15, appended: MPC's PRESET menu (vst.json "programs"). Off the skin and unpolled; the engine reads Init until the next
    # edit, then Current, so picking Init after an edit always resets (the wrapper skips a pick of the current program)
    # 0.23: the genre presets after Init (src/presets.c, in its order; the wrapper names each program by its option)
    p.append({"key": "init", "name": "Preset", "options": ["Current", "Init", "House 124", "Techno 130", "Trance 138", "Pop 110",
                                                           "Rock 120", "Disco 118"], "default": 0, "poll": False})
    # VST-12, appended: MELODY RECIPE phrasing (src/engine.c TAPS): Form, Breath, Cadence and Fill's cells, the hint line (what
    # the setting set last does) and RESET PHRASE
    for key, name, n in PHRASING:
        p += [{"key": "%s_%d" % (key, i), "name": "%s %d" % (name, i), "display": "string"} for i in range(1, n + 1)]
    p.append({"key": "mphint", "name": "Melody Phrase Hint", "display": "string"})
    p.append({"key": "mpr_1", "name": "Melody Reset Phrase", "display": "string"})
    # 0.16, appended: BASS and MELODY FIX by bar (src/engine.c TAPS): each lane's bar strip, NEW BAR / RESET BAR, its summary
    for lane, name in (("b", "Bass"), ("m", "Melody")):
        p += [{"key": "%snb_%d" % (lane, i), "name": "%s Fix Bar %d" % (name, i), "display": "string"} for i in range(1, 17)]
        p += [{"key": "%sba_%d" % (lane, i), "name": "%s Bar Action %d" % (name, i), "display": "string"} for i in (1, 2)]
        p.append({"key": lane + "fn", "name": name + " Fix Note", "display": "string"})
    p += chords_page_params()
    p += rhythm_page_params()
    p += bass_page_params()
    p += bass_rhythm_params()
    p += melody_page_params()
    p += phrase_page_params()
    p += mknob_params(MKNOBS_21, 6)   # 0.21, appended: the MELODY tab's knobs 6-8
    p += jam_22_params()   # 0.22, appended: the CHORDS and BASS JAM pages
    p += phrase_24_params()   # 0.24, appended: knob 7's slots, OCTAVE, NAMES on GLOBAL, the sound pages' status row and centre
    p += phrase_cells_24_params()   # 0.24 Task 7, appended: the PHRASE pages of CHORDS and BASS
    p += sound_dots_24_params()     # 0.24 Task 8 fix, appended: the BASS sound page's centre dots
    for q in p:   # 0.22 the tabs open on JAM: the slots' defaults are the option a new instance reports there (R4), as 0.21's MELODY
        if q["key"] in JAM_DEFAULTS_22:
            q["default"] = JAM_DEFAULTS_22[q["key"]]
    for q in p:
        if q["key"] in DYNAMIC:   # the knob's text is the engine's ("7ths 1/4", "Gate 40%"): not positional
            q["dynamic_display"] = True
        if q["key"] in ("rh_1", "rh_2", "rh_3"):   # 0.12: crh_1..9 replace them on the skin; they still answer
            q["poll"] = False
        if q["key"] in STEPPED_OLD or q["key"] == "act_2":   # 0.13: the stepped knobs and PLAY's UNDO alone replace them
            q["poll"] = False
        if re.match(r"fsl?_\d+$", q["key"]):   # 0.14: FIX by bar (fbs_, fbar_) replaces the slot cells; they still answer
            q["poll"] = False
        if q["key"] in ("mph_1", "mph_2"):   # VST-12: the phrasing steppers replace CALL + ANSWER; they still answer
            q["poll"] = False
        if q["key"] in RETIRED_17 or re.match(r"(md|ds|fd|ia|pn|mv|vo|oc)_\d+$", q["key"]):   # 0.17: the CHORDS page
            q["poll"] = False   # replaces IDEAS and SHAPE; they still answer
        if q["key"] in RETIRED_18 or re.match(r"crh_\d+$", q["key"]):   # 0.18: the RHYTHM page replaces them; NEW IDEA is dropped
            q["poll"] = False
        if q["key"] in RETIRED_19 or re.match(RETIRED_19_CELLS, q["key"]):   # 0.19: the BASS page replaces them; they still answer
            q["poll"] = False
        if q["key"] in RETIRED_20 or re.match(RETIRED_20_CELLS, q["key"]):   # 0.20: the MELODY JAM and PHRASE pages replace them
            q["poll"] = False
        if q["key"] in UNPLACED_21:   # 0.21: the MELODY slots k_j6..k_j8 replace them on the skin
            q["poll"] = False
        if q["key"] in RETIRED_24 or re.match(RETIRED_24_CELLS, q["key"]):   # 0.24: PHRASE and the sound pages replace them
            q["poll"] = False
        if q["key"] in RETIRED_24P or re.match(RETIRED_24P_CELLS, q["key"]):   # 0.24 Task 7: PHRASE's own cells replace them
            q["poll"] = False
    for q in p:   # 0.9: the chord strip (chord_1..8) left the skin with the old CHORDS tab; unpolled, it costs no audio-thread time
        if q["key"].startswith("chord_") and q["key"] != "chord_len":
            q["poll"] = False
    json.dump({"name": "Cadenza", "params": p}, open(os.path.join(HERE, "params.json"), "w"), indent=1)
    write_keyflags(p)
    return len(p)


INK, DIM, LIGHT = "e9e6df", "9aa0a8", "f4efe6"
# 0.12 text floors (decision 1): a list cell's text, a readout's (and a stepper's, a knob's value), a baked name (x 10 px)
TX, RX, NX = 24, 22, 2.0


def hexc(colour):
    return "%02x%02x%02x" % colour


def tiles(w, h):
    TILES.add((w, h))
    return "img=images/tile_off_%dx%d.png img_on=images/tile_on_%dx%d.png" % (w, h, w, h)


def name(a, x, cy, text, colour=DIM, align="left", spacing=2):
    """A 20 px section or control name, baked into the page (0.20 spacing: the letters' tracking, 1 where a column is narrow)"""
    a('text cx=%d cy=%d label="%s" align=%s size=%.1f weight=600 spacing=%d color=%s' % (x, cy, text, align, NX, spacing, colour))


def hint(a, x, cy, text, colour="c9c5bd", align="left"):
    """A 20 px line of help, baked into the page (sentence case)"""
    a('text cx=%d cy=%d label="%s" align=%s size=%.1f case=none color=%s' % (x, cy, text, align, NX, colour))


def knob(a, cx, cy, key, bw=130):
    """An r=24 knob (a bw x 87 touch box) with a 22 px value; its name is baked text above it (the param names are 0.6's)"""
    a("knob cx=%d cy=%d r=24 key=%s ns=0 vs=%d bw=%d" % (cx, cy, key, RX, bw))


def cells(a, x, y, w, h, key, cols, rows=1, gap=8, size=TX, colour=INK, img=None, pad=12, band=None):
    """A list of 64 px (or taller) cells; pad: the text's inset from each side (narrow cells with long text take less); band:
    (ty, tth), the text's rows in the cell (0.18), else the whole cell"""
    tw, th = (w - (cols - 1) * gap) // cols, (h - (rows - 1) * gap) // rows
    a("list x=%d y=%d w=%d h=%d key=%s cols=%d rows=%d th=%d gap=%d %s tsize=%d tcolor=%s talign=center tx=%d ttw=%d%s"
      % (x, y, w, h, key, cols, rows, th, gap, img or tiles(tw, th), size, colour, pad, tw - 2 * pad,
         " ty=%d tth=%d" % band if band else ""))


def row_w(n, w, gap=8):
    """n cells of w px, gap apart"""
    return n * w + (n - 1) * gap


def readout(a, x, cy, w, key, h=28, colour="c9c5bd", align="left", size=RX, pad=None):
    """A live text line from x, w px wide; pad: the text's inset (tpad, the toolchain's default 8)"""
    a("readout cx=%d cy=%d w=%d h=%d label=\"\" key=%s box=0 tsize=%d tcolor=%s talign=%s%s"
      % (x + w // 2, cy, w, h, key, size, colour, align, "" if pad is None else " tpad=%d" % pad))


def header(a, title, colour=INK):
    a('text cx=%d cy=126 label="%s" align=left size=2.4 weight=700 spacing=3 color=%s' % (L, title, colour))


def part_readout(a):
    """The playing (and edited) part's name, right of the tab title"""
    a("readout cx=400 cy=126 w=360 h=44 label=\"\" key=part box=0 tsize=%d tcolor=f2a33a talign=left" % RX)


STEP_W = 224                    # PLAY and FIX's view stepper: "Bar 16" at 22 px between its two 64 px arrows


def play_tab(a):
    a("[tab PLAY]")
    act_w = 130                                                  # UNDO
    cells(a, L, HEAD_Y, R - L - act_w - 16, 64, "pt", 6)   # the parts: a tap queues one for the next bar line (lit: playing)
    cells(a, R - act_w, HEAD_Y, act_w, 64, "act", 1, pad=6)
    readout(a, L, LINE_Y + 12, 760, "pos", colour="f2a33a")
    readout(a, R - 480, LINE_Y + 12, 480, "title", colour="b4b9c0", align="right")
    name(a, L, BAR_Y + 32, "BARS")   # FOLLOW moved to GLOBAL (after 0.16)
    cells(a, GX, BAR_Y, BAR_W, 64, "bar", 8, gap=CELL_GAP, pad=2)   # bars 1-8; the stepper reaches 9-16
    a("stepper cx=%d cy=%d w=%d h=64 key=view prev=view_prev next=view_next tsize=%d" % (GX + BAR_W + 8 + STEP_W // 2, BAR_Y + 32, STEP_W, RX))
    readout(a, R - VCHORD_W, BAR_Y + 32, VCHORD_W, "vchord", h=64, colour="f2a33a", align="center")
    a("list x=%d y=%d w=%d h=%d key=ph cols=%d rows=1 th=%d gap=%d tap=no mark=1 tcolor=none" % (GX, PH_Y, GW, PH_H, STEPS, PH_H, CELL_GAP))
    cells(a, L, ROW0, 120, 3 * ROW_PITCH - (ROW_PITCH - CELL_H), "mt", 1, rows=3, gap=ROW_PITCH - CELL_H,
          img="img=images/tile_off_120x%d.png img_on=images/mute_on.png" % CELL_H)   # lane names: a tap mutes (red)
    for n in range(1, len(LANES) + 1):
        y = ROW0 + (n - 1) * ROW_PITCH
        a('art file="images/lane_%d.png" x=%d y=%d w=4 h=%d' % (n, GX - 8, y, CELL_H))
        a("list x=%d y=%d w=%d h=%d key=l%d cols=%d rows=1 th=%d gap=%d img=images/cell_off.png img_on=images/cell_l%d_on.png "
          "tsize=%d tcolor=111214 talign=center tx=2 ttw=%d" % (GX, y, GW, CELL_H, n, STEPS, CELL_H, CELL_GAP, n, TX, CELL_W - 4))
    knobs = [(lane, hexc(colour), "cx_%d" % n) for n, (lane, colour) in enumerate(LANES, 1)]
    knobs += [("SWING", INK, "swing"), ("GATE", INK, "gate"), ("DYNAMICS", INK, "dyn")]
    for i, (label, colour, key) in enumerate(knobs):
        name(a, 140 + 200 * i, KNOB_Y - 50, label, colour, "center")
        knob(a, 140 + 200 * i, KNOB_Y, key)
    a('qlinks "PLAY" = cx_1,cx_2,cx_3,swing,gate,dyn')


MD_X = L + 120                  # the lane tabs' mode cells, right of the 2.4 title (CHORDS is about 105 px)


def when(a, cond):
    """a's lines shown only while cond ("<param>:<option>") holds; .raw is a itself, for a line that needs another condition
    (a line takes one when=)"""
    def emit(line):
        a("%s when=%s" % (line, cond))
    emit.raw = a
    return emit


# 0.17 the CHORDS page (VST-15; layout y = plugin y + 86): the header, the strip, the main area, KEYS. 0.24 (VST-31) the CHORDS and BASS
# tabs' sound and PHRASE pages share this frame (sp_*, below)
CS_Y, CS_H = 166, 80            # the strip (plugin y 80-160): 0.24 the sound pages' status row, PHRASE's strip
CM_Y, CM_H = 254, 388           # the main area (plugin y 168-556)
CK_Y, CK_H = 650, 44            # KEYS (plugin y 564-608)
KW_W, KW_GAP, KE_W = 32, 2, 24  # KEYS: a white key (35), the gap between them, the edge boxes
KB_W, KB_H = 20, 26             # a black key
KW_X = L + KE_W + 6             # the white keys' left edge, right of the low edge box
DRIFT_COLOURS = [(0x3c, 0xba, 0x6a), (0xe6, 0xc8, 0x4a), (0xf0, 0x78, 0x20), (0xe2, 0x3a, 0x32)]   # the tint gels' hues:
# close (tint 5), colourful (0), tense (2), dissonant (1), as the strip's cells (src/engine.c DRIFT_TINT)
# the ring, in reading order (ring cell = row * 3 + column; 4 is the centre chord): Force knobs 1-8 (the CHORDS qlinks)
# 0.18: knobs 2-5 are the mode-following slots k_s2..k_s5 (COLOUR..STRUM here). 0.24: the CHORDS sound page's
RING = [("1 COMPLEXITY", "cx_1", 0), ("2 COLOUR", "k_s2", 1), ("3 INVERSION", "k_s3", 2), ("4 SPREAD", "k_s4", 3),
        ("5 STRUM", "k_s5", 5), ("6 PUSH", "cpush", 6), ("7 GATE", "cgate", 7), ("8 DYNAMICS", "cdyn", 8)]
# 0.22 the CHORDS and BASS tabs' Q-Links: knob 1, 6 and 8 the slots that follow the mode (JAM, the own page, RHYTHM), 7 the lane's GATE
QLINKS_22 = (("k_s1", "k_s2", "k_s3", "k_s4", "k_s5", "k_s6", "cgate", "k_s8"), ("k_b1", "k_b2", "k_b3", "k_b4", "k_b5", "k_b6", "bgate", "k_b8"))
# 0.24 knob 7 is a slot too (k_s7, k_b7: GATE on JAM and the own page, FILL on PHRASE); cgate and bgate still answer (a 0.23 project's map)
QLINKS_24 = tuple(tuple("k_%s%d" % (t, i) for i in range(1, 9)) for t in "sb")
PROG_ROWS = ["POP", "MOODY", "CLASSIC", "LOOPS"]   # the PROGRESSIONS browser's rows (src/engine.c BPR_ORDER)
TITLE_Y = CM_Y + 8              # the browsers' title row: 64 px


def button(a, x, y, w, h, key, img, img_on):
    """A sized image button: a tap toggles key (a tap cell the engine reads), drawn from the port's own pictures"""
    a('button cx=%d cy=%d w=%d h=%d label="" key=%s img=images/%s img_on=images/%s' % (x + w // 2, y + h // 2, w, h, key, img, img_on))


def hit_plate(a, x, y, w, h, key, lit=True):
    """0.18 a transparent tap over a readout (0.17's field pattern): it taps key; lit: the amber plate while key is lit"""
    HITS.add((w, h))
    if lit:
        HITS_ON.add((w, h))
    button(a, x, y, w, h, key, "hit_%dx%d.png" % (w, h), "hit_%s%dx%d.png" % ("on_" if lit else "", w, h))


def art(a, x, y, w, h, img):
    a('art file="images/%s" x=%d y=%d w=%d h=%d' % (img, x, y, w, h))


# 0.24 (VST-31) the three-page model on the CHORDS and BASS tabs (spec "The pages"; boards V24Chords, BassR3Page, ChPhr2Page / Edit / Lock,
# BassPhr2Page / Edit / Lock, ChPhrCopy, BassPhrCopy): JAM (0.22), the sound page and PHRASE. Both pages have the boards' 450 px left panel
# with the ring's panel right of it (so the ring sits in the same place on both), SP_H tall, then one line under both: the colour legend
# (the boards': a word, the 5 colours' gradient, a word) and the message line. Over the main area: the sound pages' status row (as JAM's),
# PHRASE's strip (as MELODY's). Text floors move four things from the boards: the ring's where-line and its end or word line run across
# the ring's top and foot (as MELODY PHRASE's: "PLAYING · BAR 6 · tap to edit on PHRASE" at 22 px is wider than the centre), the strip's
# tag line and its chords are two lines over 30 px dots, the editor's beat labels sit beside the open bar's dots (no room for a row of
# their own), and COPY TO...'s actions share its title row (its bars are 4 rows of 4, as 0.18's). The PROGRESSIONS and VOICING browsers
# take the main area's width (as BASS's STYLE browser): their cells don't fit right of the wider left panel
SP_LW, SP_H = 450, 356          # the left panel (x 16-466), both panels' height
SP_RX = L + SP_LW + 10          # the ring's panel (x 476-1264)
SP_RW = R - SP_RX
SR_SW, SR_GAP = 186, 6          # the ring's side columns (a knob's 18-character value at 22 px), the gap between columns
SR_MW = SP_RW - 12 - 2 * SR_SW - 2 * SR_GAP   # its middle column (392): the centre
SR_Y0, SR_H, SR_PITCH = CM_Y + 26, 94, 99    # its rows, under the where-line, over the end line (MELODY PHRASE's)
SP_LINE_CY = CM_Y + SP_H + 16   # the line under both panels: the legend, the message line
SF_W = SP_LW - 12 - 2 * 72      # a left-panel field, between its 64 px arrows
SN_W, SE_W = 280, 200           # the status row's NOW PLAYING box ("Uplifting Octaves (edited)" at 22 px) and EDIT BARS
PEDAL_W = (96, 150, 176)        # PEDAL's cells: "OFF", "TONIC I · C#", "DOMINANT V · C#" at 24 px
BASS_RING = [("1 COMPLEXITY", "cx_2", 0), ("2 MOVEMENT", "k_b2", 1), ("3 OCTAVES", "k_b3", 2), ("4 APPROACH", "k_b4", 3),
             ("5 REGISTER", "k_b5", 5), ("6 PUSH", "bpush", 6), ("7 GATE", "bgate", 7), ("8 DYNAMICS", "bacc", 8)]
DRIFT_EDGES = {"t": (0, 0, SR_MW, 12), "b": (0, SR_H - 12, SR_MW, 12), "l": (0, 12, 4, SR_H - 24), "r": (SR_MW - 4, 12, 4, SR_H - 24)}
# the CHORDS centre frame's edges (x, y, w, h in the centre: the rounded corners in the top and bottom ones)
LEGEND_CHORDS, LEGEND_BASS = ("close", 44, "dissonant", 83), ("chord tones", 101, "clash", 44)   # the legend's words and their widths at
# 20 px (Titillium Web 400, measured as LEGEND's)
# the dots (meters over filmstrips, src/engine.c chords_frame and bass_frame: CHORDS 36 frames as the Task 5 probe drew them,
# probe/dots_chords/make_dots.py; BASS MELODY's 73 over the bass range plus 60 ticked notes): (w, h, a note's dot height) of the centre's,
# the editor's and the strip's
PD_C, PD_E, PD_S = (16, 46, 6), (16, 30, 4), (8, 30, 4)
# a PHRASE page's keys and looks: p the cells' prefix (cq CHORDS, uq BASS), the title's ink, the other lane (LOCK WITH THE ...), the ring's
# knobs (Force knobs 1-8, the tab's qlinks: knobs 2-8 the slots in their PHRASE mode), the legend, the editor's and COPY TO...'s panel
# (framed in the lane's colour), the dots' frame count
PhPage = collections.namedtuple("PhPage", "p ink other ring legend panel frames")
PH_CHORDS = PhPage("cq", hexc(LANES[0][1]), "BASS",
                   [("1 COMPLEXITY", "cx_1", 0), ("2 SYNC", "k_s2", 1), ("3 GHOSTS", "k_s3", 2), ("4 HUMANIZE", "k_s4", 3),
                    ("5 CADENCE", "k_s5", 5), ("6 BREATH", "k_s6", 6), ("7 FILL", "k_s7", 7), ("8 PUSH", "k_s8", 8)],
                   LEGEND_CHORDS, "rpanel", PHRASE_24_DOTS["cq"] + 1)
PH_BASS = PhPage("uq", hexc(LANES[1][1]), "CHORDS",
                 [("1 COMPLEXITY", "cx_2", 0), ("2 SYNC", "k_b2", 1), ("3 GHOSTS", "k_b3", 2), ("4 HUMANIZE", "k_b4", 3),
                  ("5 CADENCE", "k_b5", 5), ("6 BREATH", "k_b6", 6), ("7 FILL", "k_b7", 7), ("8 PUSH", "k_b8", 8)],
                 LEGEND_BASS, "upanel", PHRASE_24_DOTS["uq"] + 1)


def sp_ring_cell(i):
    """0.24 (x, y, w) of the sound and PHRASE pages' ring cell i (row-major 0..8; 4 the centre)"""
    col, row = i % 3, i // 3
    x = SP_RX + 6 + (0, SR_SW + SR_GAP, SR_SW + SR_MW + 2 * SR_GAP)[col]
    return x, SR_Y0 + row * SR_PITCH, SR_MW if col == 1 else SR_SW


def lane_header(a, n, jam, modes, count, fol):
    """The CHORDS or BASS tab's title (LANES[n]), JAM (jam: 0.22 cvm_4 / ubm_3, first: a list starts at _1), the lane's page and PHRASE
    (modes_1..2), the bar counter over FOLLOW (count, fol), PLAY's part cells. 0.24 CHORDS's FIX and NAMES cells leave (NAMES is GLOBAL's)"""
    title, colour = LANES[n]
    a("[tab %s]" % title)
    header(a, title, hexc(colour))
    tap_cell(a, MD_X, HEAD_Y, 100, jam)
    cells(a, MD_X + 108, HEAD_Y, row_w(2, 100), 64, modes, 2, pad=3)
    pt_w = 84
    rx = MD_X + row_w(3, 100) + 16
    readout(a, rx, HEAD_Y + 16, R - row_w(6, pt_w) - 8 - rx, count, colour="f2a33a", pad=4)   # "> BAR 15 / 16", "FOLLOW PAUSED"
    readout(a, rx, HEAD_Y + 48, R - row_w(6, pt_w) - 8 - rx, fol, colour=DIM, pad=4)
    cells(a, R - row_w(6, pt_w), HEAD_Y, row_w(6, pt_w), 64, "pt", 6, pad=4)


def chords_tab(a):
    """The CHORDS tab (0.17; 0.24 JAM, CHORDS, PHRASE): the header, the two panels the sound and PHRASE pages draw on, and KEYS on every
    view; the views hang on cview. Mode images are drawn in the order their first line comes (later on top), so: the sound page, the
    browsers, PHRASE's page, its editor, COPY TO... and the RHYTHMS browser, JAM; then the status row (cmode Ideas: the sound page and its browsers) and PHRASE's strip (cmode
    Shape: PHRASE's four views) last, as a view's readouts reach up over them (shadow_skin.py baked_rect). No stepper
    (check_stepper_arrows)"""
    title = LANES[0][0]
    lane_header(a, 0, "cvm_4", "cvm", "ccount", "cfol")
    sp_panels(a)
    chords_keys(a)
    chords_sound(when(a, "cview:Chords"))
    chords_progressions(when(a, "cview:Progressions"))
    chords_voicing(when(a, "cview:Voicing"))
    phrase_page(when(a, "cview:Rhythm"), PH_CHORDS)
    phrase_editor(when(a, "cview:Bar"), PH_CHORDS)
    phrase_copy(when(a, "cview:Copy"), PH_CHORDS)
    rhythm_browser(when(a, "cview:Rhythms"), RP_CHORDS)
    lane_jam(when(a, "cview:Jam"), L, JP_CHORDS)   # 0.22
    sp_status(when(a, "cmode:Ideas"), "cs", hexc(LANES[0][1]), "snow_c")
    phrase_strip(when(a, "cmode:Shape"), PH_CHORDS)
    a('qlinks "%s" = %s' % (title, ",".join(QLINKS_24[0])))


def bass_tab(a):
    """The BASS tab (0.19; 0.24 JAM, BASS, PHRASE): the header, the panels and KEYS on every view; the views hang on bview, the status row
    and the strip on yview (engine-set: Bass on the BASS page and its STYLE browser, Edit on PHRASE and its editor, Other on its COPY TO...
    and RHYTHMS; Jam). The views first, the status row and the strips last (a view's readouts reach up over them). No stepper"""
    title = LANES[1][0]
    lane_header(a, 1, "ubm_3", "ubm", "ucnt", "ufol")
    sp_panels(a)
    chords_keys(a, "uk", "u")
    lane_jam(when(a, "bview:Jam"), L, JP_BASS)   # 0.22
    bass_sound(when(a, "bview:Bass"))
    bass_styles(when(a, "bview:Style"))
    phrase_page(when(a, "bview:Rhythm"), PH_BASS)
    phrase_editor(when(a, "bview:Bar"), PH_BASS)
    phrase_copy(when(a, "bview:Copy"), PH_BASS)
    rhythm_browser(when(a, "bview:Rhythms"), RP_BASS)
    sp_status(when(a, "yview:Bass"), "us", hexc(LANES[1][1]), "snow_u")
    for g in ("Edit", "Other"):
        phrase_strip(when(a, "yview:" + g), PH_BASS)
    a('qlinks "%s" = %s' % (title, ",".join(QLINKS_24[1])))


def sp_panels(a):
    """0.24 the left panel and the ring's panel the sound and PHRASE pages draw on (every other view covers them)"""
    art(a, L, CM_Y, SP_LW, SP_H, "panel_%dx%d.png" % (SP_LW, SP_H))
    art(a, SP_RX, CM_Y, SP_RW, SP_H, "panel_%dx%d.png" % (SP_RW, SP_H))


def sp_status(a, p, ink, box):
    """0.24 a sound page's status row (as JAM's, 0.22): NOW PLAYING (p + "now", in the lane's box), the playing phrase's 4 bars (p + "b_1..4":
    "BAR 6" in the top band, lit while it plays; its chords p + "c_1..4", "VI · C", under it under a hit plate, so a tap there still taps
    the bar), EDIT BARS (p + "e_1", "ON PHRASE" baked under it: a cell a list can't start, as tap_cell)"""
    art(a, L, CS_Y, SN_W, CS_H, "%s_%dx%d.png" % (box, SN_W, CS_H))
    name(a, L + 12, CS_Y + 22, "NOW PLAYING")
    readout(a, L, CS_Y + 54, SN_W, p + "now", h=28, colour=ink, pad=6)
    bx = L + SN_W + 8
    bw = (R - SE_W - 8 - bx - 3 * 8) // 4
    cells(a, bx, CS_Y, row_w(4, bw), CS_H, p + "b", 4, pad=4, band=(4, 32))
    for i in range(4):
        x = bx + i * (bw + 8)
        readout(a, x, CS_Y + 56, bw, "%sc_%d" % (p, i + 1), h=26, colour="c9a36a", align="center", pad=4)
        hit_plate(a, x, CS_Y + 40, bw, CS_H - 40, "%sb_%d" % (p, i + 1), lit=False)
    ex = R - SE_W
    TILES.add((SE_W, CS_H))
    art(a, ex, CS_Y, SE_W, CS_H, "tile_off_%dx%d.png" % (SE_W, CS_H))
    readout(a, ex, CS_Y + 28, SE_W, p + "e_1", h=30, colour=INK, align="center", size=TX, pad=5)
    name(a, ex + SE_W // 2, CS_Y + 58, "ON PHRASE", DIM, "center", spacing=1)
    hit_plate(a, ex, CS_Y, SE_W, CS_H, p + "e_1")


def sp_field(a, y, keys, lines):
    """0.24 a left-panel field row from y: <, the box (lines: its readouts, (key, cy from y, h, colour); a tap on it taps keys[1]: it opens the
    browser), >; keys: the three cells (image buttons, as 0.17's PROGRESSION field and 0.18's RHYTHM field)"""
    x0 = L + 6
    for key, side, ax in ((keys[0], "l", x0), (keys[2], "r", x0 + SP_LW - 12 - 64)):
        button(a, ax, y, 64, 64, key, "arrow64_%s_off.png" % side, "arrow64_%s_on.png" % side)
    fx = x0 + 72
    art(a, fx, y, SF_W, 64, "sfield_%dx64.png" % SF_W)
    for key, dy, h, colour in lines:
        readout(a, fx, y + dy, SF_W, key, h=h, colour=colour, align="center", pad=6)
    hit_plate(a, fx, y, SF_W, 64, keys[1])


def sp_ring(a, ring, where, end):
    """0.24 a sound or PHRASE page's ring: the where-line over it, the 8 knobs (their names baked) in reading order, the end line under it"""
    readout(a, SP_RX + 6, CM_Y + 14, SP_RW - 12, where, h=26, colour=DIM, align="center", pad=4)   # "PLAYING · BAR 6 · tap to edit"
    for label, key, cell in ring:
        x, y, w = sp_ring_cell(cell)
        name(a, x + w // 2, y + 8, label, INK, "center")
        knob(a, x + w // 2, y + 46, key, min(w - 4, 200))   # its box: 87 px from cy - 29, 6 px into the next row
    readout(a, SP_RX + 6, CM_Y + 334, SP_RW - 12, end, h=24, colour="c9c5bd", align="center", pad=4)


def sp_centre_name(a, key, x, y, w, cy=32, size=30):
    """0.24 the sound page's centre: the chord's name (CHORDS cct_1) or the bar's chords (BASS uct_1, over its dots) at size px, cy from the
    centre's top, over the notes line; a tap on the centre is key's (it opens PHRASE)"""
    a("readout cx=%d cy=%d w=%d h=%d label=\"\" key=%s box=0 tsize=%d tcolor=%s tweight=bold talign=center tpad=8"
      % (x + w // 2, y + cy, w, size + 4, key, size, LIGHT))
    hit_plate(a, x, y + 12, w, 64, key, lit=False)   # over the name and the notes, under knob 2's box


def sp_legend(a, lo, lo_w, hi, hi_w):
    """0.24 the colour legend under the panels (the boards'): lo, the 5 colours' gradient, hi. -> the message line's left edge"""
    x = L + 6
    hint(a, x, SP_LINE_CY, lo, "c9c5bd")
    x += lo_w + 18
    art(a, x, SP_LINE_CY - 4, 140, 8, "legend_140x8.png")
    hint(a, x + 158, SP_LINE_CY, hi, "c9c5bd")
    return x + 158 + hi_w + 24


def chords_sound(a):
    """0.24 the CHORDS sound page (cview Chords; V24Chords): the left panel (PROGRESSION and VOICING: <, the field (cpr_2, cvo_2; a tap opens
    the browser), >; OCTAVE cpo_ -1 0 +1, the whole part's; RESET KNOBS), the ring (the where-line cswh over it; the 8 knobs; the centre: the
    chord's name cct_1, its frame the drift's colour (cdrift, the tab's), its notes csnt; the word line cswd under it), the legend"""
    x0 = L + 6
    name(a, x0 + 4, CM_Y + 14, "PROGRESSION")
    sp_field(a, CM_Y + 26, ("cpr_1", "cpr_2", "cpr_3"), (("cpr_2", 32, 28, INK),))   # PROG_CHARS (22) at 22 px
    name(a, x0 + 4, CM_Y + 108, "VOICING")
    sp_field(a, CM_Y + 120, ("cvo_1", "cvo_2", "cvo_3"), (("cvo_2", 32, 28, INK),))
    name(a, x0 + 4, CM_Y + 202, "OCTAVE · THE WHOLE PART")
    cells(a, x0, CM_Y + 214, SP_LW - 12, 64, "cpo", 3, pad=6)
    cells(a, x0, CM_Y + SP_H - 70, SP_LW - 12, 64, "crk", 1, pad=6)                     # RESET KNOBS
    sp_ring(a, RING, "cswh", "cswd")
    x, y, w = sp_ring_cell(4)
    art(a, x, y, w, SR_H, "scentre_%dx%d.png" % (w, SR_H))
    for k, (ex, ey, ew, eh) in DRIFT_EDGES.items():   # the frame in the chord's drift colour (drift_edges)
        a("meter cx=%d cy=%d w=%d h=%d key=cdrift strip=images/drift_%s_%dx%d.png frames=4" % (x + ex + ew // 2, y + ey + eh // 2, ew, eh, k, ew, eh))
    sp_centre_name(a, "cct_1", x, y, w)
    readout(a, x, y + 70, w, "csnt", h=24, colour="c9c5bd", align="center", pad=6)   # "C4 G4 B4 E5"
    sp_legend(a, *LEGEND_CHORDS)


def bass_sound(a):
    """0.24 the BASS sound page (bview Bass; BassR3Page): the left panel (STYLE: <, the field ustn over ustg, a tap opens the STYLES browser,
    >; PEDAL; OCTAVE uoc_; RESET KNOBS), the ring (uswh over it; the 8 knobs; the centre: the bar's chords uct_1, its 16 dots usz_ (Task 8
    fix: the board's, uqdots_e), its first notes usnt; the word line uswd under it), the legend and the message line (umsg)"""
    x0 = L + 6
    name(a, x0 + 4, CM_Y + 14, "STYLE")
    sp_field(a, CM_Y + 26, ("usy_1", "usy_2", "usy_3"), (("ustn", 21, 26, INK), ("ustg", 46, 24, "c9a36a")))
    name(a, x0 + 4, CM_Y + 108, "PEDAL")
    x = x0
    for i, w in enumerate(PEDAL_W):   # tap_cell: their widths differ
        tap_cell(a, x, CM_Y + 120, w, "upd_%d" % (i + 1))
        x += w + 8
    name(a, x0 + 4, CM_Y + 202, "OCTAVE · THE WHOLE PART")
    cells(a, x0, CM_Y + 214, SP_LW - 12, 64, "uoc", 3, pad=6)
    cells(a, x0, CM_Y + SP_H - 70, SP_LW - 12, 64, "urk", 1, pad=6)                     # RESET KNOBS
    sp_ring(a, BASS_RING, "uswh", "uswd")
    x, y, w = sp_ring_cell(4)
    art(a, x, y, w, SR_H, "scentre_%dx%d.png" % (w, SR_H))
    dots(a, x + (w - 16 * 18 + 2) // 2, y + 36 + PD_E[1] // 2, 18, "usz_1", 16, PD_E, "uqdots_e.png", PH_BASS.frames)   # Task 8 fix: its dots
    sp_centre_name(a, "uct_1", x, y, w, cy=18, size=28)
    readout(a, x, y + 81, w, "usnt", h=24, colour="c9c5bd", align="center", pad=6)   # "C2 C3 C2 C3 C2 C3 ..."
    mx = sp_legend(a, *LEGEND_BASS)
    readout(a, mx, SP_LINE_CY, R - mx, "umsg", h=24, colour="f2a33a", pad=4)


def phrase_strip(a, P):
    """0.24 PHRASE's strip (ChPhr2Page): the page's 4 bars (P.p + "s_": "6 · LOCKED" in the top band, src/engine.c CST_CHARS; lit the centre
    bar or the open one), each with its chords (P.p + "h_", "C·G@9") and its 16 live dots (P.p + "zs_") under it, under a hit plate that
    opens the bar; the page arrows, BARS 1-4 over "phrase 1 of 4", UNDO, BACK TO PLAYING (strip_frame)"""
    p = P.p
    cells(a, L, CS_Y, CST_W, CS_H, p + "s", 4, pad=5, band=(1, 26))
    for i in range(4):
        x = L + i * 160
        readout(a, x, CS_Y + 37, 152, "%sh_%d" % (p, i + 1), h=24, colour="c9a36a", align="center", pad=4)
        dots(a, x + 4, CS_Y + 49 + PD_S[1] // 2, 9, "%szs_%d" % (p, i * 16 + 1), 16, PD_S, "%sdots_s.png" % p, P.frames)
        hit_plate(a, x, CS_Y + 26, 152, CS_H - 26, "%ss_%d" % (p, i + 1), lit=False)
    strip_frame(a, p + "g", p + "page", p + "u", p + "b", sub=p + "pgn")


def phrase_page(a, P):
    """0.24 PHRASE (cview / bview Rhythm; ChPhr2Page, BassPhr2Page): the left panel (RHYTHM · ALL BARS: <, the field P.p + "f_2", a tap opens
    the RHYTHMS browser, >, its line; LOCK WITH THE BASS / CHORDS: OWN, WITH, IN THE GAPS, its line; ANTICIPATE: OFF 16TH 8TH BEAT, its
    line), the ring (the where-line, the 8 PHRASE knobs, the centre: the bar's chords and its 16 live dots, a tap opens the editor; where it
    ends under the ring), the legend and the message line under both"""
    p, x0, w0 = P.p, L + 6, SP_LW - 12
    name(a, x0 + 4, CM_Y + 14, "RHYTHM · ALL BARS")
    sp_field(a, CM_Y + 26, (p + "f_1", p + "f_2", p + "f_3"), ((p + "f_2", 32, 28, INK),))   # "Push (edited)", "in the gaps of the bass"
    readout(a, x0, CM_Y + 104, w0, p + "fl", h=24, pad=2)
    name(a, x0 + 4, CM_Y + 128, "LOCK WITH THE " + P.other)
    cells(a, x0, CM_Y + 140, w0, 64, p + "w", 3, pad=4)
    readout(a, x0, CM_Y + 218, w0, p + "wl", h=24, pad=2)
    name(a, x0 + 4, CM_Y + 242, "ANTICIPATE · INTO EACH CHORD CHANGE")
    cells(a, x0, CM_Y + 254, w0, 64, p + "a", 4, pad=4)
    readout(a, x0, CM_Y + 332, w0, p + "al", h=24, pad=2)
    sp_ring(a, P.ring, p + "cw", p + "ce")
    x, y, w = sp_ring_cell(4)
    art(a, x, y, w, SR_H, "scentre_%dx%d.png" % (w, SR_H))
    readout(a, x, y + 17, w, p + "cc", h=26, colour=LIGHT, align="center", pad=4)   # "VI · C  ->  III · G"
    dots(a, x + (w - 16 * 18 + 2) // 2, y + 34 + PD_C[1] // 2, 18, p + "zc_1", 16, PD_C, "%sdots_c.png" % p, P.frames)
    hit_plate(a, x, y + 10, w, 64, p + "t_1", lit=False)   # under knob 2's box
    mx = sp_legend(a, *P.legend)
    readout(a, mx, SP_LINE_CY, R - mx, p + "msg", h=24, colour="f2a33a", pad=4)


# 0.24 the bar editor (cview / bview Bar; ChPhr2Edit, BassPhr2Edit), across the main area: rows from its top
RI_X0, RI_X1 = L + 10, R - 10   # the full-width panels' inner edges
PE_TY = CM_Y + 4                # the title (over the message line) and the actions
PE_PY = CM_Y + 74               # the paint row, COPY TO ALL and COPY TO... at its right
PE_DY = CM_Y + 142              # the open bar's 16 dots (PD_E), each step's beat label left of its dot
PE_MY = CM_Y + 173              # the steps' marks (a 5 px bar over each step: amber where a chord starts, dashed a hit the knobs add)
PE_SY = CM_Y + 178              # the steps: what plays in the top band, the third line under it
PE_KY = CM_Y + 248              # the picker's two rows, 70 apart
PE_SW = (RI_X1 - RI_X0 - 15 * 6) // 16   # a step (71)
PE_AW, PE_CW = 104, 150         # an action ("RESET BAR" at 24 px), a paint cell (7 a row: each 150, "COPY TO ALL" at 24 px)
PK_X = RI_X0 + 96               # the picker's cells, right of their row names ("BORROW")
BE_KX0 = RI_X0 + 76             # 0.19 a piano's first key, right of its name (MELODY's editor too)
PE_RC_W = 420                   # BASS: the column right of the piano (OCTAVE, FOLLOW CHORD)
MARK_H = 5


def phrase_editor(a, P):
    """0.24 the bar editor (ChPhr2Edit, BassPhr2Edit): its title (P.p + "tt") over the message line, NEW BAR, RESET BAR, LOCK / UNLOCK,
    CANCEL, DONE; PAINT ACCENT HIT GHOST HOLD REST, COPY TO ALL, COPY TO...; the open bar's 16 live dots (P.p + "zc_", the centre's: the
    editor shares them, lever 1) with the beat labels (baked: 1 e & a), the steps' marks (P.p + "m_", meters), the 16 steps (what plays,
    the third line P.p + "l_" under a hit plate); the picker: CHORDS CHORD (7), BORROW (3), TYPE, OCTAVE -2..+2, THIS HIT ONLY; BASS NOTE,
    a two-row piano (uqk_1..5 C# D# F# G# A# over the gaps of uqk_6..12 C..B: tap_cell), OCTAVE 1-4, FOLLOW CHORD"""
    p = P.p
    art(a, L, CM_Y, R - L, CM_H, "%s_%dx%d.png" % (P.panel, R - L, CM_H))
    ax = RI_X1 - row_w(5, PE_AW)
    readout(a, RI_X0, PE_TY + 20, ax - 8 - RI_X0, p + "tt", h=30, colour=P.ink, size=TX)   # "EDITING · BAR 6 · C·G@9 · LOCKED"
    readout(a, RI_X0, PE_TY + 48, ax - 8 - RI_X0, p + "msg", h=24, colour="f2a33a")
    cells(a, ax, PE_TY, row_w(5, PE_AW), 64, p + "e", 5, pad=3)
    name(a, RI_X0, PE_PY + 32, "PAINT")
    cells(a, PK_X, PE_PY, row_w(7, PE_CW), 64, p + "p", 7, pad=5)   # one list (a list row sends one set: COPY TO ALL acts once)
    cells(a, RI_X0, PE_SY, RI_X1 - RI_X0, 64, p + "n", STEPS, gap=6, pad=1, band=(3, 30))   # "Bbmaj7", "D2*" at 24 px
    for i in range(STEPS):
        x = RI_X0 + i * (PE_SW + 6)
        a('text cx=%d cy=%d label="%s" align=left size=%.1f weight=%d case=none color=%s'
          % (x + 6, PE_DY + 15, "1234"[i // 4] if i % 4 == 0 else "e&a"[i % 4 - 1], NX, 700 if i % 4 == 0 else 600,
             INK if i % 4 == 0 else "6b717a"))
        dots(a, x + (PE_SW - PD_E[0]) // 2, PE_DY + PD_E[1] // 2, 0, "%szc_%d" % (p, i + 1), 1, PD_E, "%sdots_e.png" % p, P.frames)
        a("meter cx=%d cy=%d w=%d h=%d key=%sm_%d strip=images/marks_%dx%d.png frames=4"
          % (x + PE_SW // 2, PE_MY + MARK_H // 2, PE_SW, MARK_H, p, i + 1, PE_SW, MARK_H))
        readout(a, x, PE_SY + 47, PE_SW, "%sl_%d" % (p, i + 1), h=24, colour=DIM, align="center", pad=0)   # "HIT +1", "+GHOST"
        hit_plate(a, x, PE_SY + 34, PE_SW, 30, "%sn_%d" % (p, i + 1), lit=False)
    (chords_picker if p == "cq" else bass_picker)(a)


def chords_picker(a):
    """0.24 the CHORDS editor's picker: CHORD, the 7 scale chords (cqk_); BORROW, the 3 borrowed (cqr_), TYPE (cqy_1, it cycles) and THIS HIT
    ONLY (cqy_2: one list, so a touch sends one set; the board has it last), OCTAVE -2..+2 (cqo_)"""
    name(a, RI_X0, PE_KY + 32, "CHORD")
    cells(a, PK_X, PE_KY, RI_X1 - PK_X, 64, "cqk", 7, pad=4)
    y = PE_KY + 70
    name(a, RI_X0, y + 32, "BORROW")
    cells(a, PK_X, y, row_w(3, 100), 64, "cqr", 3, pad=4)
    tx = PK_X + row_w(3, 100) + 8
    cells(a, tx, y, row_w(2, 172), 64, "cqy", 2, pad=4)       # "TYPE · as knobs", THIS HIT ONLY
    ox = tx + row_w(2, 172) + 12
    name(a, ox, y + 32, "OCTAVE")
    cells(a, ox + 86, y, row_w(5, 64), 64, "cqo", 5, pad=2)


def bass_picker(a):
    """0.24 the BASS editor's picker: NOTE, the two-row piano (uqk_: "D 5", the degree on the step's chord), OCTAVE 1-4 (uqo_), FOLLOW CHORD
    (uqy_1) right of it"""
    rx = RI_X1 - PE_RC_W
    kw = (rx - 16 - BE_KX0 - 6 * 6) // 7
    name(a, RI_X0, PE_KY + 67, "NOTE")
    for i, white in enumerate((0, 1, 3, 4, 5)):   # C#, D#, F#, G#, A#: over the gap right of C, D, F, G, A
        tap_cell(a, BE_KX0 + (white + 1) * (kw + 6) - 3 - kw // 2, PE_KY, kw, "uqk_%d" % (i + 1))
    for i in range(7):
        tap_cell(a, BE_KX0 + i * (kw + 6), PE_KY + 70, kw, "uqk_%d" % (i + 6))
    name(a, rx, PE_KY + 32, "OCTAVE")
    cells(a, rx + 96, PE_KY, PE_RC_W - 96, 64, "uqo", 4, pad=4)
    cells(a, rx, PE_KY + 70, PE_RC_W, 64, "uqy", 1, pad=6)     # FOLLOW CHORD


RCX_W = 92                      # COPY TO...'s ALL, NONE
RCB_W, RCP_H, RCP_GAP = 301, 34, 38     # COPY TO...'s bar cells (4 x 4), and each one's inner cell (its chords and rhythm), a list on the
# same pitch (271 + 38 = 301 + 8 across, 34 + 38 = 64 + 8 down), so its cards are shared (R31)
PC_TW = 204                     # COPY TO...'s title ("COPY BAR 16 TO..." at 24 px)
PC_CW = (236, 240, 100)          # SAME IN EVERY PHRASE, COPY: RHYTHM + CHORDS, CANCEL (then GO to the right edge)
PC_NW = 560                     # its note line, then the message line, under the bars


def phrase_copy(a, P):
    """0.24 COPY TO... (cview / bview Copy; ChPhrCopy, BassPhrCopy, laid out as 0.18's rhythm_copy): its title (P.p + "ctl", "COPY BAR 6
    TO..."), ALL, NONE, SAME IN EVERY PHRASE, COPY: RHYTHM ONLY / + CHORDS (+ NOTES), CANCEL, "COPY TO 3 BARS" (P.p + "x_1..6"); the loop's
    bars (4 rows of 4: "BAR 6 · FROM", "· LOCKED", "· PICKED" lit), each with its chords and rhythm in an inner cell (P.p + "i_": "C·G@9 ·
    x---x---x---x---", ruling 22: a text picture, not dots); its note (P.p + "cn") and the message line under them"""
    p = P.p
    art(a, L, CM_Y, R - L, CM_H, "%s_%dx%d.png" % (P.panel, R - L, CM_H))
    ty = CM_Y + 6
    readout(a, RI_X0, ty + 32, PC_TW, p + "ctl", colour=P.ink, size=TX)
    x = RI_X0 + PC_TW + 8
    cells(a, x, ty, row_w(2, RCX_W), 64, p + "x", 2, pad=5)                  # ALL, NONE
    x += row_w(2, RCX_W) + 8
    for i, w in enumerate(PC_CW + (RI_X1 - x - sum(PC_CW) - 3 * 8,)):          # cells a list can't start
        tap_cell(a, x, ty, w, "%sx_%d" % (p, i + 3))
        x += w + 8
    cells(a, RI_X0, ty + 72, row_w(4, RCB_W), 4 * 64 + 3 * 8, p + "c", 4, rows=4, pad=6, band=(2, 24))   # "BAR 15 · LOCKED"
    ww = RCB_W + 8 - RCP_GAP
    cells(a, RI_X0 + (RCB_W - ww) // 2, ty + 72 + 64 - 4 - RCP_H, row_w(4, ww, RCP_GAP), 4 * RCP_H + 3 * RCP_GAP, p + "i", 4,
          rows=4, gap=RCP_GAP, pad=4, img="img=images/well_%dx%d_off.png img_on=images/well_%dx%d_on.png" % (ww, RCP_H, ww, RCP_H))
    readout(a, RI_X0, ty + 368, PC_NW, p + "cn", h=24, colour=INK)
    readout(a, RI_X0 + PC_NW + 8, ty + 368, RI_X1 - RI_X0 - PC_NW - 8, p + "msg", h=24, colour="f2a33a")


def chords_keys(a, p="ky", lit=""):
    """KEYS (VST-11): 5 octaves from C1 (MIDI 24-83), display only, lit = a note the Chords lane plays, the edge boxes lit for a
    note past either end. The white keys are a list; a list's cells are evenly spaced, so the black keys are image buttons
    over the gaps between their white keys (a tap is ignored: kyb_ are display cells). 0.19 p: the key prefix (ky the Chords
    lane's, uk the Bass lane's), lit: the lit pictures' prefix ("" amber, "u" the Bass lane's colour)"""
    a("list x=%d y=%d w=%d h=%d key=%se cols=2 rows=1 th=%d gap=%d tap=no img=images/key_ke_off.png img_on=images/key_%ske_on.png "
      "tsize=%d tcolor=%s talign=center tx=0 ttw=%d" % (L, CK_Y, R - L, CK_H, p, CK_H, R - L - 2 * KE_W, lit, TX, DIM, KE_W))
    a("list x=%d y=%d w=%d h=%d key=%sw cols=35 rows=1 th=%d gap=%d tap=no img=images/key_kw_off.png img_on=images/key_%skw_on.png "
      "tcolor=none" % (KW_X, CK_Y, 35 * KW_W + 34 * KW_GAP, CK_H, p, CK_H, KW_GAP, lit))
    n = 0
    for octave in range(5):
        for white in (0, 1, 3, 4, 5):   # C#, D#, F#, G#, A#: right of C, D, F, G, A
            n += 1
            k = 7 * octave + white
            cx = KW_X + (k + 1) * (KW_W + KW_GAP) - KW_GAP // 2
            button(a, cx - KB_W // 2, CK_Y, KB_W, KB_H, "%sb_%d" % (p, n), "key_kb_off.png", "key_%skb_on.png" % lit)


def browser_panel(a, done):
    """A browser's panel across the main area (0.24: it covers the left panel, as BASS's STYLE browser) and its title row's actions at the
    right: done, the keys of its 64 px cells and their widths, right to left. -> the title row's free width from the left"""
    art(a, L, CM_Y, R - L, CM_H, "panel_%dx%d.png" % (R - L, CM_H))
    x = R - 10
    for key, w in done:
        x -= w
        cells(a, x, TITLE_Y, w, 64, key, 1, pad=6)
        x -= 8
    return x - (L + 16)


def chords_progressions(a):
    """The PROGRESSIONS browser: its title (what it shows), CANCEL, DONE; the 16 presets in rows by feel"""
    tw = browser_panel(a, (("bdo", 130), ("bcn", 130)))
    readout(a, L + 16, TITLE_Y + 32, tw, "btl", colour=INK)
    x0 = L + 108
    for i, label in enumerate(PROG_ROWS):
        a('text cx=%d cy=%d label="%s" align=left size=%.1f weight=600 spacing=1 color=%s' % (L + 16, TITLE_Y + 72 + 72 * i + 32, label, NX, DIM))
    cells(a, x0, TITLE_Y + 72, R - 10 - x0, 4 * 64 + 3 * 8, "bpr", 4, rows=4, pad=4)   # BPR_CHARS (19) at 24 px


def chords_voicing(a):
    """The VOICING browser: its title, CANCEL, DONE; the 13 genres (5 a row: bgn_14 and bgn_15 are blank), the picked genre's
    4 styles and their feel"""
    tw = browser_panel(a, (("bdo", 130), ("bcn", 130)))
    readout(a, L + 16, TITLE_Y + 32, tw, "btl", colour=INK)
    x0, w = L + 10, R - L - 20
    cells(a, x0, TITLE_Y + 70, w, 3 * 64 + 2 * 6, "bgn", 5, rows=3, gap=6, pad=4)
    sy = TITLE_Y + 70 + 3 * 64 + 2 * 6 + 8
    cells(a, x0, sy, w, 64, "bvs", 4, pad=6)
    tw = (w - 3 * 8) // 4
    for i in range(4):
        readout(a, x0 + i * (tw + 8), sy + 76, tw, "bvf_%d" % (i + 1), h=24, colour=DIM, align="center")


# 0.19 a RHYTHMS browser's keys, so one function draws both: p its cells' and readouts' prefix (r the CHORDS tab's, y the BASS tab's), its
# title, CANCEL and DONE (bview's are shared with the tab's other browser). 0.24 PHRASE's RHYTHM field opens it
RPage = collections.namedtuple("RPage", "p btl bcn bdo")
RP_CHORDS = RPage("r", "btl", "bcn", "bdo")
RP_BASS = RPage("y", "ubtl", "ubc", "ubd")


def tap_cell(a, x, y, w, key):
    """A 64 px cell whose key a list can't start (a list's cells start at _1): its tile baked, its text a readout under a hit
    plate (lit: the amber plate)"""
    TILES.add((w, 64))
    art(a, x, y, w, 64, "tile_off_%dx64.png" % w)
    readout(a, x, y + 32, w, key, h=30, colour=INK, align="center", size=TX, pad=5)
    hit_plate(a, x, y, w, 64, key)


def rhythm_browser(a, P):
    """The RHYTHMS browser: btl (what a pick fills), rnote, CANCEL, DONE; the 6 feels; the feel's 4 rhythms, each with its
    picture and feel under it (as VOICING's styles)"""
    art(a, L, CM_Y, R - L, CM_H, "rpanel_%dx%d.png" % (R - L, CM_H))
    ty = CM_Y + 6
    readout(a, RI_X0, ty + 32, 440, P.btl, colour=INK)                       # "RHYTHMS · Garage Skip · every unlocked bar"
    bx = RI_X1 - row_w(2, 130)
    readout(a, RI_X0 + 448, ty + 32, bx - 8 - RI_X0 - 448, P.p + "note")
    for i, key in enumerate((P.bcn, P.bdo)):
        cells(a, bx + i * 138, ty, 130, 64, key, 1, pad=6)
    w = RI_X1 - RI_X0
    cells(a, RI_X0, ty + 76, w, 64, P.p + "bg", 6, pad=6)
    cells(a, RI_X0, ty + 156, w, 112, P.p + "bt", 4, pad=6)                  # "Trance Gate · 16 hits"
    tw = (w - 3 * 8) // 4
    for i in range(4):                                                         # "the & of 4 pushes into the bar" at 22 px
        x = RI_X0 + i * (tw + 8) - 3
        readout(a, x, ty + 290, tw + 6, "%sbp_%d" % (P.p, i + 1), h=30, colour=LIGHT, align="center", size=26)
        readout(a, x, ty + 320, tw + 6, "%sbf_%d" % (P.p, i + 1), h=24, colour="c9a36a", align="center", pad=4)


CST_W = 4 * 152 + 3 * 8                 # the strip's 4 bar cells: src/engine.c CST_CHARS (13) at 24 px


def strip_frame(a, pg, page, und, btp, sub=None):
    """Right of the strip's cells: the page arrows (pg_1..2), 64 px, with "BARS 13-16 · 4/4" between them (page: 172 px at 22 px),
    UNDO (und_1), BACK TO PLAYING (btp_1). 0.20 sub: a second line under page ("phrase 1 of 4")"""
    px = L + CST_W + 8
    page_w = 172
    cy = CS_Y + CS_H // 2
    cells(a, px, CS_Y, 2 * 64 + page_w + 16, CS_H, pg, 2, gap=page_w + 16, pad=4)
    a("readout cx=%d cy=%d w=%d h=28 label=\"\" key=%s box=0 tsize=%d tcolor=%s talign=center tpad=4"
      % (px + 72 + page_w // 2, cy - 12 if sub else cy, page_w, page, RX, INK))
    if sub:
        readout(a, px + 72, cy + 14, page_w, sub, h=24, colour=DIM, align="center", pad=4)
    ux = px + 2 * 64 + page_w + 24
    cells(a, ux, CS_Y, 96, CS_H, und, 1, pad=6)
    cells(a, ux + 104, CS_Y, R - ux - 104, CS_H, btp, 1, pad=4)


UTI_TW = 390                    # a STYLE tile's text ("Pedal Drone · PEDAL E · one low...": src/engine.c UTI_CHARS 32 at 24 px)
UTP_W = 170                     # its picture right of it ("1-5-1-o-........" at 22 px)
UTI_TB = (4, 32)                # the tile's top band (ty, tth): its text and picture; its summary (uts_, 22 px) in the 24 px under
                                # its text, as wide (38 characters at most: "moves lots · oct a little · scale step")


def bass_styles(a):
    """The STYLE browser (bview Style), across the main area (text floors: 6 genres a row and 32-character tiles need the width;
    the left panel's STYLE, PEDAL and OCTAVE wait under it): ubtl (what the style shown sets), CANCEL, DONE; the 12 genres (ugn_,
    2 rows of 6); the genre's 4 styles (uti_, 2 x 2), each with its picture of the shown bar (utp_) right of its text and its summary
    (uts_, "moves some · oct some · half below") under both, each under a hit plate that taps the tile; the message line (umsg)"""
    art(a, L, CM_Y, R - L, CM_H, "rpanel_%dx%d.png" % (R - L, CM_H))
    ty = CM_Y + 6
    bx = RI_X1 - row_w(2, 130)
    readout(a, RI_X0, ty + 32, bx - 8 - RI_X0, "ubtl", colour=INK)          # "STYLES · moves some · oct some · half below"
    for i, key in enumerate(("ubc", "ubd")):
        cells(a, bx + i * 138, ty, 130, 64, key, 1, pad=6)
    w = RI_X1 - RI_X0
    cells(a, RI_X0, ty + 74, w, 2 * 64 + 6, "ugn", 6, rows=2, gap=6, pad=6)   # "Film / Ambient" at 24 px
    tw, y = (w - 8) // 2, ty + 212
    a("list x=%d y=%d w=%d h=%d key=uti cols=2 rows=2 th=64 gap=8 %s tsize=%d tcolor=%s talign=left tx=14 ttw=%d ty=%d tth=%d"
      % ((RI_X0, y, w, 2 * 64 + 8, tiles(tw, 64), TX, INK, UTI_TW) + UTI_TB))
    for i in range(4):
        tx, ty0 = RI_X0 + (i % 2) * (tw + 8), y + (i // 2) * 72
        x, cy = tx + tw - UTP_W - 12, ty0 + UTI_TB[0] + UTI_TB[1] // 2
        readout(a, x, cy, UTP_W, "utp_%d" % (i + 1), h=28, colour=LIGHT, align="center", pad=4)
        hit_plate(a, x, cy - 14, UTP_W, 28, "uti_%d" % (i + 1), lit=False)
        sy = ty0 + sum(UTI_TB)                                                   # the summary's 24 px, the tile's lower band
        readout(a, tx + 6, sy + 12, UTI_TW + 16, "uts_%d" % (i + 1), h=24, colour="c9c5bd", pad=8)
        hit_plate(a, tx + 6, sy, UTI_TW + 16, 24, "uti_%d" % (i + 1), lit=False)
    readout(a, RI_X0, CM_Y + CM_H - 14, w, "umsg", h=24, colour="f2a33a")


# 0.20 the MELODY tab (VST-27): the JAM page and the PHRASE page (with its ANSWER BLOCK picker and bar editor) on the BASS tab's frame
# (header, strip CS_*, main area CM_*, KEYS CK_*; layout y = plugin y + 86). The skin's groups follow mgroup (engine-set: Jam, Phrase on
# PHRASE and its picker, Edit) and the views mview (Jam, Phrase, Pick, Edit). Text floors move four things from the boards (MelR2*):
# a block's cell ("RUN-DOWN  NEXT", 14 characters at 24 px) needs 156 px, so the JAM grid takes 966 px and its 2 x 4 knob panel the
# 274 px right of it (knob values: JAM's texts, at most 13 characters: "ARP · UP-DOWN"); JAM's phrase bars ("BAR 15 / BREAK UP") and
# SWITCH AT are single lines across the status row, NEXT over the message line under the grid; on PHRASE the left panel
# holds the ANSWER line ("SWAP: another JAM block answers (the call keeps its own)" at 22 px) and the ring the knobs' "LOOP: no
# answers" (158 px a side column), so the centre's end line runs under the ring and the JAM knobs' summary (wsum) and the message line
# share a line under both panels; the picker's blocks ("SEQUENCE  ANSWER", 16 characters) take the main area's width, its title,
# CANCEL and DONE a column at the right and its note (wpnt) the line under them. The ring is in reading order (1 2 3 / 4 centre 5 /
# 6 7 8) as BASS's, not the boards' clockwise ring.
MEL_INK = hexc(LANES[2][1])     # the MELODY lane's colour (3cc6a8)
JAM_COLS = ["ARP", "OSTINATO", "HOOK", "LEAD", "RUN", "PEDAL"]   # src/melody2.c's columns, in order (the cells' c)
JB_W, JB_GAP, JB_TB = 156, 6, (2, 30)   # a JAM block: 156 x 64, 6 apart; its text band (ty, tth), its picture under it
JG_W = row_w(6, JB_W, JB_GAP)   # the JAM grid (x 16-982)
JG_H = row_w(5, 64, JB_GAP)
JS_Y = CS_Y                     # JAM's status row (plugin y 80-144): NOW PLAYING, the phrase's 4 bars, SWITCH AT
JNOW_W, JPH_W = 200, 188        # "OSTINATO · OCTAVE" at 22 px; "BAR 15 / BREAK UP" at 24 px
JG_NY = JS_Y + 74               # the column names' centre line
JG_Y = JG_NY + 10               # the blocks (plugin y 164-508), then NEXT (jnext) over the message line (jmsg)
JK_X, JK_Y = L + JG_W + 8, JS_Y + 72   # the knob panel (x 990-1264, plugin y 152-556): 2 x 4
JK_W, JK_H = R - JK_X, CM_Y + CM_H - JK_Y
JK_BW, JK_PITCH = 129, JK_H // 4   # a knob's box ("ARP · UP-DOWN" at 22 px), a row
JK_NDX = 10                     # the left column's names sit 10 px right of their knobs: "1 COMPLEXITY" (about 141 px at 20 px on the
                                # Force) centred on its knob crossed the panel's left border (0.20.0 device test)
JAM_KNOBS = [("1 COMPLEXITY", "cx_3"), ("2 DENSITY", "k_j2"), ("3 RANGE", "k_j3"), ("4 OCTAVE", "k_j4"), ("5 VARIATION", "k_j5"),
             ("6 SWING", "k_j6"), ("7 GATE", "k_j7"), ("8 TIME", "k_j8")]   # Force knobs 1-8 (the MELODY qlinks; 0.21 all slots)
# 0.22 a JAM page's keys (NOW PLAYING, the phrase's bars, SWITCH AT, the blocks, NEXT, the message line), NOW PLAYING's width (its box
# images/<now>_<w>x64.png), its grid picture's name, column names, lane colour and knobs; meters: (the bars' meters, the blocks' meters,
# the blocks' filmstrip) or None (MELODY's cells light)
JamPage = collections.namedtuple("JamPage", "now now_w bars sw blocks nxt msg grid cols ink knobs meters")
JP_MEL = JamPage("jnow", JNOW_W, "jph", "jsw", "jbk", "jnext", "jmsg", "jgrid", JAM_COLS, MEL_INK, JAM_KNOBS, None)
# PHRASE: the left panel (x 16-594), the ring's panel (x 602-1264), both PP_H tall, then one line under both (wsum, wmsg)
PL_W, PP_H = 578, 356
PR_X = L + PL_W + 8
PR_W = R - PR_X
PRS_W, PR_GAP = 164, 6          # the ring's side columns ("ARP · UP-DOWN" at 22 px), the gap between columns
PRM_W = PR_W - 12 - 2 * PRS_W - 2 * PR_GAP   # its middle column (310): the centre's role line, its 16 dots
PR_Y0, PR_H, PR_PITCH = CM_Y + 26, 94, 99    # its rows, under the where-line (wcwh), over the end line (wcend)
PSW_W = PL_W - 12 - 2 * 72      # ANSWER PLAYS' field, between its arrows
WSUM_W = 612                    # wsum (61 characters at 22 px), then the message line, under both panels
MEL_RING = [("1 COMPLEXITY", "cx_3", 0), ("2 ANSWER", "k_j2", 1), ("3 STRAY", "k_j3", 2), ("4 ANSWER OCT", "k_j4", 3), ("5 CADENCE", "k_j5", 5),
            ("6 BREATH", "k_j6", 6), ("7 FILL", "k_j7", 7), ("8 PUSH", "k_j8", 8)]   # ring cell = row * 3 + column; 4 the centre
LEGEND = [("chord tone", 0, 92), ("warm", 1, 49), ("colourful", 2, 77), ("tense", 3, 47), ("clash", 4, 44)]   # the dots' colours
# (spec: bands 0-4), each word's width at 20 px (Titillium Web 400, measured)
PB_W = 177                      # a picker block: "SEQUENCE  ANSWER" (16 characters) at 24 px
PG_W = row_w(6, PB_W, JB_GAP)   # the picker's grid (x 16-1108), its column at the right (title, CANCEL, DONE)
PG_NY, PG_Y = CM_Y + 10, CM_Y + 20
PC_X = L + PG_W + 8
# the bar editor (mview Edit), across the main area as BASS's: the title row, the open bar's 16 dots, the beat labels, the steps, the piano
# (BE_KX0, BE_KW: BASS's) with OCTAVE (3-7) and REST, HOLD at the right, the message line
ME_DOT_Y = CM_Y + 74
ME_STEP_Y = CM_Y + 140
ME_KEY_Y = ME_STEP_Y + 72
ME_RC_W, ME_OCT_X = 460, 98     # the right column: OCTAVE (its name, then 5 cells of 66) over REST, HOLD
ME_KW = (RI_X1 - ME_RC_W - 16 - BE_KX0 - 6 * 6) // 7   # a piano key (91 px)
# the step dots (toolchain meters over 73-frame filmstrips, as the DOTS probe: probe/dots/make_dots.py, RESULTS.md): the centre's and
# the editor's (dots_c), the strip's (dots_s)
DOT_FRAMES, DOT_LEVELS, DOT_COLOURS = 73, 12, 5
DOT_TENSION = [(0x3f, 0xbf, 0x6f), (0x9c, 0xcf, 0x4f), (0xe3, 0xb2, 0x3c), (0xe8, 0x83, 0x3a), (0xd9, 0x53, 0x4f)]
DOT_HOLD, DOT_BASE = (0x2f, 0x7f, 0x6c), (0x2c, 0x30, 0x38)
DOTS_C, DOTS_S = (16, 46, 6), (8, 46, 5)   # (w, h, the dot's height): a centre / editor dot, a strip dot (frame h <= 168)


def dot_strip(w, h, dot_h, name):
    """A dot filmstrip of DOT_FRAMES frames of w x h, top to bottom, exactly as probe/dots/make_dots.py strip() draws it: 0 a rest
    (the step's baseline), 1 + level x 5 + colour a note, 61 + level a held step"""
    img = Image.new("RGBA", (w, h * DOT_FRAMES), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = max(2, dot_h // 2)
    span = h - 2 * pad - dot_h

    def y_of(level):   # level 0 = lowest note
        return pad + span - round(span * level / (DOT_LEVELS - 1))

    for f in range(DOT_FRAMES):
        top = f * h
        d.rectangle([0, top + h - 3, w - 1, top + h - 2], fill=DOT_BASE)   # the step's baseline, on every frame
        if 1 <= f <= DOT_LEVELS * DOT_COLOURS:
            level, col = divmod(f - 1, DOT_COLOURS)
            y = top + y_of(level)
            d.rounded_rectangle([1, y, w - 2, y + dot_h], radius=max(1, dot_h // 3), fill=DOT_TENSION[col])
        elif f > DOT_LEVELS * DOT_COLOURS:
            level = f - 1 - DOT_LEVELS * DOT_COLOURS
            y = top + y_of(level) + dot_h // 2 - 1
            d.rectangle([0, y, w - 1, y + max(2, dot_h // 3)], fill=DOT_HOLD)
    save(img, name)


def pictures(name):
    """The JAM blocks' pictures (vst/<name>): a line of 16 frames per block, column by column"""
    return [[int(f) for f in ln.split()] for ln in open(os.path.join(HERE, name)) if ln.strip()]


def jam_grid(bw, name, pics=None, colour=LANES[2][1], chords=False):
    """The 30 blocks' frames and fixed pictures for a grid of bw x 64 blocks (6 columns of 5, JB_GAP apart, column by column as jbk_
    and wpk_): each block's 16 dots on Em (vst/jam_pictures.txt, the engine's own table: tests/test_melody2.c pins it) in the MELODY
    colour under its text band, a note's dot at its height, a held step a line. The cells over it are see-through (melody_blocks).
    0.22 pics, colour: the CHORDS and BASS blocks' (vst/jam_pictures_cb.txt, tests/test_jamlib.c pins it) in the lane's colour; BASS's
    frames are MELODY's (jamlib's height window), chords: CHORDS's (0 rest, 1 a hit: 3 stacked dots, 2 held: 3 lines)"""
    pics = pics or pictures("jam_pictures.txt")
    im = Image.new("RGB", (row_w(6, bw, JB_GAP), JG_H), (15, 16, 18))
    d = ImageDraw.Draw(im)
    x0, y0, y1 = 6, JB_TB[0] + JB_TB[1] + 2, 64 - 9   # the picture's box in a block: its left, the top rows of its highest and
    sw = (bw - 2 * x0) / 16.0                         # lowest dots
    for n, frames in enumerate(pics):
        bx, by = (n // 5) * (bw + JB_GAP), (n % 5) * (64 + JB_GAP)
        d.rectangle([bx, by, bx + bw - 1, by + 63], fill=TILE_OFF, outline=CELL_LINE)
        for i, f in enumerate(frames):
            sx = bx + x0 + i * sw
            if chords:   # a chord: 3 dots at the box's top, middle and bottom, a held step 3 lines
                for y in (by + y0, by + (y0 + y1) // 2, by + y1):
                    if f == 2:
                        d.rectangle([round(sx) - 1, y + 1, round(sx + sw), y + 2], fill=colour)
                    elif f:
                        d.rounded_rectangle([round(sx) + 1, y, round(sx + sw) - 1, y + 3], radius=1, fill=colour)
            elif f > DOT_LEVELS * DOT_COLOURS:   # held: a line at the held note's height
                y = by + y1 - round((y1 - y0) * (f - 1 - DOT_LEVELS * DOT_COLOURS) / (DOT_LEVELS - 1)) + 1
                d.rectangle([round(sx) - 1, y, round(sx + sw), y + 1], fill=colour)
            elif f:
                y = by + y1 - round((y1 - y0) * ((f - 1) // DOT_COLOURS) / (DOT_LEVELS - 1))
                d.rounded_rectangle([round(sx) + 1, y, round(sx + sw) - 1, y + 3], radius=1, fill=colour)
    save(im, name)


def melody_images(panel, field):
    """0.20 the MELODY tab's pictures: KEYS lit in its colour, the JAM and picker grids' blocks, NOW PLAYING's box, the panels, ANSWER
    PLAYS' field, the centre's box, the picker's column, the dots' legend swatches and the two dot filmstrips"""
    for name_, (w, h) in (("kw", (KW_W, CK_H)), ("kb", (KB_W, KB_H)), ("ke", (KE_W, CK_H))):
        save(tile(w, h, LANES[2][1]), "key_j%s_on.png" % name_)
    jam_grid(JB_W, "jgrid_%dx%d.png" % (JG_W, JG_H))
    jam_grid(PB_W, "jgrid_%dx%d.png" % (PG_W, JG_H))
    save(rounded(JNOW_W, 64, (20, 58, 51), CELL_LINE), "jnow_%dx64.png" % JNOW_W)
    for w, h in ((JK_W, JK_H), (PL_W, PP_H), (PR_W, PP_H)):
        save(rounded(w, h, panel, CELL_LINE, radius=10), "panel_%dx%d.png" % (w, h))
    save(rounded(PSW_W, 64, field, CELL_LINE, page=panel), "mfield_%dx64.png" % PSW_W)
    save(rounded(PRM_W, PR_H, field, CELL_LINE, radius=12, page=panel), "mcentre_%dx%d.png" % (PRM_W, PR_H))
    save(rounded(R - PC_X, CM_H, panel, LANES[2][1], 2, radius=10), "mpick_%dx%d.png" % (R - PC_X, CM_H))
    save(rounded(R - L, CM_H, panel, LANES[2][1], 2, radius=10), "mpanel_%dx%d.png" % (R - L, CM_H))
    for _, band, _ in LEGEND:
        save(rounded(12, 12, DOT_TENSION[band], radius=3, page=panel), "mdot_%d.png" % band)
    dot_strip(DOTS_C[0], DOTS_C[1], DOTS_C[2], "dots_c.png")
    dot_strip(DOTS_S[0], DOTS_S[1], DOTS_S[2], "dots_s.png")


def dots(a, x, cy, pitch, key, n, size, strip, frames=DOT_FRAMES):
    """n step dots (meters on the filmstrip strip): key the first's ("zs_17"), the others numbered on; x the first's left, pitch apart.
    0.24 frames: the strip's frame count (CHORDS and BASS PHRASE's own)"""
    prefix, first = key.rsplit("_", 1)
    for i in range(n):
        a("meter cx=%d cy=%d w=%d h=%d key=%s_%d strip=images/%s frames=%d"
          % (x + i * pitch + size[0] // 2, cy, size[0], size[1], prefix, int(first) + i, strip, frames))


def melody_blocks(a, x, y, bw, key, grid="jgrid", meter=None):
    """The 30 blocks over their pictures (jam_grid, baked): a list of see-through cells column by column (order=cols: key_1..5 the first
    column's levels), its text in the top band. Its tiles are the toolchain's own (no img=), so they keep their alpha: the off tile's fill
    is clear (alpha 1, so MPC keeps its touch) and the picture under it shows; the lit tile is the theme's tile_on, a translucent teal
    gel, framed in theme_seg_active (vst/layout.theme: only these lists use them). 0.22 grid: the picture's name; meter: (key, strip),
    a meter under each cell (CHORDS and BASS JAM: the cells never light, the meters show playing and NEXT)"""
    art(a, x, y, row_w(6, bw, JB_GAP), JG_H, "%s_%dx%d.png" % (grid, row_w(6, bw, JB_GAP), JG_H))
    for i in (range(30) if meter else ()):
        jam_meter(a, x + (i // 5) * (bw + JB_GAP), y + (i % 5) * (64 + JB_GAP), bw, "%s_%d" % (meter[0], i + 1), meter[1], 3)
    a("list x=%d y=%d w=%d h=%d key=%s cols=6 rows=5 th=64 gap=%d order=cols color=00000001 tsize=%d tcolor=%s talign=left tx=2 ttw=%d "
      "ty=%d tth=%d" % ((x, y, row_w(6, bw, JB_GAP), JG_H, key, JB_GAP, TX, INK, bw - 4) + JB_TB))


def jam_meter(a, x, y, w, key, strip, frames):
    """0.22 a w x 64 meter (frames of a filmstrip, the engine's whole number picks one) from (x, y), under a see-through cell"""
    a("meter cx=%d cy=%d w=%d h=64 key=%s strip=images/%s frames=%d" % (x + w // 2, y + 32, w, key, strip, frames))


def melody_tab(a):
    """The MELODY tab (0.20): the header and KEYS on every view; JAM (mgroup Jam); the PHRASE strip on PHRASE and its picker (mgroup
    Phrase) and with the editor (mview Edit: one line takes one when=); PHRASE's panels, the picker and the editor on mview Phrase, Pick,
    Edit. A mode image spans its lines' baked parts, a readout's counted with 34 px above it (shadow_skin.py baked_rect), so the
    header's role line hangs on mview Jam, a mode of its own, not on the JAM page's. No two groups drawn at once overlap, so their order
    is free. No stepper (check_stepper_arrows)"""
    title = LANES[2][0]
    melody_header(a)
    chords_keys(a, "jk", "j")
    jam_page(when(a, "mgroup:Jam"), JP_MEL)
    melody_strip(when(a, "mgroup:Phrase"))
    melody_phrase(when(a, "mview:Phrase"))
    melody_pick(when(a, "mview:Pick"))
    e = when(a, "mview:Edit")
    melody_strip(e)
    melody_editor(e)
    a('qlinks "%s" = %s' % (title, ",".join(k for _, k in JAM_KNOBS)))


def melody_header(a):
    """The tab's title, JAM / PHRASE (jmd_), the bar counter (jcnt) over the bar's role (jrole, JAM) or FOLLOW (wfol), PLAY's part cells"""
    title, colour = LANES[2]
    a("[tab %s]" % title)
    header(a, title, hexc(colour))
    cells(a, MD_X, HEAD_Y, row_w(2, 100), 64, "jmd", 2, pad=3)
    pt_w = 84
    rx = MD_X + row_w(2, 100) + 16
    rw = R - row_w(6, pt_w) - 8 - rx
    readout(a, rx, HEAD_Y + 16, rw, "jcnt", colour="f2a33a", pad=4)          # "> BAR 15 / 16 · F#dim"
    readout(when(a, "mview:Jam"), rx, HEAD_Y + 48, rw, "jrole", colour=DIM, pad=4)
    for g in ("Phrase", "Edit"):
        readout(when(a, "mgroup:" + g), rx, HEAD_Y + 48, rw, "wfol", colour=DIM, pad=4)
    cells(a, R - row_w(6, pt_w), HEAD_Y, row_w(6, pt_w), 64, "pt", 6, pad=4)


def jam_page(a, P):
    """JAM: the status row (NOW PLAYING over jnow, the playing phrase's bars jph_, SWITCH AT jsw_1), the 6 column names and the 30 blocks
    (jbk_) over their pictures, NEXT (jnext) over the message line (jmsg) under them, the 8 knobs in a 2 x 4 panel at the right. 0.22 P: a
    JamPage, MELODY's (JP_MEL) or the CHORDS and BASS tabs' (JP_CHORDS, JP_BASS: their bars and blocks see-through over meters)"""
    art(a, L, JS_Y, P.now_w, 64, "%s_%dx64.png" % (P.now, P.now_w))
    name(a, L + 12, JS_Y + 18, "NOW PLAYING")
    readout(a, L, JS_Y + 44, P.now_w, P.now, h=28, colour=P.ink, pad=6)
    px = L + P.now_w + 8
    if P.meters:   # 0.22 the bars over their meters (plain, playing, locked, both): never lit, their text over the meter's LOCKED
        for i in range(4):
            jam_meter(a, px + i * (JPH_W + 8), JS_Y, JPH_W, "%s_%d" % (P.meters[0], i + 1), "jbar_%dx64.png" % JPH_W, 4)
        a("list x=%d y=%d w=%d h=64 key=%s cols=4 rows=1 th=64 gap=8 color=00000001 tsize=%d tcolor=%s talign=center tx=2 ttw=%d ty=%d "
          "tth=%d" % ((px, JS_Y, row_w(4, JPH_W), P.bars, TX, INK, JPH_W - 4) + JQ_TB))
    else:
        cells(a, px, JS_Y, row_w(4, JPH_W), 64, P.bars, 4, pad=2)
    sx = px + row_w(4, JPH_W) + 8
    cells(a, sx, JS_Y, R - sx, 64, P.sw, 1, pad=4)
    for c, label in enumerate(P.cols):
        name(a, L + c * (JB_W + JB_GAP) + JB_W // 2, JG_NY, label, INK, "center")
    melody_blocks(a, L, JG_Y, JB_W, P.blocks, P.grid, P.meters and P.meters[1:])
    ly = JG_Y + JG_H + 13
    readout(a, L, ly, JG_W, P.nxt, h=24, colour=INK, pad=4)      # "NEXT · OSTINATO · ROOT-5 · at bar 16"
    readout(a, L, ly + 24, JG_W, P.msg, h=24, colour="f2a33a", pad=4)
    art(a, JK_X, JK_Y, JK_W, JK_H, "panel_%dx%d.png" % (JK_W, JK_H))
    for i, (label, key) in enumerate(P.knobs):
        cx, y = JK_X + 4 + JK_BW // 2 + (i % 2) * (JK_BW + 8), JK_Y + (i // 2) * JK_PITCH
        name(a, cx + JK_NDX * (1 - i % 2), y + 14, label, INK, "center", spacing=1)
        knob(a, cx, y + 50, key, JK_BW)


def melody_strip(a):
    """The PHRASE strip: the page's 4 bars (wst_: "2 · ANSWER · own · G" in the top band), each with its 16 live dots (zs_) along its
    foot under a hit plate that opens the bar, the page arrows, BARS 1-4 over "phrase 1 of 4", UNDO, BACK TO PLAYING"""
    cells(a, L, CS_Y, CST_W, CS_H, "wst", 4, pad=5, band=(2, 28))
    for i in range(4):
        x = L + i * 160
        dots(a, x + 4, CS_Y + 30 + DOTS_S[1] // 2, 9, "zs_%d" % (i * 16 + 1), 16, DOTS_S, "dots_s.png")
        hit_plate(a, x, CS_Y + 30, 152, CS_H - 30, "wst_%d" % (i + 1), lit=False)
    strip_frame(a, "wpg", "wpage", "wund", "wbtp", sub="wpgn")


def melody_phrase(a):
    """PHRASE (mview Phrase): the left panel (FORM, its line, ANSWER, its line, ANSWER PLAYS: <, the SWAP block's field, a tap opens the
    picker, >; the dots' legend), the ring (the where-line wcwh, the 8 knobs around the centre: its role wcrl, its 16 dots zc_, a tap
    opens the editor (wct_1); where it ends, wcend, under the ring), then wsum and the message line (wmsg) under both"""
    art(a, L, CM_Y, PL_W, PP_H, "panel_%dx%d.png" % (PL_W, PP_H))
    x0, w0 = L + 6, PL_W - 12
    name(a, x0 + 4, CM_Y + 14, "FORM \u00b7 THE 4-BAR PHRASE")
    cells(a, x0, CM_Y + 26, w0, 64, "wfm", 6, pad=4)
    readout(a, x0, CM_Y + 104, w0, "wfmt", h=24, pad=2)
    name(a, x0 + 4, CM_Y + 128, "ANSWER \u00b7 HOW IT ANSWERS THE CALL")
    cells(a, x0, CM_Y + 140, w0, 64, "wan", 6, pad=4)
    readout(a, x0, CM_Y + 218, w0, "want", h=24, pad=2)
    y = CM_Y + 236
    for key, side, ax in (("wsw_1", "l", x0), ("wsw_3", "r", x0 + w0 - 64)):
        button(a, ax, y, 64, 64, key, "arrow64_%s_off.png" % side, "arrow64_%s_on.png" % side)
    fx = x0 + 72
    art(a, fx, y, PSW_W, 64, "mfield_%dx64.png" % PSW_W)
    name(a, fx + PSW_W // 2, y + 18, "ANSWER PLAYS", align="center")
    readout(a, fx, y + 44, PSW_W, "wsw_2", h=26, colour=INK, align="center", pad=6)   # "OSTINATO · OCTAVE", "pick SWAP first"
    hit_plate(a, fx, y, PSW_W, 64, "wsw_2")
    x, ly = x0 + 4, CM_Y + 332                    # the dots' legend
    name(a, x, ly, "DOTS")                        # 56 px
    x += 72
    for word, band, ww in LEGEND:
        art(a, x, ly - 6, 12, 12, "mdot_%d.png" % band)
        hint(a, x + 18, ly, word, "c9c5bd")
        x += 18 + ww + 18
    melody_ring(a)
    readout(a, L, CM_Y + PP_H + 16, WSUM_W, "wsum", h=24, pad=4)    # "On JAM, knobs 2-5: BREATH Half · ... · FILL STEP-IN."
    mx = L + WSUM_W + 8
    readout(a, mx, CM_Y + PP_H + 16, R - mx, "wmsg", h=24, colour="f2a33a", pad=4)


def mel_ring_cell(i):
    """(x, y, w) of the PHRASE ring's cell i (row-major 0..8)"""
    col, row = i % 3, i // 3
    x = PR_X + 6 + (0, PRS_W + PR_GAP, PRS_W + PRM_W + 2 * PR_GAP)[col]
    return x, PR_Y0 + row * PR_PITCH, PRM_W if col == 1 else PRS_W


def melody_ring(a):
    """PHRASE's ring: its panel, the where-line, the 8 knobs (their names baked) in reading order, the centre, where the bar ends"""
    art(a, PR_X, CM_Y, PR_W, PP_H, "panel_%dx%d.png" % (PR_W, PP_H))
    readout(a, PR_X + 6, CM_Y + 14, PR_W - 12, "wcwh", h=26, colour=DIM, align="center", pad=4)   # "PLAYING · BAR 2 · G · tap to change"
    for label, key, cell in MEL_RING:
        x, y, w = mel_ring_cell(cell)
        name(a, x + w // 2, y + 8, label, INK, "center")
        knob(a, x + w // 2, y + 46, key, min(w - 4, 200))   # its box: 87 px from cy - 29, 6 px into the next row
    x, y, w = mel_ring_cell(4)
    art(a, x, y, w, PR_H, "mcentre_%dx%d.png" % (w, PR_H))
    readout(a, x, y + 16, w, "wcrl", h=26, colour=LIGHT, align="center", pad=4)   # "ANSWER · SWAP: RUN · RUN-DOWN"
    dots(a, x + (w - 16 * 18 + 2) // 2, y + 30 + DOTS_C[1] // 2, 18, "zc_1", 16, DOTS_C, "dots_c.png")
    hit_plate(a, x, y + 8, w, PR_H - 8, "wct_1", lit=False)   # under knob 2's box
    readout(a, PR_X + 6, CM_Y + 334, PR_W - 12, "wcend", h=24, colour="c9c5bd", align="center", pad=4)   # "ends on E4 = 1 (home) · ..."


def melody_pick(a):
    """The ANSWER BLOCK picker (mview Pick): the 30 blocks (wpk_: "MOTIF  CALL" the call's own, "RUN-UP  ANSWER" lit) over their
    pictures with their column names, the column at the right (its title, CANCEL, DONE) and what a tap does (wpnt) under the blocks.
    The message line waits for PHRASE: a tap's effect shows in the lit block, the strip and KEYS"""
    for c, label in enumerate(JAM_COLS):
        name(a, L + c * (PB_W + JB_GAP) + PB_W // 2, PG_NY, label, INK, "center")
    melody_blocks(a, L, PG_Y, PB_W, "wpk")
    cw = R - PC_X
    art(a, PC_X, CM_Y, cw, CM_H, "mpick_%dx%d.png" % (cw, CM_H))
    for i, word in enumerate(("ANSWER", "BLOCK")):
        a('text cx=%d cy=%d label="%s" align=left size=2.4 weight=700 spacing=2 color=%s' % (PC_X + 12, CM_Y + 26 + 30 * i, word, LIGHT))
    for i, key in enumerate(("wpc", "wpd")):
        cells(a, PC_X + 8, CM_Y + CM_H - 152 + 76 * i, cw - 16, 64, key, 1, pad=6)
    readout(a, L, PG_Y + JG_H + 13, PG_W, "wpnt", h=24, colour=INK, pad=4)   # "Tap a block for the answers. The call keeps HOOK · PICKUP."


def melody_editor(a):
    """The bar editor (mview Edit), across the main area (as BASS's): its title (wett) over its note line (went), NEW BAR, RESET BAR,
    LOCK / UNLOCK, CANCEL, DONE (wea_); the open bar's 16 live dots (zc_) over the 16 steps (wes_: "B4* 7", "-" held, "·" a rest)
    under their beat labels (baked: 1 e & a); NOTE, a two-row piano (wnk_1..5 C# D# F# G# A# over the gaps of wnk_6..12 C..B: image
    buttons, tap_cell), OCTAVE 3-7 (wno_) and REST, HOLD (wrh_) at the right; the message line (wmsg). Its panel is framed in the
    MELODY colour (BASS's is amber)"""
    art(a, L, CM_Y, R - L, CM_H, "mpanel_%dx%d.png" % (R - L, CM_H))
    ty = CM_Y + 6
    ax = RI_X1 - row_w(5, 112)
    readout(a, RI_X0, ty + 20, ax - 8 - RI_X0, "wett", h=30, colour="f2a33a", size=TX)   # 51 characters at 24 px
    readout(a, RI_X0, ty + 48, ax - 8 - RI_X0, "went", h=24, colour=DIM)
    cells(a, ax, ty, row_w(5, 112), 64, "wea", 5, pad=6)
    sw = (RI_X1 - RI_X0 - 15 * 6) // 16
    for i in range(STEPS):
        x = RI_X0 + i * (sw + 6)
        dots(a, x + (sw - DOTS_C[0]) // 2, ME_DOT_Y + DOTS_C[1] // 2, 0, "zc_%d" % (i + 1), 1, DOTS_C, "dots_c.png")   # 1 a step
        a('text cx=%d cy=%d label="%s" align=center size=%.1f weight=%d case=none color=%s'
          % (x + sw // 2, ME_STEP_Y - 14, "1234"[i // 4] if i % 4 == 0 else "e&a"[i % 4 - 1], NX, 700 if i % 4 == 0 else 600,
             INK if i % 4 == 0 else "6b717a"))
    cells(a, RI_X0, ME_STEP_Y, RI_X1 - RI_X0, 64, "wes", STEPS, gap=6, pad=1)   # "C#7* 7" at 24 px
    name(a, RI_X0, ME_KEY_Y + 67, "NOTE")
    for i, white in enumerate((0, 1, 3, 4, 5)):   # C#, D#, F#, G#, A#: over the gap right of C, D, F, G, A
        tap_cell(a, BE_KX0 + (white + 1) * (ME_KW + 6) - 3 - ME_KW // 2, ME_KEY_Y, ME_KW, "wnk_%d" % (i + 1))
    for i in range(7):
        tap_cell(a, BE_KX0 + i * (ME_KW + 6), ME_KEY_Y + 70, ME_KW, "wnk_%d" % (i + 6))
    rx = RI_X1 - ME_RC_W
    name(a, rx, ME_KEY_Y + 32, "OCTAVE")
    cells(a, rx + ME_OCT_X, ME_KEY_Y, ME_RC_W - ME_OCT_X, 64, "wno", 5, pad=4)
    cells(a, rx, ME_KEY_Y + 70, ME_RC_W, 64, "wrh", 2, pad=6)               # REST, HOLD
    readout(a, RI_X0, ME_KEY_Y + 134 + 22, RI_X1 - RI_X0, "wmsg", h=28, colour="f2a33a")


# 0.22 (VST-29) the CHORDS and BASS tabs' JAM pages (spec "The JAM pages (skin)"): MELODY's JAM page (jam_page) in the lane's colour, on
# cview Jam and bview Jam, with the genres for columns and the spec's knob tables. The cells never light (the theme's teal gel would hide
# the lane's colour): each block and each phrase bar is a see-through cell over a meter the engine sets (cjm_ / ujm_ 0 plain, 1 playing,
# 2 NEXT; cjq_ / ujq_ 0 plain, 1 playing, 2 locked, 3 both). The page colour first covers what the tab draws on every view in the main
# area (the ring's panel and its baked names, CHORDS's centre drift frame: a mode image drawn later covers the cdrift ones)
JAM_GENRES = ["HOUSE", "TECHNO", "TRANCE", "POP", "ROCK", "DISCO"]   # src/jamlib.c's columns, in order (the cells' c)
JQ_TB = (4, 36)                 # a phrase bar's text band ("BAR 16 · F#maj7" at 24 px); its meter's LOCKED under it
CJNOW_W = 212                   # NOW PLAYING: "TECHNO · DUB STAB (edited)" shortened to 20 characters (src/engine.c DJNOW_CHARS) at 22 px
JQ_LOCKED = (0x2c, 0x24, 0x40)  # a locked bar (the approved mock's violet), its word's ink
JQ_WORD = (0xb9, 0xa8, 0xf0)
JP_CHORDS = JamPage("cjnow", CJNOW_W, "cjp", "cjsw", "cjb", "cjnext", "cjmsg", "cjgrid", JAM_GENRES, hexc(LANES[0][1]),
                    list(zip(("1 COMPLEXITY", "2 DENSITY", "3 COLOUR", "4 SPREAD", "5 VARIATION", "6 SWING", "7 GATE", "8 TIME"),
                             QLINKS_22[0])), ("cjq", "cjm", "cjblk_%dx64.png" % JB_W))
JP_BASS = JamPage("ujnow", CJNOW_W, "ujp", "ujsw", "ujb", "ujnext", "ujmsg", "ujgrid", JAM_GENRES, hexc(LANES[1][1]),
                  list(zip(("1 COMPLEXITY", "2 DENSITY", "3 MOVEMENT", "4 OCTAVES", "5 VARIATION", "6 SWING", "7 GATE", "8 TIME"),
                           QLINKS_22[1])), ("ujq", "ujm", "ujblk_%dx64.png" % JB_W))
# their NOW PLAYING boxes (the mock's fill and line) and their blocks' playing tint (alpha over the block's tile, so the picture in the
# lane's colour keeps its colour)
JAM_NOW_22 = {"cjnow": ((0x33, 0x28, 0x1a), (0x5a, 0x44, 0x22)), "ujnow": ((0x1d, 0x2a, 0x33), (0x2f, 0x43, 0x52))}
JAM_TINT_22 = 36


def lane_jam(a, x0, P):
    """0.22 a CHORDS or BASS JAM page: the page colour over the main area from x0 (0.24 L: the sound and PHRASE pages' panels), then
    jam_page"""
    art(a, x0, CM_Y, R - x0, CM_H, "page_%dx%d.png" % (R - x0, CM_H))
    jam_page(a, P)


def skin_font(size):
    """0.22 Titillium Web SemiBold (the toolchain's tools/html_art/fonts, which vst/build.sh mounts at /fonts as SKIN_FONTS), for a
    word drawn into a meter's frames"""
    d = os.environ.get("SKIN_FONTS") or os.path.join(os.environ.get("MPC_VST", os.path.join(HERE, "..", "..", "mpc-vst-plugins")),
                                                     "tools", "html_art", "fonts")
    return ImageFont.truetype(os.path.join(d, "TitilliumWeb-SemiBold.ttf"), size)


def dashed(d, box, colour, width=2, dash=8, gap=5):
    """0.22 a dashed outline inside box (x0, y0, x1, y1), width px"""
    x0, y0, x1, y1 = box
    for a0, a1, fixed, horizontal in ((x0, x1, y0, True), (x0, x1, y1 - width + 1, True), (y0, y1, x0, False),
                                      (y0, y1, x1 - width + 1, False)):
        for p in range(a0, a1 + 1, dash + gap):
            q = min(p + dash - 1, a1)
            d.rectangle([p, fixed, q, fixed + width - 1] if horizontal else [fixed, p, fixed + width - 1, q], fill=colour)


def jam_images_22():
    """0.22 the CHORDS and BASS JAM pages' pictures: each lane's grid (its blocks' fixed pictures in its colour), NOW PLAYING's box, the
    blocks' meter filmstrip (0 clear: the grid shows; 1 playing: a 3 px frame in the lane's colour over a light tint of it; 2 NEXT: a
    2 px dashed amber outline, the cell's own text says NEXT), the phrase bars' (plain, playing, locked: violet with LOCKED under the
    bar's text, locked and playing), the page colour over the main area (0.24)"""
    pics = pictures("jam_pictures_cb.txt")
    for n, P in enumerate((JP_CHORDS, JP_BASS)):
        colour = LANES[n][1]
        jam_grid(JB_W, "%s_%dx%d.png" % (P.grid, JG_W, JG_H), pics[30 * n:30 * n + 30], colour, chords=n == 0)
        save(rounded(P.now_w, 64, *JAM_NOW_22[P.now]), "%s_%dx64.png" % (P.now, P.now_w))
        im = Image.new("RGBA", (JB_W, 3 * 64), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        d.rectangle([0, 64, JB_W - 1, 127], fill=colour + (JAM_TINT_22,), outline=colour + (255,), width=3)
        dashed(d, (0, 128, JB_W - 1, 191), AMBER + (255,))
        save(im, P.meters[2])
    font = skin_font(20)
    im = Image.new("RGB", (JPH_W, 4 * 64), TILE_OFF)
    d = ImageDraw.Draw(im)
    for f in range(4):
        top = 64 * f
        d.rectangle([0, top, JPH_W - 1, top + 63], fill=JQ_LOCKED if f >= 2 else TILE_ON if f else TILE_OFF,
                    outline=AMBER if f % 2 else CELL_LINE, width=3 if f % 2 else 1)
        if f >= 2:   # LOCKED, tracked 2 px, centred under the text band
            w = sum(font.getlength(ch) for ch in "LOCKED") + 2 * 5
            x = (JPH_W - w) / 2
            for ch in "LOCKED":
                d.text((x, top + 51), ch, font=font, fill=JQ_WORD, anchor="lm")
                x += font.getlength(ch) + 2
    save(im, "jbar_%dx64.png" % JPH_W)
    save(tile(R - L, CM_H, (15, 16, 18)), "page_%dx%d.png" % (R - L, CM_H))   # 0.24 the page colour over the main area's panels


def global_tab(a):
    a("[tab GLOBAL]")
    header(a, "GLOBAL")
    part_readout(a)
    name(a, 1010, 126, "TEMPO", align="right")
    a("readout cx=1144 cy=126 w=232 h=44 label=\"\" key=bpm box=0 tsize=28 tcolor=f2a33a talign=right")
    name(a, L, 170, "KEY")   # a two-row piano: each black key centred over the boundary of its two white keys
    pitch, black = KEY_WW + KEY_GAP, "img=images/key_b_off.png img_on=images/key_b_on.png"
    for key, first, n in (("kb", 0, 2), ("kb2", 3, 3)):   # C# D# over C-D, D-E; F# G# A# over F-G, G-A, A-B
        edge = L + (first + 1) * pitch - KEY_GAP // 2
        cells(a, edge - KEY_BW // 2, 186, n * pitch - (pitch - KEY_BW), KEY_BH, key, n, gap=pitch - KEY_BW, img=black, pad=4)
    cells(a, L, 258, 7 * pitch - KEY_GAP, KEY_WH, "kw", 7, gap=KEY_GAP, colour="111214",
          img="img=images/key_w_off.png img_on=images/key_w_on.png")
    sx = L + 7 * pitch - KEY_GAP + 16      # SCALE, right of the piano ("Harm. minor" at 24 px)
    name(a, sx, 170, "SCALE")
    cells(a, sx, 186, R - sx, 150, "sc", 4, rows=2, pad=3)
    name(a, L, 372, "LOOP LENGTH")
    cells(a, L, 388, 7 * pitch - KEY_GAP, 64, "ln", 5, pad=6)
    name(a, sx, 372, "CHORD LENGTH")
    cells(a, sx, 388, R - sx, 64, "cl", 4)
    name(a, L, 488, "MIDI CHANNELS")
    for n, (lane, colour) in enumerate(LANES, 1):
        x0 = L + (n - 1) * 424
        a('text cx=%d cy=536 label="%s" align=left size=%.1f weight=700 color=%s' % (x0, lane, NX, hexc(colour)))
        a("stepper cx=%d cy=536 w=220 h=64 key=ch_%d prev=ch_%d_prev next=ch_%d_next tsize=%d" % (x0 + 218, n, n, n, RX))
    fol_w = 130                            # FOLLOW (after 0.16: GLOBAL only, on by default), its note beside it (0.18: NEW IDEA is gone)
    cells(a, L, 600, fol_w, 64, "fol", 1, pad=6)
    hint(a, L + fol_w + 16, 619, "FOLLOW: every page shows the bar that is playing.")   # 0.24 two lines: NAMES beside it
    hint(a, L + fol_w + 16, 645, "Turn it off to save CPU.")
    nx = R - row_w(2, 130)                 # 0.24 NAMES (from the CHORDS header): what the chord cells show, right of FOLLOW's note
    name(a, nx - 12, 632, "CHORDS SHOW", align="right")
    cells(a, nx, 600, row_w(2, 130), 64, "gnm", 2, pad=6)
    a('qlinks "GLOBAL" = -')


def check_stepper_arrows(conf):
    """The toolchain crops every stepper's arrow pictures from the canvas the last mode drawn on its tab left (shadow_skin.py,
    35ba3d1; modes in order of their first when=): a stepper under another mode needs one of the same geometry there, or one
    with no when=. Stops the build otherwise."""
    for tab in conf.split("\n[tab ")[1:]:
        lines = tab.split("\n")
        conds = [m.group(1) for m in (re.search(r" when=(\S+)", ln) for ln in lines) if m]
        last = list(dict.fromkeys(conds))[-1] if conds else None   # the mode whose first line comes last
        drawn, rest = set(), []
        for ln in lines:
            if not ln.startswith("stepper "):
                continue
            f = dict(kv.split("=", 1) for kv in ln.split()[1:] if "=" in kv)
            geom = tuple(int(f[k]) for k in ("cx", "cy", "w", "h"))
            if f.get("when") in (None, last):
                drawn.add(geom)
            else:
                rest.append((f["key"], geom))
        for key, geom in rest:
            if geom not in drawn:
                raise SystemExit("stepper %s: its arrows are cropped from mode %s, which draws none at %r (cx, cy, w, h)"
                                 % (key, last, geom))


def check_names():
    """0.20 every module-level name (constant or function) is defined once: a later tab's constant reusing an earlier one's name
    changes that tab's geometry silently (0.20's PHRASE panel height took PLAY's playhead PH_H). Stops the build otherwise."""
    import ast
    seen = collections.Counter()
    for n in ast.parse(open(os.path.abspath(__file__)).read()).body:
        targets = n.targets if isinstance(n, ast.Assign) else [n.target] if isinstance(n, ast.AnnAssign) else []
        seen.update(e.id for t in targets for e in ast.walk(t) if isinstance(e, ast.Name))
        if isinstance(n, ast.FunctionDef):
            seen[n.name] += 1
    twice = sorted(k for k, v in seen.items() if v > 1)
    if twice:
        raise SystemExit("make_skin.py: defined more than once at module level: %s" % ", ".join(twice))


# vst/layout.theme's theme_tile_on and theme_seg_active (teal) style only the lists without img= (jbk_ and wpk_; 0.22 cjb_, cjp_, ujb_,
# ujp_, never lit) and the popups/enum segments: a future plain list or popup would turn teal (no comment in layout.theme itself: it is
# copied into layout.conf as it is)
def layout():
    o = [open(os.path.join(HERE, "layout.theme")).read().rstrip()]
    for tab in (play_tab, chords_tab, bass_tab, melody_tab, global_tab):
        o.append("")
        tab(o.append)
    o.append("")
    return "\n".join(o)


if __name__ == "__main__":
    check_names()
    conf = layout()   # first: it collects the tile sizes make_images draws
    check_stepper_arrows(conf)
    make_images()
    n = make_params()
    open(os.path.join(HERE, "layout.conf"), "w").write(conf)
    print("params:", n, "images:", len(os.listdir(IMG)))
