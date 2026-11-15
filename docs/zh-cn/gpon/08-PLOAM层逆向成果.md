# PLOAM 层逆向成果

> 对象：原厂 `xpon.ko`（XG2010G_fw_1.0.0.13，Linux 5.4.55，aarch64，**未 strip**）
> 方法：`.symtab` + `.eh_frame`（1634 个 FDE）+ Capstone 反汇编 + `.rela.text` 重定位还原符号引用
> 结论：**GPON 上线所需的 PLOAM 层已全部还原**，可直接照此实现

---

## 1. 模块整体结构（已完全定位）

| 组件 | 地址 / 偏移 | 大小 | 作用 |
|---|---|---|---|
| GPON MAC 寄存器窗口 | 物理 `0x1fb64000` | `0x3e8` | 1000 字节，寄存器跨度实测 `0x000–0x3c8`，**刚好装得下** |
| `g_gpon_mac_reg_BASE` | `0xBFB60000` + `0x4000` | — | 全局基址变量，`get/set_xpon_data` 的实际入参 |
| `gpGponPriv` | 全局指针 | — | 驱动私有结构，字段偏移见下表 |
| `gpPonSysData` | 全局指针 | — | 系统级数据（`+0xe1b18` 为调试位图） |

### `gpGponPriv` 关键字段偏移（从状态机与 PLOAM 代码中交叉确认）

| 偏移 | 类型 | 含义 |
|---|---|---|
| `+0x00` | `u32` | TO1 超时计数 / 状态标记 |
| `+0x04` | `u32` | **当前激活状态 O1–O7** |
| `+0x08` | `timer_list` | TO1 定时器（O5 用 `+0x08`，O6 用 `+0x30`） |
| `+0x18` | `u64` | 当前定时器到期 jiffies |
| `+0x20` | `u32` | QDMA TX buffer 相关 |
| `+0x30` | `timer_list` | O6 POPUP 定时器 |
| `+0x40` | `u64` | O6 定时器到期 jiffies |
| `+0x108` | `void *[25]` | **PLOAMd 下行消息分派表**（下标 = 消息 ID，`+0x108 + id*8`） |
| `+0x1d0` | `u8[12]` | 最近一次收到的 PLOAMd 原文（12 字节） |
| `+0x1dd` | `u8[8]` | 本机 VSSN（SN 后 8 字节），`Assign_ONU_ID` 时 `memcmp` 用 |
| `+0x210` | `u32` | O5 的 TO1 超时值（毫秒） |
| `+0x214` | `u32` | O6 的 TO2 超时值（毫秒） |
| `+0x224` | `u32` | 待写入的 EqD 值 |
| `+0x228` | `u8` | 测距重试计数 |
| `+0x24c` | `s32` | 芯片/线路参数缓存，初值 `-1` |
| `+0x300` | `struct` | PON 任务结构（tasklet 用） |
| `+0x310` | `void *` | 指向 `+0xcb0` 处的工作队列标志 |

---

## 2. PLOAMd 下行分派表（**21 个处理器全部还原**）

安装位置：`ploam_init @0x018d34`，写 `gpGponPriv->[0x108 + msg_id*8]`。
分派位置：`ploam_parser_down_message @0x014fec`，读 `msg[1]` 作为消息 ID，
`if (id > 0x18) → 丢弃并返回 -E`，否则 `call [0x108 + id*8]`。

| msg_id | 处理器地址 | G.984.3 消息名 | 说明 |
|---:|---|---|---|
| 1 | `0x015a44` | **Upstream_Overhead** | 写入 `0x0090–0x00a4` 六个开销寄存器 |
| 3 | `0x015e58` | **Assign_ONU_ID** | 校验 `0xff` 标记 + VSSN 比对 → 写 `0x0004` |
| 4 | `0x01609c` | **Ranging_Time** | 写 `0x00a8` EqD → 推进状态 |
| 5 | `0x016778` | Password (request) | 回 Password |
| 6 | `0x016a48` | **Deactivate_ONU-ID** | 回 O1 |
| 8 | `0x016ce4` | Request_Key | AES 密钥交换 |
| 9 | `0x016f44` | Configure_Port-ID | GEM Port 建立 |
| 10 | `0x0170a0` | Encrypted_Port-ID | 加密 GEM Port |
| 12 | `0x017428` | PEE (Power Emergency) | 省电 |
| 13 | `0x017680` | PST (Power Shortage) | 省电 |
| 14 | `0x017844` | BER Interval | 误码监测 |
| 15 | `0x017c20` | **Dying_Gasp** | 掉电告警 |
| 16 | `0x017d20` | 扩展突发长度 | — |
| 17 | `0x017e7c` | PON-ID | — |
| 18 | `0x017f9c` | 加密配置 | — |
| 19 | `0x0181ec` | 密钥同步 | — |
| 20 | `0x018368` | 保护倒换 | — |
| 21 | `0x018810` | 通道管理 | — |
| 22 | `0x018910` | 阈值告警 | — |
| 23 | `0x018a30` | 性能监测 | — |
| 24 | `0x018c14` | 厂商自定义 | — |

