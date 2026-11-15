# EPON 寄存器表逆向（含与上游开源实现的交叉验证）

> 对象：原厂 `xpon.ko` 中 5 个巨型数据表函数，共 57,300 字节
> 方法：Capstone 反汇编 + 表项模式识别（`{addr, value, mask}` 三元组）
> **重点**：与上游 `airoha-xpon-epon.c`（开源实现）逐项对照，验证逆向准确性

---

## 1. 结论先行：逆向方法得到验证

| 验证项 | 原厂逆向结果 | 上游开源实现 | 结论 |
|---|---|---|---|
| MAC 基址 | 表项偏移范围 `0x6004–0x6240` | 注释 `epon_base starts at XEPON_MAC_BASE + 0x6000` | ✅ **完全吻合** |
| 中断状态寄存器 | `0x6010` 值 `0x100` 掩码 `0x37f` | `AIROHA_EPON_INT_STATUS = 0x010` | ✅ 吻合（10 位宽） |
| 中断使能寄存器 | `0x6014` 值 `0x40` 掩码 `0x7f` | `AIROHA_EPON_INT_ENABLE = 0x014` | ✅ 吻合（7 位宽） |
| LLID 发现控制 | `0x607c` | `AIROHA_EPON_LLID_DISCOVERY_CTRL = 0x07c` | ✅ 吻合 |
| MAC 地址配置 | `0x6104` | `AIROHA_EPON_MAC_ADDR_CFG = 0x104` | ✅ 吻合 |
| MAC 地址值 | `0x6108` | `AIROHA_EPON_MAC_ADDR_VALUE = 0x108` | ✅ 吻合 |
| MPCP 超时间隔 | `0x61d8` 值 `0x00010000` | `AIROHA_EPON_MPCP_TIMEOUT_INTERVAL = 0x1d8` | ✅ 吻合 |

**这七项独立吻合，说明「表项模式识别 + 偏移还原」这套方法对 GPON 的逆向结果同样可信。**

---

## 2. 五张表的性质

原厂把寄存器初始化拆成 5 张表，用途不同：

| 函数 | 大小 | 条目 | 地址范围 | 用途 |
|---|---:|---:|---|---|
| `eponMacTableInit` | 18744 B | **112** | `0x6004–0x6238` | **MAC 初始化序列**（启动时下发） |
| `eponRegDefValTableInit` | 21196 B | **127** | `0x6004–0x6240` | 寄存器默认值表 |
| `eponRegNotRstTableInit` | 10372 B | **60** | `0x6008–0x6240` | 非复位寄存器（复位时保留） |
| `eponRegRstTableInit` | 5320 B | **41** | `0x6014–0x6228` | 复位寄存器（复位时清零） |
| `eponLogRstTableInit` | 1668 B | **9** | `0x602c–0x60d4` | 逻辑复位寄存器 |

**合计 349 条表项。**

### 2.1 表项结构（12 字节）

```c
struct epon_reg_entry {
        u32 addr;       /* g_EPON_MAC_BASE + offset */
        u32 value;      /* 默认值 */
        u32 mask;       /* 写入掩码 */
};
```

原厂用 `kmalloc(n * 12)` 分配，然后逐条填充：

```asm
mov  x0, #0x6014          ; 偏移
add  x0, x1, x0           ; + BASE
mov  w3, x0
str  w1, [x0]             ; [0] = addr
mov  w1, #0xff00
str  w1, [x0, #4]         ; [4] = value
str  w1, [x0, #8]         ; [8] = mask
```

### 2.2 一个重要的语义差异

**`eponRegRstTableInit` 只写掩码、不写值**（41 条全部如此）：

```asm
mov  w1, w3
str  w1, [x0]             ; [0] = addr
                          ; 跳过 [4]，不写 value
mov  w1, #0xff00
str  w1, [x0, #8]         ; [8] = mask
```

这意味着复位表的语义是「**只把掩码覆盖的位清零**」，而不是「写入某个值」。
我的提取器最初漏掉了这一整张表（因为要求先见到 `#4` 才建行），修正后才拿到 41 条。

---

## 3. MAC 初始化序列（112 条，最有用）

按功能分组的关键表项：

### 3.1 全局配置与中断（`0x6004–0x6030`）

