# 21 - EPON 驱动实现（原厂 349 条寄存器序列）

> 本文记录 EPON 驱动的最终实现方案，以及一次重要的**认知修正**。

## 一、认知修正：上游 EPON 并不缺协议逻辑

在动手写 EPON 之前，我的判断是「上游 `airoha-xpon-epon.c` 缺四块」：

| 我以为缺失 | 实际情况 |
|---|---|
| NACK 静默重试 | ✅ 已有 `AIROHA_EPON_NACK_SILENT_MS = 60000` |
| REREGISTER 处理 | ✅ 已有 `AIROHA_EPON_REGISTER_REREGISTER` |
| DENIED 状态 | ✅ 已有 `AIROHA_EPON_MPCP_DENIED` + `denied_until` |
| 时钟漂移补偿 | ✅ 已有 `apply_sync_time()` |

**上游 1007 行代码已经实现了完整的 MPCP 状态机**，包括
`WAIT → REGISTERING → REGISTER_REQUEST → REGISTER_PENDING → REGISTERED → DENIED`
全部流转，以及中断服务（fast / threaded / phy 三路）。

**教训**：判断「上游缺什么」必须读完代码，不能只看函数名列表就下结论。
我因此白写了 735 行重复实现（`airoha-xpon-epon-mpcp.c`），已删除。

## 二、真实缺口只有两个

### 缺口 1：原厂 349 条寄存器初始化序列

上游 `start_mac` 只写了约 15 个寄存器：

```c
AIROHA_EPON_TRX_ADJUST_TIME1/2   0x1e8/0x1ec
AIROHA_EPON_TX_FETCH_CFG         0x200
AIROHA_EPON_INT_ENABLE/2/3       0x014/0x01c/0x024
AIROHA_EPON_INT_STATUS/2/3       0x010/0x018/0x020
AIROHA_EPON_GLB_CFG / GLB_CFG2   0x000/0x004
AIROHA_EPON_REPORT_BITMAP/CFG    0x134/0x124
AIROHA_EPON_MPCP_TIMEOUT_INTERVAL 0x1d8
AIROHA_EPON_TX_CAL_CONST         0x230
```

而原厂 `xpon.ko` 有 **5 张完整表，共 349 条**，覆盖 `0x004–0x240`。
剩余 300+ 个寄存器在上游是「保持上电默认值」，原厂是「显式配置」。

这就是行为差异的来源：**同样的代码，寄存器没配全，某些 OLT 上会注册失败或链路不稳。**

### 缺口 2：ops 操作表

已在 `airoha-xpon-proto.c` 里实现（含正确的 OAM 控制通道），
本文档不再重复。

## 三、五张表的语义

从原厂 `xpon.ko` 逆向得到（详见 `docs/13-EPON寄存器表逆向.md`）：

| 表 | 条数 | 语义 | 使用时机 |
|---|---:|---|---|
| `epon_mac_table_init` | 112 | MAC 初始化序列 | 上电首次配置 |
| `epon_reg_def_val_table` | 127 | 寄存器默认值 | 上电首次配置 |
| `epon_reg_not_rst_table` | 60 | 非复位（**保留**） | 软复位时重写 |
| `epon_reg_rst_table` | 41 | 复位（**只写 mask**） | 软复位时清位 |
| `epon_log_rst_table` | 9 | 逻辑复位 | 掉线快速恢复 |

表项结构 `{offset, value, mask}`，写入规则：

```c
if (mask == 0)
        writel(value, base + offset);              /* 全字写 */
else
        writel((readl(base+offset) & ~mask) | (value & mask),
               base + offset);                      /* 读改写 */
```

**关键差异**：`epon_reg_rst_table` 的 `value` 恒为 0、只有 `mask`，
语义是「清除 mask 覆盖的位」，不是「写入 0」。这个差异最初让我的
提取器整张表漏掉（41 条全丢），修正后才拿到数据。

## 四、实现：airoha-xpon-epon-hw.c

### 对外接口

