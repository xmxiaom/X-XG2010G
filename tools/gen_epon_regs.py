#!/usr/bin/env python3
"""
EPON 寄存器头文件生成器

从 build/epon-tables.txt 读取逆向出的 349 条表项，
生成可直接用于编译的 airoha-xpon-epon-regs.h。

用法：
  python tools/gen_epon_regs.py build/epon-tables.txt --out src/airoha-xpon-epon-regs.h
"""
import argparse
import re
import sys


# 已知寄存器的语义名（从上游开源实现 + 逆向交叉验证得到）
KNOWN_NAMES = {
    0x000: "GLB_CFG",
    0x004: "GLB_CFG2",
    0x010: "INT_STATUS",
    0x014: "INT_ENABLE",
    0x018: "INT_STATUS2",
    0x01c: "INT_ENABLE2",
    0x020: "INT_STATUS3",
    0x024: "INT_ENABLE3",
    0x07c: "LLID_DISCOVERY_CTRL",
    0x080: "LLID0_DISCOVERY_STATUS",
    0x104: "MAC_ADDR_CFG",
    0x108: "MAC_ADDR_VALUE",
    0x120: "MPCP_STAT",
    0x124: "MPCP_RGST_STAT",
    0x124: "REPORT_CFG",
    0x134: "REPORT_BITMAP",
    0x138: "XPON_SETTING",
    0x1c4: "SYNC_TIME",
    0x1d8: "MPCP_TIMEOUT_INTERVAL",
    0x1e8: "TRX_ADJUST_TIME1",
    0x1ec: "TRX_ADJUST_TIME2",
    0x1f0: "TRX_ADJUST_TIME3",
    0x1f4: "TRX_ADJUST_TIME4",
    0x200: "TX_FETCH_CFG",
    0x238: "ERROR_BYTES",
    0x23c: "ERROR_CODEWORDS",
    0x240: "UNCORRECTED_CODEWORDS",
    0x244: "ALL_CODEWORDS",
    0x248: "FEC_ERROR_SECONDS",
    0x258: "SYNC_LOSS_COUNT",
    0x5e0: "XPON_STA",
    0x5f0: "PHY_INT_ENABLE",
    0x5f4: "PHY_INT_CLEAR",
    0x5f8: "PHY_INT_STATUS",
}


def parse_tables(path):
    """解析 epon-tables.txt，返回 {表名: [(offset, value, mask)]}"""
    tables = {}
    cur = None
    for line in open(path, encoding="utf-8"):
        if line.startswith("## "):
            cur = line.split()[1]
            tables[cur] = []
        elif line.startswith("0x") and cur:
            parts = line.split()
            off = int(parts[0], 16)
            # 相对偏移（表里是 0x6004 形式，减去 0x6000 基址）
            rel = off - 0x6000 if off >= 0x6000 else off
            val = parts[1]
            mask = parts[2] if len(parts) > 2 else "?"
            tables[cur].append((rel, val, mask))
    return tables


def gen_header(tables, out):
    """生成头文件"""
    # 收集所有出现过的寄存器偏移
    all_offs = set()
    for rows in tables.values():
        for off, _v, _m in rows:
            all_offs.add(off)

    w = out.write
    w("/* SPDX-License-Identifier: GPL-2.0-only */\n")
    w("/*\n")
    w(" * Airoha AN7581 EPON MAC 寄存器定义\n")
    w(" *\n")
    w(" * 本文件由 tools/gen_epon_regs.py 从逆向数据自动生成，请勿手改。\n")
    w(" * 数据来源：原厂 xpon.ko 的 5 个初始化表函数（共 349 条表项）\n")
    w(" * 详见 docs/13-EPON寄存器表逆向.md\n")
    w(" *\n")
    w(" * 基址说明：epon_base 已映射到 XEPON_MAC_BASE + 0x6000，\n")
    w(" * 因此下表的偏移都是相对 0x6000 的相对值。\n")
    w(" */\n\n")
    w("#ifndef _AIROHA_XPON_EPON_REGS_H_\n")
    w("#define _AIROHA_XPON_EPON_REGS_H_\n\n")

    w("/* ---- 已知寄存器（含表里未出现但语义明确的） ---- */\n")
    known = set(KNOWN_NAMES.keys()) | all_offs
    for off in sorted(known):
        name = KNOWN_NAMES.get(off)
        if name:
            w(f"#define AIROHA_EPON_{name:<32} 0x{off:03x}\n")
    w("\n/* ---- 未命名寄存器（仅编号，供初始化表引用） ---- */\n")
    for off in sorted(all_offs):
        if off not in KNOWN_NAMES:
            w(f"#define AIROHA_EPON_REG_{off:03X}{'':<26} 0x{off:03x}\n")

    # 初始化序列表
    w("\n/* ---- 寄存器初始化项 ---- */\n")
    w("struct airoha_epon_reg_init {\n")
    w("\tu16 offset;\n")
    w("\tu32 value;\n")
    w("\tu32 mask;\n")
    w("};\n\n")

    for tname, rows in tables.items():
        if not rows:
            continue
        # 表名转大写宏
        # eponMacTableInit → EPON_MAC_TABLE_INIT
        macro = re.sub(r"(?<!^)(?=[A-Z])", "_", tname).upper()
        macro = re.sub(r"_+", "_", macro).strip("_")
        w(f"/* {tname}：{len(rows)} 条 */\n")
        w(f"#define AIROHA_EPON_INIT_{macro}_COUNT {len(rows)}\n")
        w(f"static const struct airoha_epon_reg_init "
          f"airoha_epon_init_{macro.lower()}[] = {{\n")
        for off, val, mask in rows:
            v = val if val.startswith("0x") else "0"
            m = mask if mask.startswith("0x") else "0"
            w(f"\t{{ 0x{off:03x}, {v}, {m} }},\n")
        w("};\n\n")

    w("#endif /* _AIROHA_XPON_EPON_REGS_H_ */\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tables", help="build/epon-tables.txt")
    ap.add_argument("--out", default="-")
    args = ap.parse_args()

    tables = parse_tables(args.tables)
    total = sum(len(r) for r in tables.values())
    print(f"解析到 {len(tables)} 张表，共 {total} 条表项", file=sys.stderr)

    if args.out == "-":
        gen_header(tables, sys.stdout)
    else:
        with open(args.out, "w", encoding="utf-8") as f:
            gen_header(tables, f)
        print(f"已写入 {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
