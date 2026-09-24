#!/usr/bin/env python3
"""Gera oni_splash_caption.png por densidade — caption de fase sob o wordmark.

P4.6 Bloco 0 (R4 micro: versão visível). O caption do splash é um asset
GERADO (não fonte de verdade): o texto dinâmico real lê BuildConfig na
sheet de Configurações ("Versão"). Para trocar a fase:

    python3 scripts/gen_splash_caption.py P4.7

Gera em android/app/src/main/res/drawable-{mdpi,hdpi,xhdpi,xxhdpi}/.
Cor = Oni.TEXT_DIM (#8B949E, 12sp secundário do Curved Dark).
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

RES = Path(__file__).resolve().parent.parent / "android/app/src/main/res"
DENSITIES = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0}
BASE_SP = 13  # 12sp de texto + respiro vertical
COLOR = (139, 148, 158, 255)  # #8B949E
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"


def render_master(text: str) -> Image.Image:
    """Renderiza uma ÚNICA vez em supersample (base mdpi) — todas as
    densidades saem por fator exato, garantindo proporção idêntica."""
    ss = 8  # 8x sobre a base mdpi
    font = ImageFont.truetype(FONT, BASE_SP * ss)
    probe = Image.new("RGBA", (8, 8))
    bbox = ImageDraw.Draw(probe).textbbox((0, 0), text, font=font)
    w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
    img = Image.new("RGBA", (w + 8 * ss, h + 8 * ss), (0, 0, 0, 0))
    ImageDraw.Draw(img).text(
        (4 * ss - bbox[0], 4 * ss - bbox[1]), text, font=font, fill=COLOR)
    return img


def main() -> int:
    label = sys.argv[1] if len(sys.argv) > 1 else "P4.6"
    master = render_master(label)
    base = master.width / 8.0  # largura mdpi alvo
    for dpi, scale in DENSITIES.items():
        out = RES / f"drawable-{dpi}" / "oni_splash_caption.png"
        tw = max(1, int(round(base * scale)))
        th = max(1, int(round(master.height * tw / master.width)))
        img = master.resize((tw, th), Image.LANCZOS)
        img.save(out)
        print(f"{out.relative_to(RES.parent.parent)} {img.size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
