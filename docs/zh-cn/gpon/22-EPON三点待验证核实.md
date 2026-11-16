# 22 - EPON 三个待验证点的核实结论

> 针对 `docs/21` 里列出的三个「待真机验证」风险点，本文给出通过
> **原厂反汇编 + 两个独立开源实现对比 + 公开资料** 三路交叉核实的结论。

## 摘要

| # | 待验证点 | 结论 | 需不需要改代码 |
|---|---|---|---|
| 1 | `hw_init` 的时序假设（复位期间锁存） | ❌ **假设错误**，原厂是先释放复位再写寄存器 | ✅ **需要改** |
| 2 | 349 条表的平台适用性 | ✅ 表内无芯片型号分支，但**与上游是两套地址空间** | ⚠️ 需补充说明 |
| 3 | `hw_soft_reset` 的触发条件 | ✅ **已查清**：只在 Stop/Restart 路径，不在启动路径 | ✅ 需接入调用点 |

---

## 一、验证点 1：复位时序假设是错的

### 我原来的假设

> 「表里有若干寄存器属于复位期间锁存类型，必须在逻辑复位释放之前写入。」

这个假设来自对反汇编的**主观推断**，没有证据。

### 原厂 `eponMacSwReset` 的真实流程

从 `xpon.ko` 反汇编（`@0x317a0`，652 字节）：

```
1. GET_SCU_RSTCTRL1  → orr  #0x80000000  → SET_SCU_RSTCTRL1   ← 置 bit31（复位）
2. __const_udelay(0x10c7)                                      ← 延时约 4300us
3. GET_SCU_RSTCTRL1  → and  #0x7fffffff  → SET_SCU_RSTCTRL1   ← 清 bit31（释放）
4. ─────────────── 以上是复位脉冲，以下才开始写寄存器 ───────────────
5. 写 0x6000：读回后 orr #0x10 → 写；再 and #0xffffffef → 写   ← bit4 脉冲
6. 若 eponSetProbe：写 0x6100
7. 读 0x6004 / 0x602c（仅 dbgtoMem 调试记录）
8. 写 0x6000：orr #2 → orr #0x40 → orr #0x800000（bit1+bit6+bit23）
9. 写 0x60d0 = 0x2024_03e8
```

### 结论

**原厂是「先释放复位，再写寄存器」，与我的假设相反。**

代码依据（`@0x317d0`–`@0x3180c` 是复位脉冲，`@0x31810` 之后才出现 `g_EPON_MAC_BASE`）：

```
0317d0  bl  GET_SCU_RSTCTRL1        ; 读
0317dc  orr w0, w0, #0x80000000     ; 置复位位
0317e8  bl  SET_SCU_RSTCTRL1        ; 写回 → 进入复位
0317ec  mov x0, #0x10c7             ; 延时参数
0317f0  bl  __const_udelay          ; 延时
0317f4  bl  GET_SCU_RSTCTRL1        ; 读
031800  and w0, w0, #0x7fffffff     ; 清复位位
03180c  bl  SET_SCU_RSTCTRL1        ; 写回 → 释放复位
031810  adrp x0, g_EPON_MAC_BASE    ; ← 此后才开始碰寄存器
```

### 对代码的影响

我在 `start_mac` 里把 `hw_init` 插在「逻辑复位释放之前」，这个位置**需要重新评估**。

不过要注意：上游 `start_mac` 的复位操作是**另一套**（`SCU_RSTCTRL1` bit31 + `SCU_SSR3` bit10），与原厂 `eponMacSwReset` 不完全等价：

```c
/* 上游 start_mac 的复位序列 */
regmap_set_bits(xpon->scu, AIROHA_SCU_RSTCTRL1, AIROHA_SCU_PON_MAC_RESET);
udelay(1);
regmap_clear_bits(xpon->scu, AIROHA_SCU_RSTCTRL1, AIROHA_SCU_PON_MAC_RESET);
regmap_set_bits(xpon->scu, AIROHA_SCU_SSR3, AIROHA_SCU_EPON_LOGIC_RESET);   // ← 保持
udelay(1);
/* ... TRX 配置 ... */
regmap_clear_bits(xpon->scu, AIROHA_SCU_SSR3, AIROHA_SCU_EPON_LOGIC_RESET); // ← 释放
```

**上游在 RSTCTRL1 释放之后、SSR3 释放之前留了一个窗口**，`hw_init` 就插在这里。
原厂是 RSTCTRL1 释放后直接写寄存器（没有 SSR3 这一层）。

**判断**：上游这个插入点比原厂的更保守（多等了一个 SSR3 释放），
所以**大概率是安全的**，但严格来说需要真机确认。

