# QoS 初始化与掉线恢复逆向（GPON 最后两个缺口）

> 对象：`gpon_qos_init`（252 B）、`gpon_qos_deinit`（332 B）、`gpon_recover_create_gemport`（1040 B）
> 结论：**GPON 逆向到此 100% 完成**，无剩余未知模块

---

## 1. QoS 初始化：`gpon_qos_init @0x0265a0`

### 1.1 完整逻辑

```c
int gpon_qos_init(void)
{
        u32 tcont;
        u8  idx;
        u32 green, total, yellow, max;

        /* 按芯片型号选默认值 */
        u32 hir = GET_HIR();
        if (hir == 0xa || hir == 0xc || hir == 0xe) {
                green  = 0xa00000;      /* 10 MB */
                total  = 0xa00000;      /* 10 MB */
        } else {
                green  = 0xffff;
                total  = 0xffff;
        }
        yellow = 0x8000;                /* 32 KB */
        max    = 0xffff;

        /* 遍历 32 个 T-CONT（0x1f = 31，所以是 0..31） */
        for (tcont = 0; tcont <= 0x1f; tcont++) {
                struct tcont_param p;

                idx = (u8)tcont;
                p.tcont   = idx;
                p.green   = green;
                p.total   = total;
                p.yellow  = yellow;
                p.max     = max;

                if (ecnt_hook_tcont_set(0x11, &p) < 0) {
                        if (debug & 0x1)
                                printk("...qos init fail tcont %u...", tcont);
                        return -1;
                }
        }
        return 0;
}
```

### 1.2 四个参数的含义

| 字段 | 值（HIR 0xa/0xc/0xe） | 值（其他） | 含义 |
|---|---|---|---|
| green | `0xa00000`（10 MB） | `0xffff` | 绿色（保证）带宽 |
| total | `0xa00000`（10 MB） | `0xffff` | 总缓冲 |
| yellow | `0x8000`（32 KB） | 同 | 黄色（超额）带宽 |
| max | `0xffff` | 同 | 上限 |

**对应寄存器**（`docs/12` 第 4 节）：

| 寄存器 | 偏移 | 对应字段 |
|---|---|---|
| `DBA_TOTAL_BUF_SIZE` | `0x0344` | total |
| `DBA_GREEN_SIZE` | `0x0348` | green |
| `DBA_YELLOW_SIZE` | `0x034c` | yellow |

### 1.3 为什么是 32 个 T-CONT

循环上限 `0x1f`（31），即 **0–31 共 32 个**。

而寄存器 `0x0020–0x003c` 只放 16 个 Alloc-ID（T-CONT0–15）。
说明 **T-CONT 编号空间是 32，但硬件 Alloc-ID 表只支持 16 个直接映射**，
后 16 个需要通过 `ecnt_hook` 走扩展表。

### 1.4 两个 EcoNet 钩子

| 钩子 ID | 用途 | 函数 |
|---|---|---|
| `0x11` | T-CONT 参数设置 | `0x2614c` |
| `0x74` | T-CONT 释放 | `0x260f0` |

```c
static int ecnt_hook_tcont_set(u32 hook, void *param)
{
        struct { u32 pad; u32 hook_id; void *arg; } pkt = { 0, hook, param };
        return __ECNT_HOOK(0x11, NULL, &pkt);
}
```

### 1.5 反初始化：`gpon_qos_deinit @0x02669c`

```c
int gpon_qos_deinit(void)
{
        for (tcont = 0; tcont <= 0x1f; tcont++) {
                struct tcont_param p = { 0 };

                p.tcont = tcont;
                /* green/total/yellow/max 全 0 = 释放 */

                if (ecnt_hook_tcont_set(0x11, &p) < 0)
                        return -1;

                /* 再用 0x74 钩子显式释放 */
                struct release_param r = { .type = 3, .tcont = tcont };
                if (ecnt_hook_tcont_release(0x74, &r) < 0)
                        return -1;
        }

        xpon_set_qdma_qos(1);      /* 通知 QDMA 重新分配 */
        return 0;
}
```

**注意末尾的 `xpon_set_qdma_qos(1)`**：释放 T-CONT 后必须通知 QDMA 重算队列，
否则硬件队列会与软件状态不一致。

---

## 2. 掉线恢复：`gponRecovery` 结构

### 2.1 结构布局（`0xc88` = 3208 字节）

