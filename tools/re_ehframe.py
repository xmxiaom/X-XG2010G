#!/usr/bin/env python3
""".eh_frame FDE 解析：恢复 .ko 中被 symtab 记为 size=0 的静态函数边界。

原理：
  .eh_frame 里每个 FDE 描述一个函数栈帧 [initial_location, +range)。
  在 ET_REL 目标文件里，initial_location 字段带一条重定位（R_AARCH64_PREL32），
  重定位符号就是该函数 —— 于是边界和名字都能拿到，不依赖 symtab 的 st_size。

用法：
    python tools/re_ehframe.py <xpon.ko> [--out build/fde.txt] [--probe 0x133e0]
"""
import argparse
import struct
import sys

from elftools.elf.elffile import ELFFile

DW_EH_PE_omit = 0xFF
DW_EH_PE_absptr = 0x00
DW_EH_PE_pcrel = 0x10
DW_EH_PE_sdata4 = 0x0B


def uleb(buf, pos):
    result = shift = 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return result, pos


def sleb(buf, pos):
    result = shift = 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            if b & 0x40:
                result -= 1 << shift
            return result, pos


def parse_cie(buf, start):
    """返回 (aug 'R' 编码, 结束位置)。"""
    pos = start + 4  # 跳过 CIE_id
    version = buf[pos]; pos += 1
    end = buf.index(b"\0", pos)
    aug = buf[pos:end].decode("latin1"); pos = end + 1
    _, pos = uleb(buf, pos)          # code alignment
    _, pos = sleb(buf, pos)          # data alignment
    if version == 1:
        pos += 1                     # return address register (1 字节)
    else:
        _, pos = uleb(buf, pos)
    enc = DW_EH_PE_absptr
    if aug.startswith("z"):
        aug_len, pos = uleb(buf, pos)
        aug_end = pos + aug_len
        p = pos
        for ch in aug[1:]:
            if ch == "R":
                enc = buf[p]; p += 1
            elif ch == "L":
                p += 1
            elif ch == "P":
                p += 1               # 编码字节
                # 跳过 personality 指针，按长度粗估不可靠，直接停止解析
                break
            elif ch == "S" or ch == "B" or ch == "G":
                continue
            else:
                break
        pos = aug_end
    return enc, pos


def parse(path):
    elf = ELFFile(open(path, "rb"))
    eh = elf.get_section_by_name(".eh_frame")
    rela = elf.get_section_by_name(".rela.eh_frame")
    symtab = elf.get_section_by_name(".symtab")
    if eh is None or rela is None:
        sys.exit("缺少 .eh_frame 或 .rela.eh_frame")
    buf = eh.data()
    relocs = {}
    for r in rela.iter_relocations():
        s = symtab.get_symbol(r["r_info_sym"])
        relocs[r["r_offset"]] = (s.name, s["st_value"], r["r_addend"])

    cies = {}
    fdes = []
    pos = 0
    while pos + 4 <= len(buf):
        length = struct.unpack_from("<I", buf, pos)[0]
        if length == 0:
            break
        if length == 0xFFFFFFFF:
            length = struct.unpack_from("<Q", buf, pos + 4)[0]
            hdr, cie_id_size = 12, 8
        else:
            hdr, cie_id_size = 4, 4
        body = pos + hdr
        end = body + length
        if end > len(buf):
            break
        cie_id = struct.unpack_from("<I", buf, body)[0]
        if cie_id == 0:
            enc, _ = parse_cie(buf, body)
            cies[body] = enc
        else:
            cie_off = body - cie_id
            enc = cies.get(cie_off, DW_EH_PE_pcrel | DW_EH_PE_sdata4)
            loc_off = body + cie_id_size
            name, symval, addend = relocs.get(loc_off, (None, None, 0))
            rng_off = loc_off + 4
            size = struct.unpack_from("<I", buf, rng_off)[0] if rng_off + 4 <= len(buf) else 0
            fdes.append({
                "name": name,
                "start": (symval + addend) if symval is not None else None,
                "size": size,
                "enc": enc,
            })
        pos = end
    return fdes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ko")
    ap.add_argument("--out", default=None)
    ap.add_argument("--probe", action="append", default=[], type=lambda x: int(x, 0))
    a = ap.parse_args()
    fdes = parse(a.ko)
    named = [f for f in fdes if f["start"] is not None]

    # 用 symtab 补名字：FDE 的 initial_location 常指到 STT_SECTION 符号，名字为空，
    # 但同一地址在 symtab 里往往有对应函数名（哪怕 st_size = 0）。
    elf = ELFFile(open(a.ko, "rb"))
    symtab = elf.get_section_by_name(".symtab")
    text_idx = [i for i, s in enumerate(elf.iter_sections()) if s.name == ".text"]
    text_idx = text_idx[0] if text_idx else None
    symname = {}
    for s in symtab.iter_symbols():
        if s["st_shndx"] == text_idx and s["st_value"] > 0 and s.name:
            symname.setdefault(s["st_value"], s.name)
    for f in named:
        if not f["name"]:
            f["name"] = symname.get(f["start"], "")

    print(f"FDE 共 {len(fdes)} 个，其中有符号定位的 {len(named)} 个")
    print(f"能补出名字的 {sum(1 for f in named if f['name'])} 个，纯匿名 {sum(1 for f in named if not f['name'])} 个")
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write("# start size name  (来自 .eh_frame FDE)\n")
            for fde in sorted(named, key=lambda x: x["start"]):
                f.write(f"0x{fde['start']:06x} {fde['size']:6d} {fde['name']}\n")
        print("已写入", a.out)
    for probe in a.probe:
        hit = [f for f in named if f["start"] <= probe < f["start"] + f["size"]]
        if hit:
            h = hit[0]
            print(f"0x{probe:x} 落在 {h['name']}  (0x{h['start']:x} + 0x{probe-h['start']:x} / {h['size']} 字节)")
        else:
            print(f"0x{probe:x} 未落在任何 FDE 中")


if __name__ == "__main__":
    main()