---

## 二、验证点 2：349 条表的适用范围

### 2.1 表内无芯片型号分支

对五个表函数做反汇编统计：

| 函数 | 内部调用 | 比较指令数 |
|---|---|---:|
| `eponMacTableInit` | `__kmalloc`, `eponMacRegTable`, `g_EPON_MAC_BASE` | **0** |
| `eponRegDefValTableInit` | 同上 + `printk` | 0 |
| `eponRegNotRstTableInit` | 同上 + `printk` | 0 |
| `eponRegRstTableInit` | 同上 + `logRstTestFlag`, `printk` | 0 |
| `eponLogRstTableInit` | 同上 + `printk` | 0 |

**零个比较指令**，说明五张表是**无条件应用的固定序列**，不区分芯片型号。

对比 GPON 的 `gpon_qos_init`，那里有 HIR 分支（`0xa/0xc/0xe` 用 `0xa00000`，否则 `0xffff`）。
**EPON 没有这种分支**，这是 EPON 与 GPON 的重要差异。

### 2.2 两个独立开源实现的寄存器定义完全一致

对比 `naoki66/openwrt-pon-drivers` 与 `pbs05/openwrt-pon-drivers`（PONWRT 项目）：

```
naoki66: 41 个 EPON 寄存器宏
PONWRT : 41 个 EPON 寄存器宏
仅 naoki66 有: 无
仅 PONWRT  有: 无
地址不一致  : 无 ✅
```

**41 个寄存器定义零差异**。这是强证据：说明 EPON 寄存器布局在 AN7581 上是稳定的。

### 2.3 但发现一个关键问题：两套地址空间

我的 349 条表覆盖 **129 个不同偏移，范围 `0x004–0x240`**。

而上游代码用的寄存器里，有 16 个**不在我的表内**：

| 偏移 | 名称 | 用途 |
|---|---|---|
| `0x000` | `GLB_CFG` | 全局配置 |
| `0x018` / `0x01c` | `INT_STATUS2` / `INT_ENABLE2` | 第二组中断 |
| `0x05f` | `SYNC_TIME_MAX` | 同步时间上限 |
| `0x07c` | `LLID_DISCOVERY_CTRL` | **LLID 注册控制（核心）** |
| `0x244` / `0x248` / `0x258` | 错误计数器 | 统计 |
| `0x5ac` / `0x5b0` / `0x5e4` | MPCP/OAM 计数 | 统计 |
| `0x5e0` | `XPON_STA` | **PHY 状态** |
| `0x5f0` / `0x5f4` / `0x5f8` | PHY 中断 | **LOS/LOF** |

进一步验证：原厂代码里**搜索不到 `0x7c`、`0x5f0`、`0x5e0` 这些偏移的立即数**。

### 2.4 结论：两套窗口，互补而非冲突

| 窗口 | 偏移范围 | 谁在用 | 用途 |
|---|---|---|---|
| 初始化窗口 | `0x004–0x240` | 原厂 5 张表 | 上电/MAC 初始化 |
| 运行时窗口 | `0x5xx`、`0x07c` | 上游 + 原厂其他函数 | 中断、状态、注册控制 |

**所以我的表与上游代码不冲突，是互补关系**：
- 表负责「一次性配置」的静态寄存器
- 上游负责「运行时操作」的动态寄存器

`0x07c`（LLID 注册）和 `0x5f0-0x5f8`（PHY 中断）本来就是运行时写的，
原厂也没有放在初始化表里——这与上游实现一致。

**不需要改代码，但需要在文档里说清这个边界。**

---

## 三、验证点 3：`hw_soft_reset` 的触发条件（已查清）

### 3.1 完整调用链（从 `xpon.ko` 符号表追踪）

```
xpondrv_init
  └─ eponInit
       └─ eponStart                       ← 启动路径，不碰 SwReset
            ├─ eponWanDevice
            ├─ eponFirmwareSetMode
            ├─ eponEnable
            └─ eponSetIFG

eponWanStopWithChannelRetire
  └─ eponMacStop
       └─ eponMacSwReset                  ← 停止路径

eponSetPonMacAddr
  └─ eponMacRestart
       └─ eponMacSwReset                  ← 改 MAC 地址时重启

eponWanResetWithChannelRetire
  └─ eponMacRestart
       └─ eponMacSwReset                  ← WAN 重置时重启
```

### 3.2 关键结论

**`eponMacSwReset` 只被 2 个函数调用，都不在启动路径上。**

