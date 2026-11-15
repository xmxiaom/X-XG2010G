# 06 · 逆向成果：GPON MAC 寄存器表已完整还原

对象：`/lib/modules/5.4.55/xpon.ko`（原厂 XG2010G_fw_1.0.0.13，aarch64，1,176,304 字节）
工具：`tools/re_xpon.py`（capstone + pyelftools）
产物：`src/airoha-xpon-gpon-regs.h`、`build/gpon-csr.csv`、`build/gpon-funcs.txt`

---

## 1. 突破口：这个模块根本没 strip

```
.symtab    size = 0x6d50  →  1165 个符号，其中 466 个 STT_FUNC 带名字和大小
.strtab    size = 0x3c56
```

意味着不需要从 `__ksymtab` 里猜函数边界，直接有 `gpon_init`、`gpon_act_change_gpon_state`、
`gponDevSendPloamMsg` 这些实名函数。**逆向难度比预想低一个数量级。**

### 关键全局变量（`.bss`）

| 符号 | 大小 | 含义 |
|---|---|---|
| `g_gpon_mac_reg_BASE` | 8 | **GPON MAC 基址指针** |
| `g_EPON_MAC_BASE` | 8 | EPON MAC 基址 |
| `xpon_regs_p` | 8 | 寄存器基址结构体指针 |
| `gpGponPriv` | 8 | GPON 私有状态（+4 = 状态号，+0x1dd = SN，+0x1f3 = 密钥） |
| `gpPonSysData` | 8 | PON 系统数据（+0x1b18 = 调试标志位） |
| `gponRecovery` | 3252 | 掉线恢复表（Alloc-ID / GEM Port 备份） |
| `pon_mode` | 1 | 线路模式（GPON/EPON） |

## 2. 方法：从 `gponDevDumpCsr` 反推整张表

`gponDevDumpCsr`（0x22a28，4452 字节）是一段**完全展开**的序列：

```
adrp x0, g_gpon_mac_reg_BASE      ; 取基址
add  x0, x0, #0
ldr  x1, [x0]                     ; x1 = MAC 基址
mov  x0, #0x40b0
add  x0, x1, x0                   ; x0 = base + 0x40b0
bl   get_xpon_data                ; 读寄存器
mov  w1, w0
adrp x0, #...                     ; 取格式串
add  x0, x0, #...
bl   printk                       ; "0x40B0: Vendor ID ... :0x%.8x"
```

格式串里**自带偏移和寄存器名**。把 "偏移 ↔ 格式串" 配对即可还原全表，
`tools/re_xpon.py` 就是做这件事，输出 110 条。

## 3. 地址换算（已交叉验证，非常重要）

三条来自调试字符串的**绝对地址**，与还原出的表逐条对上：

| 调试字符串里的绝对地址 | 减 0xBFB64000 | 还原表条目 | 是否一致 |
|---|---|---|---|
| `G_EQD (0xBFB640A8)` | 0x00A8 | `MAIN PATH EQUALIZATION DELAY` @ 0x00A8 | ✅ |
| `G_ACTIVATION (0xBFB640BC)` | 0x00BC | `ONU STATE IN ACTIVATION` @ 0x00BC | ✅ |
| `DBG_RX_GTC_CNT (0xBFB64308)` | 0x0308 | `RX GTC COUNTER` @ 0x0308 | ✅ |

由此确定：

```
g_gpon_mac_reg_BASE = 0xBFB60000（原厂虚拟地址）
GPON MAC 寄存器实际位于   base + 0x4000 .. base + 0x43c8
                       = 0xBFB64000 .. 0xBFB643C8
物理地址               = 0x1FB64000 .. 0x1FB643C8
```

**而设备树 `gpon` 块正是 `0x1fb64000`，长度 `0x3e8`（= 1000 字节）。**
寄存器表跨度 `0x0000–0x03c8`（968 字节），**刚好装得下。**

> 结论：`docs/05` 里担心的"窗口太小"不成立 —— **DTB 不需要改**，
> 直接把 `gpon` 块 ioremap 出来，用本头文件的偏移即可。原厂偏移减去 0x4000 就是这里的偏移。

## 4. 还原出的寄存器表（110 条，摘要）

完整清单见 `build/gpon-csr.csv` 与 `src/airoha-xpon-gpon-regs.h`。

### 4.1 身份与配置

| 偏移 | 名称 |
|---|---|
| `0x0000` | ONU ID |
| `0x0004` | ONU GLOBAL CONFIGURATION REGISTER |
| `0x0008` / `0x000c` | GPON MAC INTERRUPTS STATUS / ENABLE |
| `0x00b0` / `0x00b4` | **Vendor ID / Vendor-specific serial number** |
| `0x00b8` | SN_ONU MESSAGE INFORMATION |
| `0x00bc` | **ONU STATE IN ACTIVATION**（激活状态机） |
| `0x00ac` | ONU ACTIVATION PARAMETER |

