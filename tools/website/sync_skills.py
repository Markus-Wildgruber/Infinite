#!/usr/bin/env python3
"""Publishes the user-facing AI skills to the website library.

What: copies each listed .claude/skills/<name>/SKILL.md to website/assets/skills/<name>.md, draws a preview card for it,
      and writes website/assets/skills/index.json (category "AI Skills").
Why:  the skills are the single source of truth in .claude/skills; the library only mirrors them.
Usage: python3 tools/website/sync_skills.py && python3 tools/website/gen_seo.py
Exit codes: 0 written.
"""
import json, os, re, shutil
from PIL import Image, ImageDraw, ImageFont
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "website", "assets", "skills")
# (skill folder, library name, one-line description for people)
SKILLS = [
    ("infinite-patch-authoring", "Patch Authoring", "Headless engine: write, check and render .inf patches from the command line, no UI."),
    ("field-language", "Field Language", "The Field syntax and rules: domains, inferred rates, bare names, attributes, params and state."),
    ("field-pixel-presets", "Field Pixel Presets", "Write Field Pixel presets that look right first time: aspect, falloff and blending."),
    ("field-modifier-presets", "Field Modifier Presets", "Write Field Modifier presets that reshape geometry: position, normals, colour and outputs."),
    ("field-domains", "Field Domains", "Move data between rates in Field: reduce, map, broadcast, resample and downsample."),
    ("field-state", "Field State", "Delay, feedback and smoothing in Field: how state cells reset, save and hot-reload."),
]
def font(sz):
    try: return ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", sz)
    except Exception: return ImageFont.load_default()
def card(name, desc, path):
    im = Image.new("RGB", (900, 600), "#151930"); g = ImageDraw.Draw(im)
    g.rounded_rectangle((70, 110, 830, 490), 24, fill="#20263F")
    for k, c in enumerate(["#F57F66", "#343B5E", "#343B5E"]): g.ellipse((100 + k * 26, 140, 118 + k * 26, 158), fill=c)
    lines = [("---", "#8890B1"), ("name: " + name, "#EEF1FA"), ("description: " + desc[:34], "#A3AACB"), ("  " + desc[34:68].strip(), "#A3AACB"), ("---", "#8890B1"), ("", "#fff"), ("# " + name, "#FF6B35"), ("", "#fff"), ("SKILL.md  for AI coding agents", "#A7B1FF")]
    for n, (t, c) in enumerate(lines): g.text((105, 190 + n * 32), t, font=font(24), fill=c)
    im.save(path, quality=85)
os.makedirs(OUT, exist_ok=True)
items = []
for folder, title, desc in SKILLS:
    shutil.copy(os.path.join(ROOT, ".claude", "skills", folder, "SKILL.md"), os.path.join(OUT, folder + ".md"))
    card(folder, desc, os.path.join(OUT, folder + ".webp"))
    items.append({"id": folder.replace("infinite-", "") + "-skill", "name": title, "category": "AI Skills", "kind": "Skill", "author": "n1m21n",
                  "description": desc, "price": 0, "preview": f"assets/skills/{folder}.webp", "file": f"assets/skills/{folder}.md"})
json.dump({"version": 1, "items": items}, open(os.path.join(OUT, "index.json"), "w"), indent=1)
print(len(items), "skills written")