**msg_id 2 / 7 / 11 未注册**（表项为 NULL）：分别对应 SN 请求的应答路径、
Ranging 内部响应和 PLOAMd 保留值 —— 说明 SN 应答走的是 **PLOAMu 主动发送**而非
下行分派，这与 G.984.3 一致（O2→O3 由 ONU 主动上报 Serial_Number_ONU）。

---

## 3. PLOAMu 发送：`gponDevSendPloamMsg @0x01df34`（352 字节，已完全解码）

### 3.1 函数原型

```c
int gponDevSendPloamMsg(u32 *msg /*3 个字 = 12 字节*/, u32 count);
```

### 3.2 完整逻辑

```
1. 掉电保护：if (gp_dying_info->flag3 == 1 && msg->byte[1] != 3)  return 0;
   —— 一旦进入 Dying Gasp，除 Dying_Gasp 消息(0x03)外一律不再发送

2. for (i = 0; i < count; i++)            // count 通常为 1
       for (j = 0; j < 3; j++)            // 固定 3 个 32-bit 字
           set_xpon_data(BASE + 0x4054, swab32(msg[i*3 + j]));
           ↑ 0x4054 = PLOAMu FIFO 写入端口；每个字都要字节序翻转

3. if (msg->byte[1] == 3)                 // 本条是 Dying_Gasp
       if (gp_dying_info->flag0 == 1)  delay1us(gp_dying_info->delay);
       if (gp_dying_info->flag0 == 2)  return 0;
       gp_dying_info->flag3 = 0;          // 清除掉电标记
```

### 3.3 关键结论

- **PLOAM 报文固定 12 字节 = 3 个 32-bit 字**，逐字写入 `0x4054`，**必须 `swab32`**
- `msg->byte[1]` 是消息 ID（`byte[0]` 是 `ploam[0]`，即 ONU-ID/superframe 片段）
- `msg->byte[0]` 的 bit7 是 **Send** 位，bit0–6 是 ONU-ID —— 由上层构造
- 掉电状态机由 `gp_dying_info` 三字段控制（`flag0` 模式、`flag3` 已进入、`delay` 延迟）

---

## 4. PLOAMd 接收：`gponDevGetPloamMsg @0x01de8c`（168 字节）

```c
int gponDevGetPloamMsg(u32 *out);   // out 指向 3 字缓冲

status = get_xpon_data(BASE + 0x4058);          // PLOAMd FIFO STATUS
if (status == 0)      return 0;                  // 空
if (status <= 2)      return -EINVAL;            // 1 或 2：数据不完整
for (j = 0; j < 3; j++)
    out[j] = swab32(get_xpon_data(BASE + 0x405c));   // 0x405c = PLOAMd FIFO 读端口
return status;                                   // 返回 FIFO 中剩余条数
```

**`0x4058` 低字节 = FIFO 中待读条目数**，`0` 空、`1/2` 视为异常、`>=3` 正常读取 3 个字。

### 4.1 上层取帧：`gpon_recv_ploam_message @0x01b610`

```c
int cnt, rc;
do {
    rc = gponDevGetPloamMsg(&msg);        // 读一条
    if (rc <= 0) break;                    // 空或错误
    if (ploam_check(&msg) == 0)            // 校验（见下）
        ploam_parser_down_message(&msg);   // 分派
} while (rc > 3);                          // 一次最多处理到 FIFO 只剩 <=3
```