### 4.2 测距与物理层开销（G.984.3 的 Upstream_Overhead 参数）

| 偏移 | 名称 |
|---|---|
| `0x0090` | PHYSICAL LAYER OVERHEAD TIME |
| `0x0094` | NUMBER OF GUARD BITS |
| `0x0098` | NUMBER OF TYPE1 and TYPE2 PREAMBLE BITS |
| `0x009c` | NUMBER OF TYPE3 PREAMBLE BITS |
| `0x00a0` | NUMBER OF DELIMITER BITS |
| `0x00a4` | PRE-ASSIGNED DELAY |
| `0x00a8` | **MAIN PATH EQUALIZATION DELAY（EqD）** |

### 4.3 PLOAM 与 GEM

| 偏移 | 名称 |
|---|---|
| `0x0050` | PLOAMu FIFO STATUS |
| `0x0058` | PLOAMd FIFO STATUS |
| `0x0048` | OMCI GEM PORT ID |
| `0x004c` | GEM TABLE INITIALIZATION CONTROL |
| `0x0020`–`0x003c` | Alloc-ID of T-CONT0..15（每寄存器两个 T-CONT） |
| `0x03c4` | **O3 and O4 US PLOAM CONTROL** |
| `0x0360` | PLOAMD FILTER IN O5 |

### 4.4 加密与 ToD

`0x0060` 超级帧计数器（AES 换钥）、`0x0064–0x0080` 4×AES 前向 + 4×影子、`0x0084` 软件触发换钥；
`0x00d0–0x00e4` ToD（新/当前秒、纳秒、时钟周期）、`0x03c8` 1PPS 脉宽。

### 4.5 计数与调试

`0x0300` RX GEM / `0x0304` RX CRC 错 / `0x0308` RX GTC / `0x030c` TX GEM / `0x0310` TX BST /
`0x0330–0x0338` GEM 头 1bit/2bit/不可纠错 / `0x0358` DS_SPF_CNT /
`0x0340–0x0354` DBA 内部调参 / `0x0374–0x0378` 上下行 GTC 提取以太网计数。

## 5. 函数清单（466 个，其中 180 个 GPON）

| 类别 | 数量 | 代表 |
|---|---|---|
| 寄存器/设备操作 | 87 | `gponDevSetSerialNumber`、`gponDevDumpCsr` |
| 其他 | 51 | `gpon_init`、`gpon_enable`、`gpon_deinit` |
| PLOAM | 12 | `gponDevSendPloamMsg`、`gpon_recv_ploam_message`、`gponDevSetO3O4PloamCtrl` |
| GEM | 9 | `gponDevSetGemInfo`、`gponDevUpdateGemMibIdxTable` |
| 激活状态机 | 7 | `gpon_act_change_gpon_state`(2000B)、`gpon_act_to1/to2_timer_expires` |
| 中断 | 4 | `gpon_isr`(4104B)、`register_gpon_isr` |
| 掉线恢复 | 4 | `gpon_recover_create/delete_allocId`、`_gemport` |
| 定时器 | 3 | `gpon_create_timer`、`gpon_stop_timer` |
| Alloc-ID | 3 | `gpon_recover_backup_allocId` 等 |

完整清单：`build/gpon-funcs.txt`（含地址与大小，可直接喂给 IDA/Ghidra 或 `objdump`）。

## 6. 对移植计划的影响（修正 `docs/05`）

| 原判断 | 修正后 |
|---|---|
| 寄存器映射要猜，风险高 | ✅ **110 个寄存器已全部还原并命名** |
| DTB `gpon` 窗口 0x3e8 可能不够 | ✅ **刚好够用，DTB 不用改** |
| 需要先做"探针"验证硬件 | ✅ 仍建议做，但目的从"摸寄存器"变成"验证时序" |
| 主要工作量在寄存器 | ❌ 寄存器已解决，**真正的难点是状态机时序与 DMB/BWMAP 交互** |

剩余需要逆向的部分（按难度排序）：

1. `gpon_act_change_gpon_state`（2000 字节）—— O1–O7 状态迁移条件与副作用
2. `gpon_isr`（4104 字节）—— 中断分类与 PLOAMd 处理
3. `gpon_recv_ploam_message`（204 字节）—— PLOAM 消息分派表
4. `gponDevSendPloamMsg`（352 字节）—— PLOAMu 组装（含 MIC）
5. DBA / BWMAP 处理 —— `0x0220–0x0234` 相关寄存器与 `gpon_qos_init`

## 7. 复现方式

```bash
pip install capstone pyelftools
python tools/re_xpon.py /path/to/xpon.ko --out-dir .
# 输出：src/airoha-xpon-gpon-regs.h / build/gpon-csr.csv / build/gpon-funcs.txt
```

脚本不依赖任何商业工具，纯静态分析，可重复执行。
