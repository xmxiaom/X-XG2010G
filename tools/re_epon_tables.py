#!/usr/bin/env python3
"""
EPON 寄存器表提取器

原厂 xpon.ko 里有 5 个巨型函数，全是「寄存器默认值表」的展开：
  eponMacTableInit        18744B  112 条  MAC 初始化序列
  eponRegDefValTableInit  21196B  ~130条 默认值表
  eponRegNotRstTableInit  10372B  ~63条  非复位表
  eponRegRstTableInit      5320B  ~32条  复位表
  eponLogRstTableInit      1668B  ~11条  逻辑复位表

它们共用同一套代码模式：
  t = kmalloc(n * 12);
  t[i].addr = BASE + 0xNNNN;   // mov x0,#off; add x0,base,x0; mov w3,x0; str w1,[x0]
  t[i].val  = 0xVVVVVVVV;      // mov/movk 组合，或 str wzr
  t[i].mask = 0xMMMMMMMM;      // 同上
  i++;

表项 12 字节 = {addr(u32), value(u32), mask(u32)}

用法：
  python tools/re_epon_tables.py <xpon.ko> --out build/epon-tables.txt
"""
import argparse
import re
import sys

try:
    from elftools.elf.elffile import ELFFile
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
    from capstone.arm64 import ARM64_OP_IMM
except ImportError:
    sys.exit("需要 capstone 与 pyelftools：python -m pip install capstone pyelftools")


TABLES = [
    (0x0339b0, 18744, "eponMacTableInit",       "MAC 初始化序列"),
    (0x043c90, 21196, "eponRegDefValTableInit", "寄存器默认值表"),
    (0x04140c, 10372, "eponRegNotRstTableInit", "非复位寄存器表"),
    (0x03f8c0, 5320,  "eponRegRstTableInit",    "复位寄存器表"),
    (0x040d88, 1668,  "eponLogRstTableInit",    "逻辑复位寄存器表"),
]


def value_before(ins, idx):
    """解析 str 指令写入的值。

    三种情况：
      str wzr, ...        → 0
      mov w1, w3          → 'base'（写入地址本身）
      mov w1, #imm [movk] → 立即数
    """
    s = ins[idx]
    if re.search(r"\bwzr\b|\bxzr\b", s.op_str):
        return 0

    m = re.match(r"\s*([wx]\d+)\s*,", s.op_str)
    if not m:
        return None
    src = m.group(1)

    # 沿 mov 链回溯，最多 10 条
    for j in range(idx - 1, max(idx - 10, -1), -1):
        p = ins[j]
        mm = re.match(r"\s*([wx]\d+)\s*,\s*([wx]\d+)\s*$", p.op_str)
        if p.mnemonic == "mov" and mm and mm.group(1) == src:
            if mm.group(2) in ("w3", "x3"):
                return "base"
            src = mm.group(2)
            continue

        if p.mnemonic in ("mov", "movz") and len(p.operands) == 2:
            mm = re.match(r"\s*([wx]\d+)\s*,", p.op_str)
            if not mm or mm.group(1) != src:
                continue
            if p.operands[1].type != ARM64_OP_IMM:
                continue
            v = p.operands[1].imm & 0xFFFFFFFF
            # movk 在 mov 之后，须向后找（原先向前找导致高 16 位丢失）
            for k in range(j + 1, min(j + 6, len(ins))):
                q = ins[k]
                if q.mnemonic != "movk":
                    continue
                qm = re.match(r"\s*([wx]\d+)\s*,", q.op_str)
                if not qm or qm.group(1) != src:
                    continue
                sh = 0
                sm = re.search(r"lsl #(\d+)", q.op_str)
                if sm:
                    sh = int(sm.group(1))
                v = (v & ~(0xFFFF << sh)) | ((q.operands[1].imm & 0xFFFF) << sh)
            return v & 0xFFFFFFFF
    return None


def extract(ins, relmap):
    """从指令流提取 {offset, value, mask} 三元组列表"""
    rows, cur = [], None
    for idx, i in enumerate(ins):
        if relmap.get(i.address, "") == "g_EPON_MAC_BASE":
            cur = "pend"
            continue
        if cur == "pend":
            if i.mnemonic == "mov" and len(i.operands) == 2 and i.operands[1].type == ARM64_OP_IMM:
                cur = i.operands[1].imm
            elif i.mnemonic == "add" and "lsl #12" in i.op_str:
                m = re.search(r"#(0x[0-9a-f]+)", i.op_str)
                cur = (int(m.group(1), 16) << 12) if m else 0
            elif i.mnemonic == "add" and i.op_str.count("#") == 1 and "lsl" not in i.op_str:
                m = re.search(r"#(0x[0-9a-f]+|\d+)", i.op_str)
                if m:
                    cur = int(m.group(1), 16) if m.group(1).startswith("0x") else int(m.group(1))
            continue
        if isinstance(cur, int) and i.mnemonic == "str":
            if "[x0, #4]" in i.op_str:
                # 值槽位：新建一行
                rows.append([cur, value_before(ins, idx), None])
            elif "[x0, #8]" in i.op_str:
                # 掩码槽位。有些表（eponRegRstTableInit）只写掩码不写值，
                # 此时当前行还没有建立，需要补一行 value=None。
                if rows and rows[-1][0] == cur and rows[-1][2] is None:
                    rows[-1][2] = value_before(ins, idx)
                else:
                    rows.append([cur, None, value_before(ins, idx)])
                cur = None
    return [r for r in rows if r[0] != 0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ko", help="xpon.ko 路径")
    ap.add_argument("--out", default="-", help="输出文件，- 表示 stdout")
    ap.add_argument("--filter", default="", help="只处理名字含该子串的表")
    args = ap.parse_args()

    elf = ELFFile(open(args.ko, "rb"))
    text = elf.get_section_by_name(".text").data()
    symtab = elf.get_section_by_name(".symtab")
    rela = elf.get_section_by_name(".rela.text")
    relmap = {r["r_offset"]: symtab.get_symbol(r["r_info_sym"]).name
              for r in rela.iter_relocations()}

    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    md.detail = True

    out = sys.stdout if args.out == "-" else open(args.out, "w", encoding="utf-8")
    total = 0
    for addr, size, name, desc in TABLES:
        if args.filter and args.filter not in name:
            continue
        ins = list(md.disasm(text[addr:addr + size], addr))
        rows = extract(ins, relmap)
        total += len(rows)
        out.write(f"\n## {name}  ({size}B) — {desc}\n")
        out.write(f"# 共 {len(rows)} 条表项\n")
        out.write("# offset      value       mask\n")
        for off, val, mask in rows:
            def fmt(v):
                if v == "base":
                    return "base      "
                if isinstance(v, int):
                    return f"0x{v:08x}"
                return "?         "
            out.write(f"0x{off:06x}    {fmt(val)}  {fmt(mask)}\n")

    if out is not sys.stdout:
        out.close()
        print(f"已写入 {args.out}，共 {total} 条表项")


if __name__ == "__main__":
    main()