### 4.2 校验：`ploam_check @0x01b488`（392 字节）

```
if (gpPonSysData->[+8] != 0)  return 1;         // 初始化未完成，丢弃
rc = sub_0x1b410(&msg);                          // ← 重复帧过滤，不是 MIC
if (rc != 0) {
    gpPonSysData->[+0] += 1;                     // 错误计数
    if (gpPonSysData->[+0] % 3 != 0)  return 1;  // 每 3 次才打印一次
    printk(...);  return 1;
}
memcpy(gpGponPriv + 0x1d0, &msg, 12);            // 保存最近一条原文
gpPonSysData->[+0] = 0;                          // 清错误计数
return 0;                                        // 通过
```

**注意 `% 3` 的限流**：错误计数每 3 次才打一条日志，这是原厂的防刷屏设计。

### 4.3 `sub_0x1b410`（120 字节）—— **重复帧过滤，不是 MIC 校验**

这一点必须纠正：此前误以为它是 MIC 算法，实际实现极其简单：

```c
int ploam_dedup(u8 *msg)
{
        if (msg[1] == 4)                          // 消息 ID 4 = Ranging_Time
                return memcmp(gpGponPriv + 0x1d0, msg, 7) == 0;
        return memcmp(gpGponPriv + 0x1d0, msg, 12) == 0;
}
```

- 拿新帧与「上一条已收帧」（`gpGponPriv+0x1d0`）做 `memcmp`
- **Ranging_Time 只比前 7 字节**（因为 EqD 每次都会变，比全 12 字节会永远不相等）
- 返回 1 = 重复帧，直接丢弃

**结论：GPON 的 PLOAMd 不做 MIC 校验**。MIC 只在 PLOAMu 上行方向需要（由 OLT 验证），
下行由 OLT 加密/校验，ONU 侧只做去重。这大大简化了移植——**不需要实现 AES-CMAC**。

**副作用**：`gpGponPriv+0x1d0` 保存的是「上一条通过校验的帧」，
所以连续两条完全相同的 PLOAMd（例如连续两次 Ranging_Time 且 EqD 相同）第二条会被丢弃。
这是原厂的既有行为，移植时应保持一致。

---

## 5. Assign_ONU_ID 处理：`0x015e58`（580 字节，上线关键）

```
1. if (msg->byte[0] != 0xff)  → 报错 return -1
   ↑ ONU-ID 字段必须是 0xff（广播/未分配），否则说明不是给我的

2. memcmp(gpGponPriv + 0x1dd, &msg->byte[3], 8)
   ↑ 拿报文第 4–11 字节与本机 VSSN 比对，必须完全一致

3. 校验通过后：
   gpon_act_change_gpon_state(0x5)              // 直接进 O5
   v = get_xpon_data(BASE + 0x4004)             // ONU GLOBAL CONFIGURATION
   ...（按 msg->byte[0] 的低 8 位构造 ONU-ID）
   set_xpon_data(BASE + 0x4004, v)              // 写入 ONU-ID
   gpGponPriv->[+0x20] = ...                    // 记录
   gpon_init_qdma_tx_buff()                     // 初始化 QDMA TX 缓冲
```

**这解释了为什么 SN 必须正确**：`0x1dd` 处的 VSSN 直接参与 `memcmp`，
SN 写错 → 比对失败 → 永远停在 O3，OLT 侧表现为「反复收到 SN 但不给 ONU-ID」。

---

## 6. Ranging_Time 处理：`0x01609c`（1756 字节，测距核心）

关键操作序列：

```
1. 校验当前状态 ∈ {4, 5}（O4 或 O5），否则忽略

2. 取 EqD 值（msg 中带 OLT 测出的均衡延迟）
   cur = get_xpon_data(BASE + 0x40a8)            // 读当前 MAIN PATH EQUALIZATION DELAY
   new = gpGponPriv->[+0x224]                     // 新 EqD 值（上层已算好）
   set_xpon_data(BASE + 0x40a8, new)              // 写入

3. 若状态 == 5（O5 重测距）：
       ploam_eqd_adjustment()                     // 0x014bd0，1052 字节，做精细校正

4. 若状态 == 4（O4 首次测距）：
       v = get_xpon_data(BASE + 0x4004)
       set_xpon_data(BASE + 0x4004, v | ...)      // 置位
       gpon_act_change_gpon_state(...)            // 推进到 O5
```

