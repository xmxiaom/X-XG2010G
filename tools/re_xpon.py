#!/usr/bin/env python3
"""xpon.ko 逆向工具：从原厂 GPON 驱动恢复寄存器表与函数清单。

依赖：pip install capstone pyelftools

用法：
    python tools/re_xpon.py <xpon.ko> --out-dir .

产出：
    src/airoha-xpon-gpon-regs.h    GPON MAC 寄存器定义（可直接用于移植）
    build/gpon-csr.csv             偏移/名称/读写 对照表
    build/gpon-funcs.txt           GPON 函数清单（地址/大小）

原理：
    .ko 未 strip，.symtab 保留全部符号。g_gpon_mac_reg_BASE 是 MAC 基址全局变量。
    gponDevDumpCsr() 是一段完全展开的 "get_xpon_data(base + imm)" 序列，
    每条读之后紧跟一个 printk，其格式串里带寄存器名。
    把 "偏移 → 格式串" 配对，即可还原整张寄存器表。
"""
import argparse
import csv
import os
import re
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
from capstone.arm64 import ARM64_OP_IMM, ARM64_SFT_LSL
from elftools.elf.elffile import ELFFile

BASE_SYMBOL = "g_gpon_mac_reg_BASE"
# 原厂固件里 g_gpon_mac_reg_BASE = 0xBFB60000，GPON MAC 实际落在 +0x4000。
# 设备树 "gpon" 块为 0x1fb64000 / 0x3e8，正好覆盖 0x4000..0x43e7。
GPON_WINDOW_START = 0x4000

# "0x40B0: Vendor ID ..." → 名称
NAME_RE = re.compile(r"^0x([0-9A-Fa-f]{4})\s*:\s*(.*?)\s*:\s*0x%", re.S)


def load(path):
    elf = ELFFile(open(path, "rb"))
    text = elf.get_section_by_name(".text").data()
    symtab = elf.get_section_by_name(".symtab")
    rela = elf.get_section_by_name(".rela.text")
    sections = {i: s for i, s in enumerate(elf.iter_sections())}
    relocs = {}
    for r in rela.iter_relocations():
        s = symtab.get_symbol(r["r_info_sym"])
        relocs[r["r_offset"]] = (s.name, s["st_shndx"], r["r_addend"])
    funcs = sorted(
        (s["st_value"], s["st_size"], s.name)
        for s in symtab.iter_symbols()
        if s["st_info"]["type"] == "STT_FUNC" and s["st_size"] > 0
    )
    return text, sections, relocs, funcs


def read_string(sections, secidx, off):
    sec = sections.get(secidx)
    if sec is None or off < 0:
        return None
    try:
        data = sec.data()
    except Exception:
        return None
    if off >= len(data):
        return None
    end = data.find(b"\0", off)
    return data[off:end].decode("utf-8", "replace")


def extract_csr(text, sections, relocs, funcs):
    """还原 gponDevDumpCsr 里的 (偏移, 寄存器名) 对。"""
    target = {n: (a, s) for a, s, n in funcs}.get("gponDevDumpCsr")
    if not target:
        sys.exit("找不到 gponDevDumpCsr，符号表可能已被裁剪")
    addr, size = target
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    rows, cur = [], None
    for insn in md.disasm(text[addr : addr + size], addr):
        if insn.mnemonic == "mov" and len(insn.operands) == 2 and insn.operands[1].type == ARM64_OP_IMM:
            cur = insn.operands[1].imm
        elif insn.mnemonic == "add" and len(insn.operands) == 3 and insn.operands[2].type == ARM64_OP_IMM:
            imm = insn.operands[2].imm
            # 处理 "add x0, x0, #4, lsl #12" 这类带移位的立即数
            sh = insn.operands[2].shift
            if sh.type == ARM64_SFT_LSL:
                imm <<= sh.value
            cur = imm if cur is None else cur + imm
        elif insn.mnemonic == "adrp":
            _, sec, add = relocs.get(insn.address, ("", "SHN_UNDEF", 0))
            txt = read_string(sections, sec, add)
            if txt and cur is not None:
                m = NAME_RE.match(txt.strip())
                if m:
                    rows.append((cur, m.group(1), m.group(2).strip()))
                cur = None
    return rows


def sanitize(name):
    name = re.sub(r"[^0-9A-Za-z]+", "_", name).strip("_").upper()
    drop = {"THE", "OF", "AND", "FOR", "IN", "IS", "A"}
    parts = [p for p in re.split(r"_+", name) if p and p not in drop]
    return "_".join(parts)


def emit_header(rows, out_path):
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    lines = [
        "/* SPDX-License-Identifier: GPL-2.0-only */",
        "/*",
        " * Airoha AN7581 GPON MAC 寄存器定义",
        " *",
        " * 由 tools/re_xpon.py 从原厂 xpon.ko 逆向还原，请勿手工修改。",
        " * 偏移基准 = 设备树 \"gpon\" 资源块（0x1fb64000，长度 0x3e8）。",
        " * 原厂驱动里这些寄存器位于 g_gpon_mac_reg_BASE + 0x4000，",
        " * 此处已减去 0x4000，可直接配合 devm_platform_ioremap_resource_byname(pdev, \"gpon\") 使用。",
        " */",
        "#ifndef _AIROHA_XPON_GPON_REGS_H_",
        "#define _AIROHA_XPON_GPON_REGS_H_",
        "",
    ]
    used = {}
    for off, hexs, name in sorted(rows):
        rel = off - GPON_WINDOW_START
        if rel < 0:
            continue
        sym = "AIROHA_GPON_" + sanitize(name)[:48] or "REG"
        n = used.get(sym, 0)
        used[sym] = n + 1
        if n:
            sym = f"{sym}_{n}"
        lines.append(f"#define {sym:<52s} 0x{rel:04x} /* {name} */")
    lines += ["", "#endif /* _AIROHA_XPON_GPON_REGS_H_ */", ""]
    open(out_path, "w", encoding="utf-8").write("\n".join(lines))
    return len(used)


def emit_csv(rows, out_path):
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["base_offset", "gpon_block_offset", "name"])
        for off, hexs, name in sorted(rows):
            w.writerow([f"0x{off:04x}", f"0x{off - GPON_WINDOW_START:04x}", name])


def emit_funcs(funcs, out_path):
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(f"# 函数总数 {len(funcs)}\n")
        for a, s, n in funcs:
            tag = "GPON" if "gpon" in n.lower() else ""
            f.write(f"0x{a:06x} {s:6d} {tag:5s} {n}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ko")
    ap.add_argument("--out-dir", default=".")
    a = ap.parse_args()
    text, sections, relocs, funcs = load(a.ko)
    rows = extract_csr(text, sections, relocs, funcs)
    print(f"寄存器 {len(rows)} 条，函数 {len(funcs)} 个")
    n = emit_header(rows, os.path.join(a.out_dir, "src", "airoha-xpon-gpon-regs.h"))
    emit_csv(rows, os.path.join(a.out_dir, "build", "gpon-csr.csv"))
    emit_funcs(funcs, os.path.join(a.out_dir, "build", "gpon-funcs.txt"))
    print(f"头文件定义 {n} 个寄存器宏")


if __name__ == "__main__":
    main()