```c
int  airoha_epon_hw_init(struct airoha_xpon *xpon);        /* 112 + 127 条 */
int  airoha_epon_hw_soft_reset(struct airoha_xpon *xpon);  /* 41 + 60 条 */
int  airoha_epon_hw_logic_reset(struct airoha_xpon *xpon); /* 9 条 */
void airoha_epon_hw_debug(struct airoha_xpon *xpon, struct seq_file *m);
```

### 接入点：start_mac 的精确位置

```c
airoha_epon_write(xpon, AIROHA_EPON_TRX_ADJUST_TIME1, 0x0000fff6);
airoha_epon_write(xpon, AIROHA_EPON_TRX_ADJUST_TIME2, 0x00000006);
airoha_epon_write(xpon, AIROHA_EPON_TX_FETCH_CFG, 0x002a03e8);
xpon->epon.sync_time = AIROHA_EPON_SYNC_TIME_DEFAULT;

/* ★ 这里插入 hw_init(xpon) ★ */

/* PHY readiness gates EPON MAC logic reset release. */
regmap_clear_bits(xpon->scu, AIROHA_SCU_SSR3,
                  AIROHA_SCU_EPON_LOGIC_RESET);
udelay(1);
```

**为什么必须在这里**：表里有若干寄存器属于「复位期间锁存」类型，
必须在逻辑复位释放（`SCU_SSR3` 的 `EPON_LOGIC_RESET` 清零）**之前**写入。
释放后再写不生效。

### 调试能力

`hw_debug` 会逐个核对 MAC 表里 mask 非 0 的表项：

```
EPON register init tables:
  mac_table      112 entries
  def_val_table  127 entries
  ...

  offset    expect     actual     mask
  0x010    0x00000100 0x00000000 0x0000037f
  ...
  12 mismatched entries (read-only or self-clearing)
```

不匹配的表项通常是只读寄存器或自清零寄存器——这是排查
「原厂表与实际硬件差异」的第一手信息。

## 五、验证

### 静态编译

```
MinGW-w64 GCC 16.2.0 -Wall -Wextra:
  airoha-xpon-gpon.c      OK（零错误零警告）
  airoha-xpon-epon-hw.c   OK（零错误零警告）
  airoha-xpon-proto.c     OK（零错误零警告）
```

### 表数据自检

```
MAC_TABLE_INIT           声明= 112 实际= 112 ✅
REG_DEF_VAL_TABLE_INIT   声明= 127 实际= 127 ✅
REG_NOT_RST_TABLE_INIT   声明=  60 实际=  60 ✅
REG_RST_TABLE_INIT       声明=  41 实际=  41 ✅
LOG_RST_TABLE_INIT       声明=   9 实际=   9 ✅
合计 349 条，与逆向结果一致
```

### 补丁应用验证

```
git apply --check 通过
实际应用后：
  is_epon 从 17 处降到 1 处（仅剩注释）
  airoha_epon_hw_init 调用已插入 epon.c
  7 个新文件全部到位
```

## 六、待真机验证的点

1. **hw_init 的时序**：复位期间锁存的假设来自原厂反汇编，
   真机上若出现寄存器写入无效，需调整插入点
2. **349 条表的实际效果**：原厂表是针对 AN7581 的，
   若 naoki66 固件的 OLT 环境不同，可能需要微调
3. **软复位路径**：`hw_soft_reset` 目前没有调用点，
   需要确认原厂 `eponMacSwReset` 的触发条件后再接入

## 七、文件清单

| 文件 | 行数 | 说明 |
|---|---:|---|
| `airoha-xpon-epon-hw.c` | 276 | 寄存器序列 + 调试 |
| `airoha-xpon-epon-regs.h` | 541 | 349 条表数据（自动生成） |
| `tools/gen_epon_regs.py` | — | 表数据生成器 |
| `build/epon-tables.txt` | 349 | 原始提取数据 |

**已删除**：`airoha-xpon-epon-mpcp.c`（735 行，与上游重复）
