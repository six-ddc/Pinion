#!/usr/bin/env python3
"""font_puhui_24_4 生成器 —— UI chrome 中文字体（静态文案子集）。

24px 档只服务静态 UI 文案（设置页/快捷面板/按钮/标题）；动态文本（SSID、
蓝牙设备名、agent 回复正文）仍用 xiaozhi-fonts 的 20/30 完整常用字集渲染，
所以这里只需覆盖源码里出现过的字符 —— 新增文案出现缺字（渲染成空白）时
重跑本脚本即可。

用法（在仓库根目录）：
    python3 main/display/font/gen_puhui_24.py /path/to/AlibabaPuHuiTi-3-55-Regular.ttf

字体源：阿里巴巴普惠体 3.0-55-Regular（免费商用；xiaozhi-fonts 的
font_puhui_* 同源）。需要 node/npx（lv_font_conv）。
"""

import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCAN_DIRS = [REPO / "main/display/screen"]
SKIP_FILES = {"pi_models_data.h"}  # gitignored 密钥 JSON，不是 UI 文案
OUT = Path(__file__).resolve().parent / "font_puhui_24_4.c"

# 源码之外的保底字符：全角标点 + 常用符号（文案里迟早会用到，一并纳入，
# 避免为一个标点重跑）。ASCII 0x20-0x7E 走 -r 参数，不在这里。
EXTRA = "，。？！：；、·—…（）「」『』【】《》×÷℃％"


def collect_chars() -> set[str]:
    chars: set[str] = set()
    hex_escape = re.compile(r"(?:\\x[0-9a-fA-F]{2})+")
    for d in SCAN_DIRS:
        for f in sorted(d.rglob("*")):
            if f.suffix not in {".c", ".cc", ".h"} or f.name in SKIP_FILES:
                continue
            text = f.read_text(encoding="utf-8", errors="ignore")
            # 直接内嵌的非 ASCII 字符
            chars.update(ch for ch in text if ord(ch) > 0x7E)
            # "\xe6\x8c\x89" 形式的转义串：按字节拼接后尝试 UTF-8 解码
            for m in hex_escape.finditer(text):
                raw = bytes(int(b, 16) for b in re.findall(r"\\x([0-9a-fA-F]{2})", m.group()))
                try:
                    chars.update(ch for ch in raw.decode("utf-8") if ord(ch) > 0x7E)
                except UnicodeDecodeError:
                    pass  # 非文本的 \x 数据（如 UUID 字节串）
    chars.update(EXTRA)
    # 控制符/零宽字符不进字体
    return {c for c in chars if not c.isspace() or c == "　"}


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    ttf = Path(sys.argv[1])
    if not ttf.exists():
        sys.exit(f"font not found: {ttf}")
    chars = collect_chars()
    symbols = "".join(sorted(chars))
    print(f"{len(chars)} non-ASCII chars: {symbols}")
    subprocess.run(
        [
            "npx", "--yes", "lv_font_conv",
            "--font", str(ttf),
            "--format", "lvgl",
            "--lv-include", "lvgl.h",
            "--bpp", "4",
            "--size", "24",
            "-r", "0x20-0x7E",
            "--symbols", symbols,
            "-o", str(OUT),
        ],
        check=True,
    )
    print(f"wrote {OUT} ({OUT.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
