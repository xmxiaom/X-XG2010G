# LOS/LOF 检测逆向（EPON + GPON 双份）

> **本篇是意外收获**：原本只想逆向 EPON 的 LOS 检测，作为 GPON 的参照，
> 结果在符号表里直接找到了 `gpon_detect_los_lof`（`0x01ab04`，788 字节），
> **GPON 最后一个主要缺口就此补上**。

---

## 1. 结论：两套实现，同一个 PHY

| 项 | EPON | GPON |
|---|---|---|
| 入口 | `eponDetectPhyLosLof` (`0x032e08`) | **`gpon_detect_los_lof` (`0x01ab04`)** |
| 大小 | 76 B（薄封装） | 788 B（完整逻辑） |
| 实际处理 | `eponDetectPhyLosHandler` (`0x032b98`) | 内联在同一函数 |
| PHY 状态来源 | `gpPhyData->[+0x30]` bit8–10 | 同 |
| 模式字段 | `gpPhyData->[+0x30]` bit3–6 | 同 |

**关键：两者共用同一个 `gpPhyData` 结构**，只是模式字段不同值（EPON=0，GPON=1）。

---

## 2. `gpPhyData->[+0x30]` 的位布局（核心）

这是一个 16 位字段，两个协议共用：

```
 bit  15 14 13 12 11 10  9  8 | 7  6  5  4  3 | 2  1  0
      ────────────────────────┼───────────────┼─────────
              PHY 状态        |   模式(bit3-6)|  标志位
              (bit8-10)       |               |
```

### 2.1 模式字段（bit3–6）

```c
mode = (gpPhyData->flags30 & 0x78) >> 3;

mode == 0  →  EPON
mode == 1  →  GPON
mode == 10 (0xa) → auto（EPON 连接建立后回退到 auto）
```

### 2.2 PHY 状态字段（bit8–10）

```c
phy_state = (gpPhyData->flags30 >> 8) & 0x7;
```

从代码里的比较值推断：

| 值 | 含义 | 依据 |
|---:|---|---|
| `0` | 无光 / LOS | `eponDrv->[0x18] == 0` 时走 EPON 分支 |
| `8`（即 bit11） | **PHY READY** | `eponDrv->[0x18] == 8` 判定同步成功 |

**注意**：`& 0xfffff8ff` 是**清除 bit8–10**（`0xfffff8ff` = `~0x700`）：
```c
gpPhyData->flags30 &= ~0x700;   // 清 PHY 状态字段
```

---

## 3. GPON 的完整实现（`0x01ab04`，788 字节）

这是本轮最有价值的成果，**逐行还原**：

```c
void gpon_detect_los_lof(u32 arg)
{
        struct airoha_gpon_priv *priv = gpGponPriv;

        /* --- 1. 调试日志：打印 GEM 头错误计数器 --- */
        if (debug_bitmap & 0x1004)
                printk("[%lu0ms]GPON IRQ: gpon detect los lof.",
                       jiffies);

        if (debug_bitmap & 0x1) {
                printk("Rx Loss of GEM Delineation: %d",
                       get_xpon_data(BASE + 0x4330));   /* GEM 头 1bit 错误 */
                printk("Loss of GEM delineation interrupt: %d",
                       get_xpon_data(BASE + 0x4334));   /* GEM 头 2bit 错误 */
                printk("...", get_xpon_data(BASE + 0x4338)); /* 不可纠错 */
        }

        /* --- 2. 状态分支 --- */
        state = priv->state;            /* priv->[+0x04] */

        if (state == 2 || state == 3 || state == 4) {
                /* O2 / O3 / O4：还没上线，直接重来 */
                if (arg == 1)
                        return;         /* arg==1 表示「忽略」，直接返回 */

                gpPonSysData->[0] = 0;          /* 清模式 */
                gpPhyData->flags30 &= ~0x700;   /* 清 PHY 状态 */
                xmcs_report_event(1, 0, 0);     /* 上报 LOS 事件 */
                fiber_hotplug_event(0);

                if (xpon_mac_print_open)
                        printk("...LOS in O%d...", state);

                gpon_disable_with_option(0);    /* 关 MAC */
        } else if (state == 5) {
                /* O5：已上线，需要区分「真掉线」和「误报」 */
                if (priv->[+0x30c] & 0x8) {
                        /* 特殊标志位置位 → 忽略本次 LOS */
                        if (arg == 1)
                                return;
                } else if (arg == 1) {
                        return;
                }

                gpPhyData->flags30 &= ~0x700;
                is_hwnat_dont_clean = 1;
                /* 清硬件 NAT 表，避免残留会话 */

                gpon_act_change_gpon_state(6);  /* → O6 POPUP */

                if (debug_bitmap & 0x4)
                        printk("...PHY STATUS LOS, Ignore Receive Popup message...");

                disable_cpu_us_traffic();        /* 停 CPU 上行流量 */
                gpon_typeb_dev_reset_for_typeB();/* TypeB 保护倒换 */
        }

        /* --- 3. 清 LOS 标记 --- */
        priv->[+0x2fc] &= ~0x4;      /* 清 bit2 */
}
```

