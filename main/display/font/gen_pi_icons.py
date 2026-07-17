#!/usr/bin/env python3
"""font_pi_icons_{16,22,28} 生成器 —— Lucide 图标字体子集 + 名字映射表。

pi_card::MakeIcon 的图标字体后端：从 Lucide 图标字体（ISC 许可，随仓库 vendor 在
main/display/font/lucide/）按 ICONS 清单抽取码点，生成三档尺寸的 LVGL 字体
（bpp4，RLE 压缩，与 puhui 同管线），并同步生成排好序的
pi_card/pi_card_icon_map.h（图标名/别名 → UTF-8 字形串），供 MakeIcon 查表。

增删图标：改下方 ICONS（键 = Lucide 官方名，值 = 额外别名列表；码点查
lucide/info.json），然后重跑本脚本。三个 .c 与映射 .h 均为生成物、随仓库提交。

用法（在仓库根目录）：
    python3 main/display/font/gen_pi_icons.py

需要 node/npx（lv_font_conv）。
"""

import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TTF = HERE / "lucide" / "lucide.ttf"
INFO = HERE / "lucide" / "info.json"
MAP_OUT = HERE.parents[1] / "display/screen/pi_screen/pi_card/pi_card_icon_map.h"
SIZES = [16, 22, 28]

# 键 = Lucide 官方图标名（kebab-case，即 MakeIcon 的正名）；值 = 兼容别名
# （历史 MakeIcon 名 / LLM 卡片 DSL 已在用的词）。"dot" 特意不在表内——
# 保留形状拼合的实心圆点作为未知名回落。
ICONS: dict[str, list[str]] = {
    # ---- 系统状态 / 硬件 ----
    "wifi": [],
    "signal": ["cellular"],
    "bluetooth": [],
    "battery": [],
    "battery-low": [],
    "battery-medium": [],
    "battery-full": [],
    "battery-charging": ["charging"],
    "zap": ["bolt"],
    "power": [],
    "plug": [],
    "plug-zap": [],
    "cpu": ["chip"],
    "monitor": ["screen"],
    "smartphone": [],
    "hard-drive": ["sd", "storage"],
    "database": [],
    "server": [],
    "usb": [],
    "gauge": [],
    "activity": [],
    "thermometer": [],
    # ---- 声音 / 媒体 ----
    "volume-2": ["volume", "volume_high"],
    "volume-1": ["volume_low"],
    "volume-x": ["mute"],
    "mic": [],
    "mic-off": [],
    "music": [],
    "headphones": [],
    "radio": [],
    "play": [],
    "pause": [],
    "square": ["stop"],
    "skip-back": [],
    "skip-forward": [],
    "shuffle": [],
    "repeat": [],
    "list-music": ["playlist"],
    "film": [],
    "camera": [],
    "image": [],
    "tv": [],
    "gamepad-2": [],
    # ---- 显示 / 主题 / 时间 ----
    "sun": ["brightness"],
    "moon": [],
    "moon-star": [],
    "sun-moon": ["theme"],
    "clock": [],
    "timer": [],
    "hourglass": [],
    "calendar": [],
    "sunrise": [],
    "sunset": [],
    "lightbulb": [],
    # ---- 基础动作 / 状态 ----
    "check": ["ok"],
    "x": ["close"],
    "plus": ["add"],
    "minus": [],
    "chevron-left": ["back"],
    "chevron-right": ["chevron", "arrow", "next"],
    "chevron-up": [],
    "chevron-down": [],
    "arrow-up": [],
    "arrow-down": [],
    "arrow-left": [],
    "arrow-right": [],
    "settings": ["gear"],
    "sliders-horizontal": ["sliders"],
    "info": [],
    "triangle-alert": ["warning", "alert"],
    "circle-alert": [],
    "circle-check": [],
    "circle-x": [],
    "circle-plus": [],
    "circle-minus": [],
    "ban": [],
    "badge-check": [],
    "circle": ["ring"],
    "search": [],
    "refresh-cw": ["refresh"],
    "download": [],
    "upload": [],
    "send": [],
    "share-2": ["share"],
    "copy": [],
    "clipboard": [],
    "link": [],
    "external-link": [],
    "eye": [],
    "pencil": ["edit"],
    "trash-2": ["trash", "delete"],
    "lock": [],
    "key": [],
    "shield": [],
    "filter": [],
    "pin": [],
    "paperclip": [],
    "wrench": [],
    "hammer": [],
    "scissors": [],
    "menu": ["hamburger"],
    "list": [],
    "layers": [],
    "layout-grid": ["grid"],
    "table": [],
    "flag": [],
    "tag": [],
    "bookmark": [],
    "qr-code": ["qrcode"],
    # ---- 人 / 交流 ----
    "message-circle": ["chat", "message"],
    "user": [],
    "users": [],
    "circle-user": [],
    "bell": [],
    "mail": [],
    "phone": [],
    "globe": [],
    "languages": [],
    "bot": [],
    "brain": [],
    "sparkles": [],
    # ---- 文件 ----
    "folder": ["files"],
    "folder-open": [],
    "file": [],
    "file-text": [],
    "save": [],
    "book-open": ["book"],
    "code": [],
    "braces": [],
    "terminal": [],
    "bug": [],
    # ---- 数据 / 金融 ----
    "trending-up": [],
    "trending-down": [],
    "chart-line": [],
    "chart-column": [],
    "chart-pie": [],
    "dollar-sign": [],
    "banknote": [],
    "wallet": [],
    "credit-card": [],
    "calculator": [],
    "percent": [],
    "hash": [],
    "at-sign": [],
    "infinity": [],
    # ---- 天气 / 自然 ----
    "cloud": [],
    "cloudy": [],
    "cloud-sun": [],
    "cloud-rain": [],
    "cloud-drizzle": [],
    "cloud-snow": [],
    "cloud-lightning": [],
    "cloud-fog": [],
    "haze": [],
    "snowflake": [],
    "wind": [],
    "droplet": [],
    "droplets": [],
    "umbrella": [],
    "rainbow": [],
    "tornado": [],
    "flame": [],
    "leaf": [],
    # ---- 地点 / 出行 ----
    "map-pin": ["location"],
    "map": [],
    "compass": [],
    "navigation": [],
    "home": [],
    "car": [],
    "plane": [],
    "bike": [],
    "train-front": ["train"],
    "anchor": [],
    "package": [],
    "truck": [],
    "rocket": [],
    # ---- 生活 / 奖励 ----
    "star": [],
    "heart": [],
    "trophy": [],
    "medal": [],
    "crown": [],
    "gem": [],
    "gift": [],
    "coffee": [],
    "utensils": [],
    "pizza": [],
}


