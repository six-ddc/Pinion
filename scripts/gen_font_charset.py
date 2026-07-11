#!/usr/bin/env python3
# gen_font_charset.py
# ------------------------------------------------------------------------------
# 从 LVGL 生成的 puhui 点阵字体 .c 文件里，解析 cmaps 还原出全部 unicode 码点，
# 编码成 delta(LEB128)+base64，写入 main/ebook/ebook_font_charset.h。
#
# 这套码点用于阅读器「Web 上传字体」页在浏览器端（harfbuzzjs）裁剪用户字体，
# 使裁后覆盖与设备内置 puhui 完全一致。
#
# 何时重跑：升级/更换内置 puhui 字体（managed_components/78__xiaozhi-fonts）后，
# 其字符集可能变化，需重新生成本头文件。
#
#   python scripts/gen_font_charset.py            # 用默认 20px 字体 + 默认输出
#   python scripts/gen_font_charset.py <in.c> <out.h>
#
# 说明：内置 16/20/30px 字体字符集逐字一致，任选其一即可（默认取 20px）。
# ------------------------------------------------------------------------------
import base64
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_IN = os.path.join(
    REPO, "managed_components", "78__xiaozhi-fonts", "src", "font_puhui_20_4.c")
DEFAULT_OUT = os.path.join(REPO, "main", "ebook", "ebook_font_charset.h")


def load(path):
    return open(path, "r", encoding="utf-8", errors="replace").read()


def parse_arrays(src, prefix):
    """解析形如  static const uintN_t <prefix>_<id>[] = { ... };  的数组。"""
    out = {}
    pat = re.compile(
        r"static\s+const\s+uint(?:8|16|32)_t\s+(" + re.escape(prefix) +
        r"_\d+)\s*\[\]\s*=\s*\{([^}]*)\};", re.S)
    for m in pat.finditer(src):
        nums = [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", m.group(2))]
        out[m.group(1)] = nums
    return out


def parse_cmaps(src):
    m = re.search(r"lv_font_fmt_txt_cmap_t\s+cmaps\s*\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    entries = []
    for block in re.findall(r"\{(.*?)\}", m.group(1), re.S):
        def field(name):
            mm = re.search(re.escape(name) + r"\s*=\s*([^,}\n]+)", block)
            return mm.group(1).strip() if mm else None
        entries.append({
            "range_start": int(field(".range_start"), 0),
            "range_length": int(field(".range_length"), 0),
            "unicode_list": field(".unicode_list"),
            "glyph_id_ofs_list": field(".glyph_id_ofs_list"),
            "type": field(".type"),
        })
    return entries


def extract_codepoints(cfile):
    src = load(cfile)
    ulists = parse_arrays(src, "unicode_list")
    olists = parse_arrays(src, "glyph_id_ofs_list")
    cps = set()
    for e in parse_cmaps(src):
        t, rs, rl = e["type"], e["range_start"], e["range_length"]
        if t.endswith("FORMAT0_TINY"):
            for i in range(rl):
                cps.add(rs + i)
        elif t.endswith("FORMAT0_FULL"):
            ofs = olists[e["glyph_id_ofs_list"]]
            for i in range(rl):
                # ofs==0 且非首个 => 该码点在此范围是空洞（无真实字形），剔除
                if i == 0 or (i < len(ofs) and ofs[i] != 0):
                    cps.add(rs + i)
        elif t.endswith("SPARSE_TINY") or t.endswith("SPARSE_FULL"):
            for off in ulists[e["unicode_list"]]:
                cps.add(rs + off)
        else:
            raise RuntimeError("未知 cmap type: %s" % t)
    cps.discard(0x0)
    return sorted(cps)


def leb128(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def encode(cps):
    data = bytearray()
    prev = 0
    for c in cps:
        data += leb128(c - prev)
        prev = c
    return base64.b64encode(bytes(data)).decode("ascii")


def emit_header(cps, b64, out_path, in_name):
    # 把 base64 切成每行 100 字符，作为相邻 C 字符串字面量拼接
    lines = [b64[i:i + 100] for i in range(0, len(b64), 100)]
    body = "\n".join('        "%s"' % ln for ln in lines)
    text = (
        "// ebook_font_charset.h\n"
        "// ==== 生成文件，请勿手改 —— 由 scripts/gen_font_charset.py 生成 ====\n"
        "// 内置 puhui 字体（%s）的字符集：%d 个码点。\n"
        "// 编码：sorted 码点 -> delta -> LEB128 varint -> base64。\n"
        "// 用途：阅读器 Web 上传页在浏览器端按此码点集裁剪用户字体（harfbuzzjs），\n"
        "//       使裁后覆盖与设备内置字体一致。升级内置 puhui 后请重跑脚本。\n"
        "\n"
        "#ifndef EBOOK_FONT_CHARSET_H\n"
        "#define EBOOK_FONT_CHARSET_H\n"
        "\n"
        "namespace ebook_font_charset {\n"
        "\n"
        "// 码点集（delta-LEB128-base64）。前端 atob + varint 解码还原为码点数组。\n"
        "inline const char* PuhuiB64() {\n"
        "    return\n"
        "%s;\n"
        "}\n"
        "\n"
        "// 码点总数（供前端/日志核对）。\n"
        "inline int PuhuiCount() { return %d; }\n"
        "\n"
        "}  // namespace ebook_font_charset\n"
        "\n"
        "#endif  // EBOOK_FONT_CHARSET_H\n"
    ) % (in_name, len(cps), body, len(cps))
    open(out_path, "w", encoding="utf-8").write(text)


def main():
    cfile = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_IN
    out = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUT
    cps = extract_codepoints(cfile)
    b64 = encode(cps)
    emit_header(cps, b64, out, os.path.basename(cfile))
    cjk = sum(1 for c in cps if 0x4E00 <= c <= 0x9FFF)
    print("源: %s" % cfile)
    print("码点总数: %d  (CJK统一表意 %d)" % (len(cps), cjk))
    print("base64 长度: %d 字节 (~%.1f KB)" % (len(b64), len(b64) / 1024))
    print("已写出: %s" % out)


if __name__ == "__main__":
    main()