### 3.1 三个状态分支的语义

| 状态 | 处理 | 原因 |
|---|---|---|
| **O2 / O3 / O4** | 直接回 O1（`gpon_disable_with_option(0)`） | 还没上线，没有业务要保，重来即可 |
| **O5** | 进 O6 POPUP，等 TO1 超时 | 已上线，先尝试保持，超时才回 O1 |
| 其他 | 只清标记 | 无需处理 |

**这解释了 GPON 的「先 POPUP 再重连」机制**：O5 掉光不会立刻断业务，
而是进 O6 等 TO1（默认几十秒），期间如果光恢复就能无缝继续。

### 3.2 `arg == 1` 的含义

`gpon_detect_los_lof(1)` 表示「**忽略本次 LOS**」，用于：
- 已知的误报（例如 OLT 侧维护）
- 主动关闭光模块时的预期 LOS

原厂在 `gpon_detect_los_lof` 的调用点用 `arg` 区分「真事件」和「预期事件」。

### 3.3 调试位图的作用

```
debug & 0x1004  → 打印入口日志
debug & 0x1     → 打印 GEM 错误计数器（0x4330/0x4334/0x4338）
debug & 0x4     → 打印 O5 掉光细节
```

这与 `gpon_isr` 的调试机制一致（`docs/10` 第 4.1 节）。

---

## 4. EPON 的实现（`0x032e08` + `0x032b98`）

### 4.1 `eponDetectPhyLosLof`（76 字节，薄封装）

```c
void eponDetectPhyLosLof(void)
{
        if (turnOffOpticalModuel)
                change_alarm_led_status(1);    /* 光模块已关，LED 状态 1 */
        else
                change_alarm_led_status(2);    /* LED 状态 2（告警） */

        if (phy_state_check() == 0)            /* 见 4.2 */
                eponDetectPhyLosHandler();
}
```

### 4.2 `phy_state_check`（`0x032cc8`，320 字节）

```c
int phy_state_check(void)
{
        if (eponDrv->[+0x04] == 0)             /* 未使能 */
                return 0;

        if (eponDrv->[+0x18] != 8)             /* 不是 PHY_READY 状态 */
                return 0;

        /* 确实是 PHY_READY，但收到 LOS —— 矛盾，需要处理 */
        gpPhyData->flags30 &= ~0x700;          /* 清 PHY 状态 */
        del_timer_sync(&pon_phy_sync_timer);   /* 停同步定时器 */

        /* 重启同步定时器：超时值 = eponDrv->[+0x06] 毫秒 */
        mod_timer(&pon_phy_sync_timer,
                  jiffies + eponDrv->[+0x06] * 1000 / 8);

        eponDrv->[+0x08] = 1;                  /* 标记「等待同步」 */
        return 1;
}
```

**注意 `* 1000 / 8` 的换算**——与 GPON 状态机的 TO1/TO2 完全相同（`docs/08` 第 8.5 节）。
这说明两者共用同一套定时器换算约定。

### 4.3 `eponDetectPhyLosHandler`（208 字节）

```c
void eponDetectPhyLosHandler(void)
{
        gpPhyData->flags30 &= ~0x700;          /* 清 PHY 状态 */
        xmcs_report_event(2, 0x10, 0);         /* EPON 的 LOS 事件 */

        if (ignoreIntMask & 0x8000) {          /* 配了「忽略 LOS」 */
                eponReadyFlag = 0;
                gpPonSysData->[0] = 0;
                eponStop();                     /* 停 MAC */
                gpWanPriv->[+0x20] = 1;
                xpon_reset_qdma_tx_buf();       /* 复位 TX 缓冲 */
                gpPonSysData->[+4] = 2;         /* 标记 EPON 模式 */
                eponFirmwareSetMode(0);
                stop_omci_oam_monitor();        /* 停 OMCI/OAM */
        } else {
                /* 只打日志，不做实际处理 */
        }
}
```

### 4.4 `eponDetectPhyReady`（`0x032f2c`，268 字节）—— 光恢复

```c
void eponDetectPhyReady(void)
{
        eponReadyFlag = 1;

        if (!(ignoreIntMask & 0x8000))
                return;

        if (!turnOffOpticalModuel)
                change_alarm_led_status(0);    /* 清告警 LED */

        if (eponDrv->[+0x08] == 0)
                eponFirmwareSetMode(0);

        xmcs_report_event(2, 0x11, 0);         /* EPON 的 PHY_READY 事件 */

        if (gpPonSysData->[+8] != 0)
                return;

        gpPonSysData->[0] = 2;                 /* 标记 EPON 模式 */
        if (eponDrv->[+0x08] == 0)
                schedule_work(&eponStartTask); /* 重新启动 */

        gpWanPriv->[0xef0] &= ~0x4;            /* 清 QoS 标记 */
}
```