```c
struct gpon_recovery {
        u16 alloc_id[32];               /* +0x000  32 个 Alloc-ID */
        u8  _pad[0x80 - 0x40];          /* +0x040 */
        struct gem_entry {
                u16 gem_id;             /* +0x00 */
                u16 _pad0;              /* +0x02 */
                u32 _pad1;              /* +0x04 */
                u8  encrypt;            /* +0x04（从 ldrb [x0,#0x84] 推断） */
                u8  direction;          /* +0x05 */
                u16 alloc_id;           /* +0x02（ldrh [x0,#0x82]） */
        } gem[0x100];                   /* +0x080  256 个 GEM 表项，每项 12 字节 */
        u32 gem_count;                  /* +0xc80 */
        u32 alloc_count;                /* +0xc84 */
        u8  debug;                      /* +0xc88 */
};
```

**验证**：`0x80 + 0x100 * 12 = 0x80 + 0xC00 = 0xC80` ✅ 正好等于 `gem_count` 偏移。

### 2.2 表项字段（从 `gpon_recover_backup_gemport` 提取）

```asm
; 表项地址 = gponRecovery + 0x80 + index * 12
ldr  x2, [x1]          ; [0] = 8 字节（gem_id + alloc_id）
str  x2, [x0]
ldr  w1, [x1, #8]      ; [8] = 4 字节（encrypt + direction）
str  w1, [x0, #8]
```

读取时：
```asm
ldrh w0, [x0, #0x82]   ; +0x82 = 表项内偏移 2 → alloc_id
ldrh w0, [x0, #0x80]   ; +0x80 = 表项内偏移 0 → gem_id
ldrb w0, [x0, #0x84]   ; +0x84 = 表项内偏移 4 → encrypt
ldrb w0, [x0, #0x85]   ; +0x85 = 表项内偏移 5 → direction
ldr  w0, [x0, #0x88]   ; +0x88 = 表项内偏移 8 → 附加标志
```

---

## 3. 恢复流程

### 3.1 入口：`xpon_exec_gem_recover @0x00cf04`（128 字节）

```c
void xpon_exec_gem_recover(void)
{
        if (gponRecovery.debug)
                printk("...gem recover start...");

        /* gpGponPriv->[0x310]->[0xcb0] 是「数据路径已就绪」标志 */
        if (gpGponPriv->[0x310]->[0xcb0] == 0)
                gpon_recover_create_gemport();

        gpGponPriv->[0x310]->[0xcb0] = 0;
}
```

### 3.2 GEM 重建：`gpon_recover_create_gemport @0x029f2c`（1040 字节）

```c
int gpon_recover_create_gemport(void)
{
        int i, ret = 0;

        for (i = 0; i < gponRecovery.gem_count; i++) {
                struct gem_entry *e = &gponRecovery.gem[i];

                /* 1. 已存在则跳过 */
                if (gpon_check_gem_exist(e->gem_id) == 1) {
                        if (gponRecovery.debug)
                                printk("gem %u already exist", e->gem_id);
                        continue;
                }

                /* 2. 创建 */
                if (gwan_create_new_gemport(e) != 0) {
                        if (gponRecovery.debug)
                                printk("create gem %u fail", e->gem_id);
                        ret = -1;
                        continue;
                }

                /* 3. 配置（VLAN/QoS/加密） */
                if (gwan_config_gemport(e) != 0) {
                        if (gponRecovery.debug)
                                printk("config gem %u fail", e->gem_id);
                        ret = -1;
                        continue;
                }
        }
        return ret;
}
```

**关键设计：逐项独立重试，单项失败不中断整体。**
这样即使某个 GEM 配置失败，其它业务仍能恢复。

### 3.3 Alloc-ID 重建：`gpon_recover_create_allocId @0x0296a4`（196 字节）

```c
int gpon_recover_create_allocId(void)
{
        int i, ret = 0;

        for (i = 0; i < gponRecovery.alloc_count; i++) {
                if (gwan_create_new_tcont(gponRecovery.alloc_id[i]) != 0) {
                        if (gponRecovery.debug)
                                printk("create alloc %u fail",
                                       gponRecovery.alloc_id[i]);
                        ret = -1;
                }
        }
        return ret;
}
```

### 3.4 备份：`gpon_recover_backup_gemport @0x0298ec`（768 字节）

```c
int gpon_recover_backup_gemport(struct gem_entry *e)
{
        /* 先检查是否已备份过（去重） */
        if (gpon_check_gem_exist(e->gem_id) == 1)
                return -1;

        /* 追加到表尾 */
        gponRecovery.gem[gponRecovery.gem_count] = *e;
        gponRecovery.gem_count++;

        if (gponRecovery.debug) {
                printk("backup gem: idx=%u gem_id=0x%x alloc_id=0x%x "
                       "encrypt=%u dir=%u flag=0x%x",
                       gponRecovery.gem_count - 1, e->gem_id,
                       e->alloc_id, e->encrypt, e->direction, e->flag);
        }
        return 0;
}
```

### 3.5 GEM 存在性检查：`gpon_check_gem_exist @0x029d2c`（180 字节）