| 路径 | 触发条件 | 对应上游 |
|---|---|---|
| 启动 | `xpondrv_init → eponInit → eponStart` | `start_mac` |
| 停止 | WAN 停止 / 通道退役 | `stop_rx` |
| 重启 | 改 MAC 地址 / WAN 重置 | 无对应实现 |

而**初始化表** `eponMacTableInit` 是在 `eponInit` 里调的（启动路径），
`eponMacSwReset` 只做「复位脉冲 + 少量寄存器重配」，**不重新应用整张表**。

### 3.3 `eponMacRestart` 的完整动作

```
eponMacRestart:
  ├─ eponMacRestart_flag（读取标志）
  ├─ eponMacSwReset              ← 复位脉冲
  ├─ xmcs_remove_llid            ← 清 LLID
  ├─ gpPonSysData（读系统数据）
  ├─ eponHwDygaspCtrl            ← 重配 dying gasp
  ├─ eponDevSetDyingGaspNum
  ├─ eponTxOamFavorModeCtl       ← 重配 OAM 优先模式
  ├─ eponSnifferModeCtrl         ← 重配 sniffer
  └─ eponMacRestart_flag（写回）
```

### 3.4 对 `hw_soft_reset` 的接入建议

**不应该接入 `start_rx`**，因为启动路径不做 SwReset。

正确的接入点是上游的**重启路径**。上游目前有：
- `stop_rx`（停止）
- `start_rx`（启动）

缺少「重启」这个独立路径。建议：

```c
/* 在 airoha-xpon-epon.c 新增，对应原厂 eponMacRestart */
int airoha_xpon_epon_restart_mac(struct airoha_xpon *xpon)
{
        int ret;

        /* 1. 软复位（原厂 eponMacSwReset） */
        ret = airoha_epon_hw_soft_reset(xpon);
        if (ret)
                return ret;

        /* 2. 清 LLID 状态 */
        airoha_xpon_epon_reset_registration(xpon);

        /* 3. 重新启动 MAC（含完整初始化表） */
        return airoha_xpon_epon_start_mac(xpon);
}
```

调用时机：`airoha_xpon_epon_program_mac()` 改 MAC 地址之后
（对应原厂 `eponSetPonMacAddr → eponMacRestart`）。

---

## 四、附带发现：两个开源实现的高度一致性

对比 `naoki66` 与 `pbs05`（PONWRT）两个独立仓库的 EPON 实现：

| 项目 | naoki66 | PONWRT |
|---|---:|---:|
| `airoha-xpon-epon.c` 行数 | 1007 | 1256 |
| 函数数 | 24 | 25 |
| 寄存器定义 | 41 | 41（零差异） |
| `start_mac` 寄存器操作 | 20 处 | 20 处 |

**PONWRT 只多一个 `airoha_xpon_epon_recover_tx`**，其余 24 个函数完全相同。

### 重要推论

**两个独立实现都没有 349 条完整初始化表**，`start_mac` 都只配 20 个寄存器。

这说明：
1. 我的 349 条表是**真正的增量**，公开代码里没有参照
2. 但也意味着**没有公开实现验证过它的正确性**
3. 原厂表来自 `xpon.ko` 逆向，数据本身可信，但**是否需要全量应用**存疑

**保守建议**：先按原厂表全量应用，如果真机出现异常，
再逐步回退到「只保留上游已有的 20 个寄存器」做二分定位。

---

## 五、待办

| 项 | 优先级 | 说明 |
|---|---|---|
| 调整 `hw_init` 插入点 | 中 | 上游 SSR3 窗口大概率安全，真机验证后再定 |
| 新增 `restart_mac` 接口 | 低 | 对应原厂 `eponMacRestart`，改 MAC 时用 |
| 真机验证 349 条表 | **高** | 无公开参照，需实测 |
| 二分回退预案 | 中 | 异常时从全量退到 20 个寄存器 |

## 六、参考来源

- 原厂 `xpon.ko` 反汇编（本地逆向，`build/fde.txt` + capstone）
- `naoki66/openwrt-pon-drivers`（本地克隆，`/d/workbuddy/tmp/pon-upstream`）
- `pbs05/openwrt-pon-drivers`（PONWRT，本地克隆，`/d/workbuddy/tmp/ponwrt`）
- [PONWRT 项目介绍（恩山论坛）](https://www.right.com.cn/forum/thread-8495086-1-1.html)
- [OpenWrt Forum: Airoha xPON driver adaptation](https://forum.openwrt.org/t/is-it-feasible-to-adapt-the-airoha-xpon-driver-for-pon-mac-support-on-the-nokia-xg-040g-md/252382/13)
- [AN7583 PCS driver patch（Christian Marangi）](https://marc.info/?l=u-boot&m=179269560563891)