**光恢复的完整链路**：LOS → 停 MAC → 等 PHY_READY → 重新 `eponStart`。

---

## 5. 两者对照：可迁移的部分

| 环节 | GPON | EPON | 可迁移 |
|---|---|---|---|
| PHY 状态来源 | `gpPhyData->[+0x30]` bit8–10 | 同 | ✅ **完全相同** |
| 模式字段 | bit3–6 = 1 | bit3–6 = 0 | ✅ 结构相同 |
| 清状态写法 | `&= ~0x700` | 同 | ✅ 完全相同 |
| 事件上报 | `xmcs_report_event(1, 0, 0)` | `xmcs_report_event(2, 0x10, 0)` | ✅ 只是参数不同 |
| 定时器换算 | `ms * 1000 / 8` | 同 | ✅ 完全相同 |
| LED 处理 | 未直接调 | `change_alarm_led_status()` | ⚠️ GPON 在别处处理 |
| 状态机响应 | O5 → O6 POPUP | 直接停 MAC 重来 | ❌ **策略不同** |
| TypeB 保护 | `gpon_typeb_dev_reset_for_typeB` | 无 | ❌ GPON 独有 |

**结论：PHY 层逻辑可以直接共用，状态机响应策略各自独立。**

---

## 6. 对 GPON 适配的直接贡献

### 6.1 补上了 `airoha_xpon_gpon_detect_los_lof` 的实现

之前是空函数，现在有完整逻辑。关键点：

```c
void airoha_xpon_gpon_detect_los_lof(struct airoha_xpon *xpon, u32 arg)
{
        u32 state = gpon_priv(xpon)->state;

        /* O2/O3/O4：未上线，直接重来 */
        if (state >= O2 && state <= O4) {
                if (arg == 1)
                        return;
                xpon->gpon_phy_state = 0;
                airoha_xpon_report_event(xpon, 1, 0, 0);
                airoha_xpon_gpon_disable(xpon, 0);
                return;
        }

        /* O5：已上线，进 O6 POPUP 等 TO1 */
        if (state == O5) {
                if ((gpon_priv(xpon)->los_ignore_flag & BIT(3)) || arg == 1)
                        return;
                xpon->gpon_phy_state = 0;
                airoha_xpon_gpon_set_state(xpon, O6_POPUP);
                airoha_xpon_gpon_disable_cpu_traffic(xpon);
                return;
        }

        gpon_priv(xpon)->bwm_stop_cfg &= ~BIT(2);
}
```

### 6.2 明确了「两个寄存器组」的分工

| 寄存器 | 用途 | 在哪个函数处理 |
|---|---|---|
| `0x4330` | GEM 头 1bit 错误计数 | `gpon_detect_los_lof` 只读打印 |
| `0x4334` | GEM 头 2bit 错误计数 | 同上 |
| `0x4338` | GEM 头不可纠错计数 | 同上 |
| `0x2fc` bit2 | LOS 标记 | LOS 处理末尾清除 |

### 6.3 澄清了一个之前的误解

我上一轮在 `docs/10` 里推测 `0x2fc` 的 bit3 是「TX_SYNC 状态」，
现在确认它确实是状态标志，但 **bit2 才是 LOS 标记**，bit3 另有用途（EqD 符号修正）。

---

## 7. 剩余未知

| 项 | 说明 |
|---|---|
| `fiber_hotplug_event(0)` | 光纤热插拔事件，参数 0 的含义待确认 |
| `is_hwnat_dont_clean` | 硬件 NAT 表保护标志，作用范围待确认 |
| `disable_cpu_us_traffic()` | 停 CPU 上行流量的具体实现 |
| `gpon_typeb_dev_reset_for_typeB()` | TypeB 保护倒换，GPON 独有 |
| `priv->[+0x30c]` bit3 | O5 态「忽略 LOS」的特殊标志，来源待查 |

这些不影响主链路，可在实现阶段按需深入。

---

## 8. 产物

```
docs/13-EPON寄存器表逆向.md     五张表 349 条 + 交叉验证
docs/14-EPON-MPCP协议逆向.md    MPCP 状态机
docs/15-LOS检测逆向.md          本篇（EPON + GPON 双份）
build/epon-tables.txt           寄存器表数据
tools/re_epon_tables.py         表提取器
```

**至此 GPON 的三个主要缺口补上了两个**（LOS 检测、命令下发），
只剩 `gpon_qos_init` 的 DBA 参数值和掉线恢复流程。