**`0x40a8` 是 EqD 的唯一落点**，O4 和 O5 都写它，区别是 O5 多走一次 `ploam_eqd_adjustment`。

---

## 7. Upstream_Overhead 处理：`0x015a44`（1044 字节）

按 G.984.3 的 9 字节载荷写六个寄存器：

| 报文位置 | 寄存器 | 名称 |
|---|---|---|
| byte 0–3 | `0x0090` | PHYSICAL LAYER OVERHEAD TIME |
| byte 4 | `0x0094` | NUMBER OF GUARD BITS |
| byte 5 | `0x0098` | NUMBER OF TYPE1 and TYPE2 PREAMBLE BITS |
| byte 6 | `0x009c` | NUNMBER OF TYPE3 |
| byte 7 | `0x00a0` | NUMBER OF DELIMETER BITS |
| byte 8 | `0x00a4` | PRE-ASSIGNED DELAY |

---

## 8. 状态机：`gpon_act_change_gpon_state @0x013b18`（2000 字节，已完整解码）

### 8.1 函数签名与入口保护

```c
void gpon_act_change_gpon_state(u32 new_state);
```

入口处的两条快速返回：

```
if (gpGponPriv->[+0xa8] == 1 && gpGponPriv->[+0x04] == 2 && new_state == 3)
        goto 直接返回;         // O2 且已在特定模式 → 忽略 O3 请求
```

### 8.2 状态迁移主表

| 新状态 | 触发动作（按代码顺序） |
|---|---|
| **O1** (1) | `xmcs_report_event(1, 0xc, 0)`（若 `[+0xac]==1`）→ 清 `[+0xac]` → 清 `[+0x310]+0xcb0` |
| **O2** (2) | 构造 `{0xf0, 0,0,0,0}` 参数块 → `apply_state_config(0x12, &blk)` → `timer_ctrl_b(0)` |
| **O3** (3) / **O4** (4) | 构造 `{0x80, ..., 2}` 参数块 → `apply_state_config(0x12, &blk)` |
| **O5** (5) | `apply_state_config(0x12, {0x80,..,3})` → `gponDevSetFilterExtBurstLengthPLOAM(1)` → `gponDevSetFilterUpstreamOverheadPLOAM(1)` → 若 `[+0x24c]==-1` 则 `[+0x24c]=get_hw_param(0x801e)` → 写 `0x00bc` 状态寄存器 → `[+0x04]=5` → **`mod_timer(TO1, jiffies + [+0x210]*1000/8)`** |
| **O6** (6) | 写 `0x00bc` → `[+0x04]=6` → **`mod_timer(TO2, jiffies + [+0x214]*1000/8)`** |
| **O7** (7) | 与 O3/O4 同路径（`apply_state_config(0x12, {0x80,..,2})`） |

### 8.3 所有分支的公共尾部

```
1. taskLedConfig()                       // 0x134e8 → 若未调度则 __tasklet_schedule
2. tasklet_pon(&gpGponPriv->[+0x300], saved_ctx)   // 0x13410
3. if (debug_bitmap & 0x1008)  printk(...)         // 状态切换日志
```

### 8.4 写状态寄存器的精确写法（`0x40bc`）

```c
v = get_xpon_data(BASE + 0x40bc);
old_state = v & 0x7;                       // 保存旧状态（低 3 位）
v = (v & ~0x7) | (new_state & 0x7);        // bfxil 写低 3 位
set_xpon_data(BASE + 0x40bc, v);
```

**注意**：`0x00bc` 只有低 3 位是状态，高位必须保留。

### 8.5 定时器超时值的换算（关键！）

```
mod_timer(timer, jiffies + (u64)timeout_ms * 1000 / 8);
```

反汇编里出现的魔数 `0x20c49ba5e353f7cf` 是 **除以 8 的乘法逆元**（`umulh` + `lsr #4`），
配合前面的 `lsl #1 / add / lsl #3 / add / lsl #2`（= `x*125`）实际是 `x * 1000 / 8`。
即 **`[+0x210]` / `[+0x214]` 的单位是毫秒，转换为 jiffies 的公式是 `ms * 1000 / 8`**
—— 前提是 HZ=1000 时等价于 `ms * 125`，与原厂 HZ 配置一致。

