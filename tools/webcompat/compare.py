"""Compares the engine's pictures of the test pages with Chromium's.

    python compare.py BROWSER_DIR ENGINE_DIR OUT_DIR

Writes OUT_DIR/report.html (side by side, with the differences in red) and
OUT_DIR/scores.json. A page's score is the share of its pixels that look the
same, counted over the pixels that are not plain page background in either
picture (so an empty page does not score well). Both pictures are softened a
little first, so that text drawn by a different rasteriser does not count, and
a pixel differs when a colour channel is off by more than 40 of 255. Needs
Pillow and numpy.
"""
import html
import json
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

THRESHOLD = 40


def load(path):
    return Image.open(path).convert('RGB')


def score(a, b):
    sa = np.asarray(a.filter(ImageFilter.GaussianBlur(1.2)), dtype=np.int16)
    sb = np.asarray(b.filter(ImageFilter.GaussianBlur(1.2)), dtype=np.int16)
    diff = np.abs(sa - sb).max(axis=2) > THRESHOLD
    # The page background: the most common colour of the reference.
    flat = sa.reshape(-1, 3)
    colors, counts = np.unique(flat, axis=0, return_counts=True)
    bg = colors[counts.argmax()]
    content = (np.abs(sa - bg).max(axis=2) > THRESHOLD) | (np.abs(sb - bg).max(axis=2) > THRESHOLD)
    total = max(int(content.sum()), 1)
    return 1.0 - (diff & content).sum() / total, diff


def main():
    browser_dir, engine_dir, out_dir = sys.argv[1:4]
    os.makedirs(out_dir, exist_ok=True)
    rows, scores = [], {}
    for name in sorted(os.listdir(browser_dir)):
        if not name.endswith('.png'):
            continue
        case = name[:-4]
        ref = load(os.path.join(browser_dir, name))
        engine_path = os.path.join(engine_dir, name)
        if os.path.exists(engine_path):
            got = load(engine_path).resize(ref.size)
            s, diff = score(ref, got)
            overlay = np.asarray(got.convert('L').convert('RGB'), dtype=np.uint8) // 2 + 120
            overlay[diff] = (230, 30, 30)
            Image.fromarray(overlay.astype(np.uint8)).save(os.path.join(out_dir, case + '.diff.png'))
        else:
            s = 0.0
        scores[case] = round(s * 100, 1)
        rows.append(case)
    total = round(sum(scores.values()) / max(len(scores), 1), 1)
    with open(os.path.join(out_dir, 'scores.json'), 'w', encoding='utf-8') as f:
        json.dump({'total': total, 'pages': scores}, f, ensure_ascii=False, indent=1)
    cells = ''.join(
        f'<section><h2>{html.escape(c)} <b>{scores[c]}%</b></h2><div class="trio">'
        f'<figure><img src="browser/{c}.png"/><figcaption>Chromium</figcaption></figure>'
        f'<figure><img src="engine/{c}.png"/><figcaption>Forge</figcaption></figure>'
        f'<figure><img src="{c}.diff.png"/><figcaption>differences</figcaption></figure></div></section>'
        for c in rows)
    with open(os.path.join(out_dir, 'report.html'), 'w', encoding='utf-8') as f:
        f.write('<!doctype html><meta charset="utf-8"><title>Web compat</title><style>'
                'body{font:14px system-ui;margin:16px}.trio{display:flex;gap:8px}figure{margin:0}'
                'img{width:400px;border:1px solid #ccc}h2{font-size:16px}b{color:#2563eb}</style>'
                f'<h1>Forge vs Chromium: {total}%</h1>{cells}')
    print(f'total {total}%')
    for c in rows:
        print(f'  {scores[c]:5.1f}%  {c}')


if __name__ == '__main__':
    main()