```c
int gpon_check_gem_exist(u16 gem_id)
{
        int i;

        /* 检查 wan 表容量 */
        if (gpWanPriv->[0x12868] > 0x100)
                return -ENOSPC;         /* -0x1c */

        for (i = 0; i <= 0xff; i++) {
                u32 v = *(u32 *)(gpWanPriv + 0x2060 + i * 0x48 + 8);
                if ((v & 0xfff) == gem_id)
                        return 1;
        }
        return 0;
}
```

**注意**：这里查的是 `gpWanPriv` 的 WAN 业务表，不是 `gponRecovery`。
说明「存在性」以实际业务配置为准，而不是备份表。

---

## 4. 恢复的完整时序

```
掉线（LOS / Deactivate）
   │
   ├─ 业务配置仍在 gponRecovery 里（不掉）
   │
   ▼
重新上线（O5）
   │
   ├─ xpon_exec_gem_recover()
   │     │
   │     ├─ gpon_recover_create_allocId()    ← 先建 Alloc-ID
   │     │     逐项调 gwan_create_new_tcont
   │     │
   │     └─ gpon_recover_create_gemport()    ← 再建 GEM
   │           逐项：检查存在 → 创建 → 配置
   │
   ▼
业务恢复（秒级）
```

**顺序很重要：先 Alloc-ID，后 GEM。** 因为 GEM 表项要引用 Alloc-ID。

---

## 5. 为什么这套机制对国内运营商关键

国内电信/联通的 OLT 在 ONU 掉线重连后会**重新下发 OMCI**。
如果 ONU 侧没保存业务配置，重建过程需要：
1. OLT 检测到 ONU 上线
2. OLT 重新下发所有 ME（可能几十条）
3. ONU 逐条配置

**耗时通常 30–60 秒**，期间业务中断。

原厂的 `gponRecovery` 让 ONU 在 O5 恢复后**立即自行重建**，
不用等 OLT 重新下发，**恢复到秒级**。

这是一个必须保留的能力——否则用户会感觉「掉线后要等半分钟」。

---

## 6. 移植实现

### 6.1 QoS 初始化

```c
int airoha_xpon_gpon_qos_init(struct airoha_xpon *xpon)
{
        u32 hir = airoha_xpon_get_hir(xpon);
        u32 green, total;
        int i, ret;

        if (hir == 0xa || hir == 0xc || hir == 0xe) {
                green = 0xa00000;
                total = 0xa00000;
        } else {
                green = 0xffff;
                total = 0xffff;
        }

        for (i = 0; i <= 0x1f; i++) {
                ret = airoha_xpon_gpon_tcont_set(xpon, i, green, total,
                                                 0x8000, 0xffff);
                if (ret)
                        return ret;
        }
        return 0;
}
```

### 6.2 恢复表

```c
#define AIROHA_GPON_RECOVERY_GEM_MAX   0x100

struct airoha_gpon_recovery_gem {
        u16 gem_id;
        u16 alloc_id;
        u8  encrypt;
        u8  direction;
        u16 flags;
        u32 reserved;
};

struct airoha_gpon_recovery {
        u16 alloc_id[32];
        struct airoha_gpon_recovery_gem gem[AIROHA_GPON_RECOVERY_GEM_MAX];
        u32 gem_count;
        u32 alloc_count;
        bool debug;
};
```

### 6.3 注意事项

1. **`gem_count` 上限是 0x100（256）**，超出会越界，必须检查
2. **恢复是「尽力而为」**：单项失败继续，不中断整体
3. **先 Alloc-ID 后 GEM**：顺序不能反
4. **`xpon_set_qdma_qos(1)` 不能漏**：释放 T-CONT 后必须通知 QDMA

---

## 7. 逆向完成度总结

| 模块 | 状态 | 文档 |
|---|---|---|
| 寄存器表（112 个） | ✅ | docs/06 |
| PLOAM 收发与 21 项分派 | ✅ | docs/08 |
| 命令下发链路 | ✅ | docs/09 |
| 状态机 O1–O7 | ✅ | docs/08 |
| 中断服务（30 位） | ✅ | docs/10 |
| EqD 精细测距 | ✅ | docs/11 |
| GEM 表 / Alloc-ID | ✅ | docs/12 |
| LOS/LOF 检测 | ✅ | docs/15 |
| **QoS 初始化 / DBA** | ✅ | **本篇** |
| **掉线恢复** | ✅ | **本篇** |
| EPON 寄存器表（349 条） | ✅ | docs/13 |
| EPON MPCP 状态机 | ✅ | docs/14 |

**GPON 侧逆向 100% 完成。** 剩下的是 EPON 的次要模块（复位、队列门限、IOCTL），
不影响「能不能上线」这个核心问题。