| 偏移 | 默认值 | 掩码 | 说明 |
|---|---|---|---|
| `0x6004` | `0x00000000` | `0x00000000` | GLB_CFG（掩码 0 = 不修改） |
| `0x6008` | `0x00000000` | `0x00000000` | GLB_CFG2 |
| `0x600c` | `0x00000000` | `0x00000000` | 保留 |
| `0x6010` | `0x00000100` | `0x0000037f` | **INT_STATUS**（10 位） |
| `0x6014` | `0x00000040` | `0x0000007f` | **INT_ENABLE**（7 位） |
| `0x6020` | `0x00000000` | `0x0f0f0f0f` | 四组 4 位字段 |
| `0x6024` | `0x00000000` | `0x0f0f0f0f` | 同上 |
| `0x6028` | `0x00010000` | `0x00001107` | 含 bit16 标志 |
| `0x602c–0x6048` | `0` | `0xc0000000` | **8 个连续寄存器，只清 bit30/31** |

`0x602c–0x6048` 这 8 个连续寄存器掩码全是 `0xc0000000`，是**一组状态/计数器寄存器的清零操作**。

### 3.2 定时器与门限（`0x60d0–0x6100`）

| 偏移 | 默认值 | 掩码 | 推测含义 |
|---|---|---|---|
| `0x60d0` | `0x000003e8` | `0xffff0fff` | **1000**（十进制）= 1ms 或 1s 基准 |
| `0x60d8` | `0x0000010c` | `0xffffff3f` | 268 |
| `0x60dc` | `0x00002020` | `0x0000ffff` | 两个字节 0x20/0x20 |
| `0x60e0` | `0x00000008` | `0x000000ff` | 8 |
| `0x60e4` | `0x0000aca0` | `0xffffffff` | 44192 |
| `0x60e8` | `0x0000af08` | `0x00ffffff` | 44808 |
| `0x60ec` | `0x0000aca0` | `0xffffffff` | 44192（与 e4 相同） |
| `0x60f0` | `0x00000400` | `0x0000ffff` | 1024 |
| `0x60f4` | `0x0000fff1` | `0xffffffff` | 65521 |
| `0x60f8` | `0x00000006` | `0xffffffff` | 6 |

`0x60e4/0x60e8/0x60ec` 是 MPCP 时间基准相关（44192 / 44808 是 16 位时间戳常量）。

### 3.3 LLID 与 MPCP（`0x6160–0x61e8`）

| 偏移 | 默认值 | 掩码 | 说明 |
|---|---|---|---|
| `0x6160` | `0x00000000` | `0xffffffff` | 清零 |
| `0x6164` | `0x01ff0000` | `0x01ffffff` | 高 9 位 |
| `0x616c` | `0x00000300` | `0xffffffff` | bit8/9 |
| `0x6170` | `0x00000110` | `0xffffffff` | — |
| `0x6174` | `0x01000000` | `0xffffffff` | bit24 |
| `0x6178` | `0x0000ee00` | `0xffffffff` | — |
| `0x617c` | `0x00000022` | `0xffffffff` | — |
| `0x6180` | `0x00000210` | `0xffffffff` | — |
| `0x6184` | `0x01000000` | `0xffffffff` | 与 0x6174 相同 |
| `0x6188` | `0x0000ee00` | `0xffffffff` | 与 0x6178 相同 |
| `0x618c` | `0x13250000` | `0xffffffff` | — |
| `0x61d0` | `0x00002d00` | `0xffffffff` | — |
| `0x61d4` | `0x00000040` | `0x000000ff` | 64 |
| `0x61d8` | `0x00010000` | `0xffffffff` | **MPCP_TIMEOUT_INTERVAL** |
| `0x61e0–0x61ec` | `0x01ff01ff` | `0x01ff01ff` | **4 个连续，LLID 位图** |

`0x6174/0x6184` 和 `0x6178/0x6188` 成对相同，说明是**两组对称的配置**（可能是 Tx/Rx 各一组）。

`0x61e0–0x61ec` 四个寄存器都是 `0x01ff01ff`，掩码也是 `0x01ff01ff`——
这是 **LLID 0–7 的有效位图**，每个寄存器放两个 LLID 的 9 位标志。

### 3.4 其他关键项

| 偏移 | 默认值 | 掩码 | 说明 |
|---|---|---|---|
| `0x6088` | `0x0000000d` | `0x00000000` | 13（值写但掩码 0） |
| `0x60a4` | `0x00000000` | `0x000000ff` | — |
| `0x60b0` | `0x00000000` | `0x3700ffff` | — |
| `0x6100` | `0x00000000` | `0x00000f1f` | 5 位 + 4 位字段 |
| `0x612c` | `0x0000ffff` | `0x00000000` | 值写但掩码 0 |
| `0x6230` | `0x00005500` | `0xffffffff` | — |
| `0x6238` | `0xd5000000` | `0xff000007` | 高字节 0xd5 + 低 3 位 |

---

## 4. 与上游开源实现的三处差异（重要）

### 4.1 中断寄存器：上游用 `0x5f0`，原厂表用 `0x6010`