---

## 9. 辅助函数定名（原 `.symtab` 中 `st_size=0`，靠 FDE 恢复边界后解读）

| 地址 | 大小 | 实际作用 |
|---|---|---|
| `0x133e0` | 48 B | `get_activation_field(p)` → `sub_0x132e0()` + `sub_0x13330(p, &out)`，读激活状态字段 |
| `0x1362c` | 64 B | **`apply_state_config(cmd, param)`** → 构造 `{1, cmd, param}` 结构体调 `sub_0x135b0`，是状态机唯一的配置下发通道 |
| `0x136a8` | 88 B | `get_hw_param(reg)`：`GET_HIR()==0xa` 时先查 `0x8023`，为 1 才读目标寄存器，否则返回 0 |
| `0x13700` | 40 B | `timer_ctrl_a(v)` → `apply_state_config(9, &v)` |
| `0x13728` | 36 B | `timer_ctrl_b(v)` → `apply_state_config(0xa, &v)` |
| `0x134e8` | 60 B | `tasklet_schedule_once(t)`：`if (test_and_set_bit(0, t+8)==0) __tasklet_schedule(t)` |
| `0x13410` | 44 B | `tasklet_pon(t, ctx)` → `sub_0x13390(t)` + `sub_0x13310(ctx)` |
| `0x12ccc` | 260 B | `mem_op`：`memcpy` + 按 8/4/2/1 分级拷贝，带多重比较（是优化过的 `memcpy` 变体） |

**`apply_state_config` 是核心发现**：所有状态配置（`0x12` = 状态切换、`0x9`/`0xa` = 定时器）
都走同一个函数，参数是 `{cmd, param}` 二元组。这意味着移植时只需要实现一个
`airoha_gpon_mac_cmd()`，不用逐个还原寄存器序列。

---

## 10. 移植映射表（直接可写代码）

| 原厂 | 开源驱动对应实现 | 状态 |
|---|---|---|
| `gponDevSendPloamMsg` | `airoha_xpon_gpon_send_ploamu()` | ✅ 逻辑已完整还原，可直接写 |
| `gponDevGetPloamMsg` | `airoha_xpon_gpon_get_ploamd()` | ✅ 完整还原 |
| `ploam_check` | 同上，含重复帧过滤 | ✅ 完整还原（无 MIC，见 4.3） |
| `ploam_parser_down_message` | `airoha_xpon_gpon_dispatch_ploamd()` | ✅ 21 项分派表已还原 |
| `gpon_act_change_gpon_state` | `airoha_xpon_gpon_set_state()` | ✅ 迁移表已还原 |
| `ploam_init` | `airoha_xpon_gpon_ploam_init()` | ✅ 表安装方式已还原 |
| `ploam_eqd_adjustment` | `airoha_xpon_gpon_eqd_adjust()` | ⚠️ 1052 字节，待细读 |

---

## 11. 剩余缺口（按优先级）

| 优先级 | 目标 | 地址 | 大小 | 缺口内容 |
|---|---|---|---|---|
| **P0** | `sub_0x1b410` | `0x01b410` | 120 B | ✅ **已解决**：重复帧过滤，非 MIC（见 4.3） |
| **P0** | `sub_0x135b0` | `0x0135b0` | 124 B | ✅ **已解决**：`__ECNT_HOOK(0x1a)` → `0x0b620` 命令分发器（见 docs/09） |
| P1 | `gpon_isr` | `0x01bcc4` | 4104 B | 中断分类与取帧触发条件 |
| P1 | `ploam_eqd_adjustment` | `0x014bd0` | 1052 B | O5 精细测距校正 |
| P2 | `gpon_qos_init` | — | — | BWMAP / DBA 初始化 |
| P2 | `gponDevSetGemInfo` | `0x01ecc8` | — | GEM Port 表建立 |

**两个 P0 全部解决，且都比预期简单**：
- MIC 不存在 → 不需要实现 AES-CMAC，省掉一大块工作量
- 命令下发走统一分发器 → 不用逐条追寄存器时序

GPON 上线的技术路径已经清晰，剩下的都是工程量而非未知风险。