def utf8_c_literal(cp: int) -> str:
    return "".join(f"\\x{b:02x}" for b in chr(cp).encode("utf-8"))


def main() -> None:
    info = json.loads(INFO.read_text())
    cps: dict[str, int] = {}
    for name in ICONS:
        if name not in info:
            sys.exit(f"not in lucide info.json: {name}")
        cps[name] = int(info[name]["unicode"].strip("&#;"))

    ranges = ",".join(f"0x{cp:x}" for cp in sorted(cps.values()))
    for size in SIZES:
        out = HERE / f"font_pi_icons_{size}.c"
        subprocess.run(
            [
                "npx", "--yes", "lv_font_conv",
                "--font", str(TTF),
                "--format", "lvgl",
                "--lv-include", "lvgl.h",
                "--bpp", "4",
                "--size", str(size),
                "-r", ranges,
                "-o", str(out),
            ],
            check=True,
        )
        print(f"wrote {out.name} ({out.stat().st_size // 1024} KB)")

    entries: list[tuple[str, str]] = []
    for name, aliases in ICONS.items():
        lit = utf8_c_literal(cps[name])
        entries.append((name, lit))
        entries.extend((a, lit) for a in aliases)
    entries.sort()

    lines = [
        "// 生成文件 —— 勿手改。由 main/display/font/gen_pi_icons.py 从 Lucide 码点表生成。",
        "// 图标名（含历史别名）→ UTF-8 字形串，按名字典序排列（可二分）。",
        "#pragma once",
        "",
        "namespace pi_card {",
        "",
        "struct IconGlyph {",
        "    const char* name;",
        "    const char* utf8;",
        "};",
        "",
        "inline constexpr IconGlyph kIconGlyphs[] = {",
    ]
    lines += [f'    {{"{n}", "{lit}"}},' for n, lit in entries]
    lines += [
        "};",
        "",
        "}  // namespace pi_card",
        "",
    ]
    MAP_OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {MAP_OUT.relative_to(HERE.parents[3])} ({len(entries)} entries, {len(ICONS)} glyphs)")


if __name__ == "__main__":
    main()