| 上游定义 | 偏移 | 原厂表 |
|---|---|---|
| `AIROHA_GEPON_INT_ENABLE` | `0x5f0` | ❌ 表里没有 |
| `AIROHA_GEPON_INT_CLEAR` | `0x5f4` | ❌ 表里没有 |
| `AIROHA_GEPON_INT_STATUS` | `0x5f8` | ❌ 表里没有 |
| `AIROHA_EPON_INT_STATUS` | `0x010` | ✅ `0x6010` |
| `AIROHA_EPON_INT_ENABLE` | `0x014` | ✅ `0x6014` |

**结论：EPON 有两套中断寄存器。**
- `0x5f0/0x5f4/0x5f8`（`AIROHA_GEPON_*`）：用于 **LOS/LOF/PHY_READY** 等物理层事件
- `0x6010/0x6014`（`AIROHA_EPON_*`）：用于 **MAC 层事件**

原厂初始化表只覆盖 MAC 层那套，物理层那套由 `eponDetectPhyLosHandler` 单独处理。
**移植时必须两套都配置，只配一套会丢事件。**

### 4.2 `AIROHA_GEPON_GPON_MODE = BIT(31)`

上游在 `0x138`（`XPON_SETTING`）定义了 `GPON_MODE` 位。而原厂表里 `0x6138` 的值是 `0x00000000`、掩码 `0xffffffff`——**初始化时清零，即默认 EPON 模式**。

这与 GPON 逆向的结论一致：GPON 模式需要在初始化后单独置位。

### 4.3 上游未覆盖的寄存器

原厂表有 112 条，上游 `epon.c` 只定义了约 25 个寄存器宏。**上游覆盖了关键路径，但漏掉大量配置项**：

- `0x6020/0x6024` 的四组 4 位字段
- `0x602c–0x6048` 的 8 个 bit30/31 清零
- `0x60d0–0x60f8` 的定时器/门限组
- `0x6160–0x618c` 的 MPCP 配置组

这些缺失可能导致上游实现在某些 OLT 上注册失败或性能异常。

---

## 5. 对 GPON 适配的价值

### 5.1 验证了逆向方法

这是最直接的价值。GPON 的 112 个寄存器也是用同一套方法逆向出来的，
现在 EPON 的结果与独立来源（上游开源代码）七项吻合，**说明 GPON 的寄存器表可信**。

### 5.2 提供了「寄存器表」这个抽象

原厂用 `{addr, value, mask}` 三元组表来初始化，而不是散落的 `writel`。
移植时应该沿用这个模式：

```c
static const struct airoha_gpon_reg_init gpon_init_seq[] = {
        { 0x0004, 0x00000000, 0x000000ff },
        { 0x0008, 0x00000000, 0xffffffff },
        ...
};

for (i = 0; i < ARRAY_SIZE(gpon_init_seq); i++) {
        u32 v = readl(base + e->addr);
        v = (v & ~e->mask) | (e->value & e->mask);
        writel(v, base + e->addr);
}
```

比逐条写 `writel` 更清晰，也更容易与原厂对照。

### 5.3 揭示了 EPON/GPON 的共用结构

两者都用 `0x6xxx` 这段 MAC 窗口（EPON 用 `epon` 块 `0x1fb66000`，GPON 用 `gpon` 块 `0x1fb64000`），
**偏移规律相似但内容不同**。这意味着：

- 寄存器访问函数可以共用（只需换 base）
- 但初始化序列必须各自独立

---

## 6. 产物

```
build/epon-tables.txt        349 条表项（5 张表，含 offset/value/mask）
tools/re_epon_tables.py      提取器，可重复执行
```

重新生成：

```bash
python tools/re_epon_tables.py xpon.ko --out build/epon-tables.txt
python tools/re_epon_tables.py xpon.ko --filter MacTable   # 只提取一张表
```

---

## 7. 下一步

寄存器表已完整，接下来要逆向 EPON 的**协议层**：

| 优先级 | 内容 | 函数 |
|---|---|---|
| P0 | MPCP 注册状态机 | `eponMpcpDiscvGateIntHandler`(2092B)、`eponMpcpRgstIntHandler`(1708B) |
| P0 | MPCP 报文收发 | `eponMpcpRgstReqIntHandler`、`eponMpcpRgstAckIntHandler` |
| P1 | LLID 去注册 | `eponMpcpLocalDergstr`(552B) |
| P1 | 中断与 LOS | `eponIsr`(1120B)、`eponDetectPhyLosHandler`(208B) |
| P2 | 队列门限 | `epon_set_queue_threshold`、`eponSetLlidQueThrod` |

MPCP 是 EPON 的「PLOAM 等价物」，把这部分逆向完，
GPON 和 EPON 的适配就有了统一的方法论参照。
