#!/usr/bin/env bash
# sim/run.sh —— 编译桌面模拟器并把所有界面渲染成 PNG(需要 cc、make、python3 + Pillow)。
# 依赖 managed_components/lvgl__lvgl(先跑一次 idf.py build 或 reconfigure 拉取)。
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-${root}/build/sim}"
mkdir -p "${out}"
make -s -C "${root}/sim" -j"$(nproc 2>/dev/null || echo 4)"
rm -f "${out}"/*.ppm "${out}"/*.png
"${root}/build/sim-obj/toolbox-sim" "${out}"
python3 - "${out}" <<'PY'
import sys, pathlib
from PIL import Image
out = pathlib.Path(sys.argv[1])
shots = sorted(out.glob("*.ppm"))
for p in shots:
    Image.open(p).save(p.with_suffix(".png"))
    p.unlink()
# 拼一张总览图,便于一次检查全部界面。
imgs = [Image.open(p.with_suffix(".png")) for p in shots]
if imgs:
    cols = 6
    rows = (len(imgs) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * 250, rows * 330), (40, 40, 40))
    for i, im in enumerate(imgs):
        sheet.paste(im, ((i % cols) * 250 + 5, (i // cols) * 330 + 5))
    sheet.save(out / "contact-sheet.png")
print(f"{len(imgs)} screenshots -> {out}")
PY
