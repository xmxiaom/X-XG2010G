# EqD 精细测距算法逆向

> 对象：`ploam_eqd_adjustment @0x014bd0`（1052 字节）
> 作用：O5 运营态重测距时，把 OLT 下发的新 EqD 增量校正后写入寄存器

---

## 1. 为什么需要这个算法

GPON 的 EqD（Equalization Delay，均衡延迟）寄存器 `0x00a8` 只有有限精度：
**硬件按 8 的整数倍生效，余数需要软件记住并在下次补偿**。

原厂的做法是维护三个变量：

| 变量 | 原厂偏移 | 含义 |
|---|---|---|
| `last_eqd` | `+0x220` | 上次的 EqD 基准值 |
| `eqd_pending` | `+0x224` | 累积校正后待写入的值 |
| `ranging_retry` | `+0x228` | 上次未补偿的余数（0–7） |
| `eqd_mode` | `+0x22a` | 0 = 首次（只记录）；非 0 = 需校正（同时也是步长） |

---

## 2. 完整算法（逐行还原）

```c
void ploam_eqd_adjustment(u32 new_eqd)
{
        s32 delta = new_eqd - priv->last_eqd;      // +0x220

        /* 首次：只记录基准，不做校正 */
        if (priv->eqd_mode == 0) {                 // +0x22a
                priv->last_eqd = new_eqd;
                return;
        }

        /* 按 0x2fc 的 bit3 修正 delta 的符号 */
        if (priv->[+0x2fc] & BIT(3))
                delta -= priv->eqd_mode;           // 减步长
        else
                delta += priv->eqd_mode;           // 加步长

        /* 子偏移基准：0x035c（TX_SYNC_OFFSET）低 2 位 */
        sub_base = get_xpon_data(BASE + 0x435c) & 0x3;

        if (delta > 0) {
                tmp = delta + priv->ranging_retry;      // +0x228
                hi  = tmp >> 3;                          // 8 的整数倍
                lo  = tmp & 0x7;                         // 余数

                adj = (hi & ~3) +
                      (((sub_base + (hi & 3)) > 3) ? 8 : 0) -
                      (hi & 3);

                priv->eqd_pending += adj << 3;           // +0x224
                priv->ranging_retry = lo;
        } else if (delta < 0) {
                tmp = (-delta) - priv->ranging_retry;
                hi  = (tmp > 0) ? (tmp >> 3) + ((tmp & 7) ? 1 : 0) : 0;
                lo  = (hi << 3) - tmp;

                adj = (hi & ~3) +
                      ((sub_base < (hi & 3)) ? 8 : 0) -
                      (hi & 3);

                priv->eqd_pending -= adj << 3;
                priv->ranging_retry = lo;
        }
        /* delta == 0：什么都不做 */

        priv->eqd_mode = 0;                              // 清标记

        if (debug & (0x1000 | 0x10))
                printk("... base=0x%x pending=0x%x delta=%d hi=%d lo=%d adj=%d retry=%d",
                       priv->last_eqd, priv->eqd_pending, delta,
                       hi, lo, adj, priv->ranging_retry);

        /* 写回 0x00a8 */
        set_xpon_data(BASE + 0x40a8, priv->eqd_pending);

        /* 收尾：按新的 retry 值调子函数 */
        sub_0x14b24(priv->ranging_retry);
}
```

---

## 3. 关键设计点

### 3.1 `adj` 的计算为什么这么绕

```c
adj = (hi & ~3) + ((sub_base + (hi & 3)) > 3 ? 8 : 0) - (hi & 3);
```

拆开看：

- `hi & ~3`：把 `hi` 向下取整到 4 的倍数
- `hi & 3`：`hi` 的低 2 位（0–3）
- `sub_base + (hi & 3) > 3`：判断「加上子偏移基准后是否进位」

这实际是**在 4 的粒度上做舍入**，而 `sub_base` 是当前硬件的相位基准。
最终 `adj << 3` 才是真正写入寄存器的值（8 的倍数）。

换句话说：
- 硬件粒度是 **8**
- 但校正步进按 **4** 走，靠 `sub_base` 决定进位方向
- 余数留在 `ranging_retry`，下次一起算

### 3.2 正负分支的 `lo` 语义不同

| 分支 | `lo` 计算 | 含义 |
|---|---|---|
| `delta > 0` | `tmp & 7` | 本轮**多减**了多少，下轮补回来 |
| `delta < 0` | `(hi << 3) - tmp` | 本轮**少减**了多少，下轮扣回来 |

两者都存进 `ranging_retry`，但下一轮参与运算的方式相反
（正分支是 `delta + retry`，负分支是 `-delta - retry`）。

### 3.3 `eqd_mode` 的双重身份

`+0x22a` 这个字节：

- **等于 0** 时：表示「首次测距」，走快速路径
- **非 0** 时：既表示「需要校正」，**其值本身又是步长**

这是原厂的一种紧凑编码，移植时建议拆成两个字段以免混淆：

```c
bool eqd_need_adjust;
u8   eqd_step;
```

---

## 4. 与状态机的衔接

```
Ranging_Time (PLOAMd 0x04)
  └─ gpon_msg_ranging_time
       ├─ 状态 == O4 → 写 EqD，推进到 O5
       └─ 状态 == O5 → ploam_eqd_adjustment(new_eqd)   ← 本函数
```

**注意**：O4 首次测距**不走**这个函数，直接把 OLT 给的 EqD 写进 `0x00a8`。
只有 O5 运营态收到重测距请求时才做增量校正。

---

## 5. 移植注意事项

1. **`sub_base` 必须实时读**，不能缓存——它反映当前硬件的 TX 相位
2. **`delta == 0` 时不能提前 return**，仍需清 `eqd_mode` 并写回（原厂行为）
3. **`ranging_retry` 是 u8**，但参与 `s32` 运算，注意符号扩展
4. **首次测距的快速路径必须保留**，否则第一次会算出巨大偏差

---

## 6. 验证方法

在真机上打日志观察收敛过程：

```
EqD: base=0x1234 pending=0x1238 delta=5 hi=0 lo=5 adj=0 retry=5
EqD: base=0x1238 pending=0x1240 delta=8 hi=1 lo=0 adj=1 retry=0
EqD: base=0x1240 pending=0x1240 delta=0 hi=0 lo=0 adj=0 retry=0
```

正常情况下 2–3 轮就收敛到 `delta == 0`。如果持续震荡，
说明 `sub_base` 读取有误或 `eqd_step` 取值不对。

---

## 7. 剩余未知

| 项 | 说明 |
|---|---|
| `sub_0x14b24` | 校正后的收尾函数，与重试触发相关，尚未细读 |
| `priv->[+0x2fc]` bit3 的来源 | 推测是 TX_SYNC 状态，需确认写入方 |
| `eqd_step` 的实际取值 | 原厂由上层配置，常见值是 1 或 2 |

这些不影响算法主体，可在实测阶段调整。
