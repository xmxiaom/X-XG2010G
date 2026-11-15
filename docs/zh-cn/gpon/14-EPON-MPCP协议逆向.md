# EPON MPCP 协议逆向

> 对象：原厂 `xpon.ko` 的 MPCP（Multi-Point Control Protocol）实现
> MPCP 是 EPON 的「PLOAM 等价物」——负责 ONU 在 OLT 上注册、测距、上报
> 方法：`mpcpStateStr` 字符串表 + 反汇编交叉印证

---

## 1. MPCP 与 GPON PLOAM 的对应关系

| 概念 | GPON | EPON |
|---|---|---|
| 多点控制协议 | PLOAM | **MPCP** |
| 逻辑连接标识 | Alloc-ID / GEM Port | **LLID**（0–7，最多 8 个） |
| 注册消息 | Serial_Number_ONU / Assign_ONU_ID | **REGISTER_REQ / REGISTER / REGISTER_ACK** |
| 测距 | Ranging_Time + EqD | **GATE + REPORT + 时间戳** |
| 去注册 | Deactivate_ONU-ID | **DEREGISTER** |
| 状态机 | O1–O7 | **DiscState**（DISCOVERY / WAIT / REGISTER_REQUEST / REGISTERED / DENIED / DEREGISTER） |

**关键差异**：GPON 是「一个 ONU 一个 ONU-ID」，EPON 是「**一个 ONU 最多 8 个 LLID**」，
每个 LLID 独立跑一遍注册状态机。这解释了为什么 EPON 代码里到处是 `llidIndex`。

---

## 2. MPCP 状态机（从字符串表完整还原）

### 2.1 状态枚举

```
MPCP DiscState:
  DISCOVERY          初始，等 GATE
  WAIT               已发 REGISTER_REQ，等 REGISTER
  REGISTER_REQUEST   收到 REGISTER，准备回 ACK
  REGISTERED         注册完成，正常收发
  DENIED             认证失败（AuthFail）
  DEREGISTER         被去注册
```

对应日志字符串（原厂 `.rodata`）：

```
MPCP DiscState is REGISTER_REQUEST
Epon Receive MPCP Register, Now State is REGISTER_REQUEST, llidDscvSts=%x
eponMpcpRgstIntHandler  mpcpDiscState==MPCP_STATE_REGISTERED
AuthFail DiscState Change to DENIED, Report EPON_DENIED Event
DiscState Change to DEREGISTER , Report DEREGISTER Event
MPCP Status: Registered / Registering / Unknown
```

### 2.2 完整状态迁移

```
                    ┌──────────────┐
                    │  DISCOVERY   │  ← 上电 / 去注册后
                    └──────┬───────┘
                           │ 收到 GATE (Discovery Gate)
                           ▼
                    ┌──────────────┐
                    │     WAIT     │  发 REGISTER_REQ
                    └──────┬───────┘
                           │ 收到 REGISTER
                           ▼
                    ┌──────────────────┐
                    │ REGISTER_REQUEST │  回 REGISTER_ACK
                    └──────┬───────────┘
                           │ ACK 成功
                           ▼
                    ┌──────────────┐
                    │  REGISTERED  │  正常收发
                    └──┬───┬───┬───┘
                       │   │   │
        收到 REREGISTER │   │   │ 认证失败
        收到 DEREGISTER │   │   │
                        ▼   ▼   ▼
                  ┌──────────┐ ┌────────┐
                  │DEREGISTER│ │ DENIED │
                  └────┬─────┘ └────────┘
                       │ 重新 eponStart
                       ▼
                  ┌──────────────┐
                  │  DISCOVERY   │
                  └──────────────┘
```

---

## 3. 关键寄存器

从字符串 `E_MPCP_STAT (0xBFB66120)` 和 `E_MPCP_RGST_STAT (0xBFB66124)` 直接得到：

| 偏移 | 原厂名 | 绝对地址 | 作用 |
|---|---|---|---|
| `0x0120` | `E_MPCP_STAT` | `0xBFB66120` | MPCP 状态 |
| `0x0124` | `E_MPCP_RGST_STAT` | `0xBFB66124` | 注册状态 |
| `0x007c` | `e_llid_dscvry_ctrl` | — | **LLID 发现控制**（核心） |
| `0x0080` | `e_llid0_dscvry_sts` | — | LLID0 发现状态 |
| `0x00d4` | 发现门限计数器 | — | 上限 `0x5f`（95） |
| `0x00f4` | 注册等待时间 | — | 超时值 |
| `0x01d8` | MPCP 超时间隔 | — | — |
| `0x05f0` | `INT_ENABLE`（物理层） | — | LOS/LOF/PHY_READY |
| `0x05f4` | `INT_CLEAR`（物理层） | — | — |
| `0x05f8` | `INT_STATUS`（物理层） | — | — |

**注意 `0x0120/0x0124` 的绝对地址 `0xBFB66120/0xBFB66124`**，
由此可反推 `g_EPON_MAC_BASE = 0xBFB60000`，与 GPON 的 `0xBFB60000` **是同一个基址**——
两者共用 `0x6xxx` 这段窗口，只是物理映射到不同地址（GPON `0x1fb64000`，EPON `0x1fb66000`）。

---

## 4. 核心处理函数

### 4.1 `eponMpcpDiscvGateIntHandler`（`0x03a0bc`，2092 字节）

**触发**：收到 GATE（Discovery Gate）中断。

```
1. 前置检查
   if (gpPonSysData->[0] != 2)  return;        // 模式不是 EPON

2. 发现门限检查（防刷）
   cnt = get_xpon_data(BASE + 0x60d4) & 0xffff
   if (cnt > 0x5f) {                            // 95 次上限
       set_xpon_data(BASE + 0x60d4, 0x5f);      // 钳位
       set_xpon_data(BASE + 0x60f4, 0x8efff1);  // 写超时值
       goto done;
   }
   if (cnt == 0) {
       set_xpon_data(BASE + 0x60d4, gMpcpSynctime);   // 初始化同步时间
       cnt = get_xpon_data(BASE + 0x60d4);
       ...
   }

3. 状态检查
   state = eponDrv->[0x18]
   if (state == 2 || state == 6) {
       if (eponMacCheckRegreqDoneBit() < 0)
           eponRstMpcpCmdDone();                // 复位命令完成位
   }

4. 发送 REGISTER_REQUEST
   if (state == 1 || state == 2) {
       xmcs_report_event(...);
       if (state == 2 && eponDrv->[0x1a] != 0)
           eponDrv->[0x1a]--;                   // 重试计数递减
       eponSetGrantLength(...);                 // 设置授权长度
       ...
   }

5. 读 LLID 发现控制寄存器
   dscvCtrl = get_xpon_data(BASE + 0x607c)      // e_llid_dscvry_ctrl
   ...
```

**日志印证**：
```
enter eponMpcpDiscvGateIntHandler gMpcpSynctime =0x%x
eponMpcpDiscvGateIntHandler llidIndex=%d state=%d
eponMpcpDiscvGateIntHandler READ_REG_WORD(e_llid_dscvry_ctrl)
eponMpcpDiscvGateIntHandler enter while dscvCtrl.Raw=%08X
Epon Send MPCP REGISTER_REQUEST DiscvGate Finished, llidIndex = %d
eponMpcpDiscvGateIntHandler done bit timeout
```

### 4.2 `eponMpcpRgstIntHandler`（`0x03b010`，1708 字节）

**触发**：收到 REGISTER 消息中断。

```
1. 使能检查
   if (eponDrv->[0x18] == DISABLE)  return;
   if (!llidEntry->eponDiscFsm.begin)  return;

2. LLID 校验（关键！）
   llid = get_xpon_data(BASE + 0x60xx)          // 读当前 LLID
   if (llid != 报文里的 LLID) {
       报错 "ERR: llidDscvSts.Bits.llidValid error"
       return;
   }

3. 状态检查
   if (state == REGISTER_REQUEST) {
       // 回 REGISTER_ACK
       if (flag_rx == nack) {
           // 收到 Nack，进入 silent
           进入静默期，等待重试
       } else {
           // 回 Ack，握手完成
           状态 → REGISTERED
           xmcs_report_event(REGISTER)
       }
   }

4. 特殊消息
   if (收到 REREGISTER) {
       report DEREGISTER event
       状态 → DEREGISTER
   }
   if (收到 DEREGISTER) {
       "ONU receive DEREGISTER ,do epon mac eponStart"
       重新 eponStart
   }
   if (AuthFail) {
       状态 → DENIED
       report EPON_DENIED event
   }
```

**日志印证**：
```
enter eponMpcpRgstIntHandler llidIndex=%d, mpcpDiscState=%d
eponMpcpRgstIntHandler LLID=%d llidEntry_p->llid =%d
eponMpcpRgstIntHandler LLID=%d flag_rx=nack
Epon received MPCP Nack change to silent, time %d
Epon Send MPCP Ack, Mpcp Handshake Done,llidIndex = %d
Epon Send MPCP Nack, MPCP Retry
Epon Receive MPCP Reregister Msg LLID %d
Epon Receive MPCP DEREGISTER Message LLID %d
```

### 4.3 `eponMpcpLocalDergstr`（`0x03b6bc`，552 字节）

**触发**：本地主动去注册（`ioctl EPON_IOCTL_LLID_DEREGISTER`）。

```
if (llidIndex > 7)  return -EINVAL;
if (!mpcpDiscFsm->begin)  return;
等待 done bit
dscvCtrl = get_xpon_data(BASE + 0x607c)
dscvCtrl |= DEREGISTER 位
set_xpon_data(BASE + 0x607c, dscvCtrl)
report DEREGISTER event
```

### 4.4 其他处理器

| 函数 | 地址 | 大小 | 触发 |
|---|---|---:|---|
| `eponMpcpRgstReqIntHandler` | `0x03a924` | 156 B | REGISTER_REQ 完成 |
| `eponMpcpRgstAckIntHandler` | `0x03a9c0` | 384 B | REGISTER_ACK 完成 |
| `eponMpcpTmOutIntHandler` | `0x03ba98` | 368 B | MPCP 超时 |
| `eponMpcpDscvFsmWaitHandler` | `0x03bc08` | 352 B | 等待状态轮询 |
| `eponMpcpGntOvrRunIntHandler` | `0x03bd68` | 36 B | 授权溢出 |
| `eponMpcpRptOvrIntvalIntHandler` | `0x03bd8c` | 204 B | 上报间隔溢出 |
| `eponMpcpSetDiscvRgstAck` | `0x03b8e4` | 100 B | 设置 ACK/NACK |
| `eponMpcpRegisterDone` | `0x03befc` | 164 B | 注册完成收尾 |
| `eponTimeDrftIntHandler` | `0x03b948` | 336 B | 时钟漂移补偿 |

---

## 5. 中断服务 `eponIsr`（`0x02fa84`，1120 字节）

从中断日志串可以完整还原中断分派表：

```
EponIsr MPCP_TIMEOUT_INT
EponIsr REGISTER_REQ_DONE_INT
EponIsr REGISTER_ACK_DONE_INT
EponIsr LLID%d_RCV_RGST_INT
ERR: GNT_BUF_OVRRUN_INT eponMpcpGntOvrRunHandler ret=%d
ERR: RPT_OVERINTVL_INT eponMpcpRptOvrIntvalIntHandler ret=%d
```

| 中断 | 处理器 | 作用 |
|---|---|---|
| `MPCP_TIMEOUT_INT` | `eponMpcpTmOutIntHandler` | 注册超时 |
| `REGISTER_REQ_DONE_INT` | `eponMpcpRgstReqIntHandler` | 请求发送完成 |
| `REGISTER_ACK_DONE_INT` | `eponMpcpRgstAckIntHandler` | ACK 发送完成 |
| `LLIDn_RCV_RGST_INT` | `eponMpcpRgstIntHandler` | 收到 REGISTER（每 LLID 一位） |
| `GNT_BUF_OVRRUN_INT` | `eponMpcpGntOvrRunIntHandler` | 授权缓冲溢出 |
| `RPT_OVERINTVL_INT` | `eponMpcpRptOvrIntvalIntHandler` | 上报间隔溢出 |

---

## 6. LLID 管理

### 6.1 LLID 相关寄存器

| 偏移 | 名称 | 作用 |
|---|---|---|
| `0x007c` | `llid_dscvry_ctrl` | **发现控制**（发 REGISTER_REQ / DEREGISTER） |
| `0x0080` | `llid0_dscvry_sts` | LLID0 发现状态 |
| `0x0104` | `mac_addr_cfg` | MAC 地址配置（bit31 写、bit16 busy、bit0 dword） |
| `0x0108` | `mac_addr_value` | MAC 地址值 |
| `0x0124` | `report_cfg` | 上报配置 |
| `0x0134` | `report_bitmap` | 上报位图 |
| `0x61e0–0x61ec` | LLID 位图 | 4 个寄存器，每两个 LLID 的 9 位标志 |

### 6.2 LLID 位图格式（从初始化表还原）

初始化表里 `0x61e0–0x61ec` 四个寄存器全是 `0x01ff01ff`，掩码也是 `0x01ff01ff`：

```
每个 32 位寄存器 = 两个 LLID 的 9 位字段
  低 16 位：[15:9] 保留 | [8:0] LLID(2n) 标志
  高 16 位：[31:25] 保留 | [24:16] LLID(2n+1) 标志

0x01ff = 0b1_1111_1111  →  9 位全 1 = 该 LLID 有效
```

所以 `0x01ff01ff` 表示「两个 LLID 都有效」。

### 6.3 LLID 关键操作

| 函数 | 地址 | 作用 |
|---|---|---|
| `eponLlidEnable` | `0x030a10` | 使能 LLID |
| `eponLlidDisable` | `0x030c10` | 禁用 LLID |
| `eponMacSetLlidEnableMask` | `0x03e41c` | 批量设置使能掩码 |
| `eponSetLlidKey` | `0x03d2f4` | 设置 LLID 加密密钥 |
| `eponSetLlidFec` | `0x03d698` | 设置 LLID FEC |
| `eponSetLlidQueThrod` | `0x03db28` | 设置 LLID 队列门限 |
| `eponGetLlidValue` | `0x03ea50` | 读 LLID 值 |
| `eponSetLlid0DeafultThreshold` | `0x030d68` | LLID0 默认门限 |

---

## 7. 与上游开源实现的对照

| 项 | 原厂 | 上游 `epon.c` | 差异 |
|---|---|---|---|
| MPCP 状态机 | 6 状态完整实现 | 简化版（只处理 REGISTER/DEASSIGN） | **上游缺 DISCOVERY/WAIT 的完整迁移** |
| LLID 数量 | 8 个（0–7） | 支持多 LLID | 一致 |
| REGISTER_REQ 发送 | 有独立中断处理 | 有 | 一致 |
| NACK 重试 | 有静默期机制 | 未实现 | **上游缺重试退避** |
| REREGISTER | 有 | 未实现 | **上游缺** |
| DENIED 状态 | 有 | 未实现 | **上游缺认证失败处理** |
| 时钟漂移补偿 | `eponTimeDrftIntHandler` | 未实现 | **上游缺** |

**结论：上游 EPON 实现只覆盖了「能注册上」的最小路径，缺容错和异常处理。**
这解释了为什么上游 EPON 在某些 OLT 上不稳定。

---

## 8. 对 GPON 适配的启发

### 8.1 两者的状态机结构高度相似

| GPON | EPON | 共同点 |
|---|---|---|
| O1 INITIAL | DISCOVERY | 初始等待 |
| O2 STANDBY | WAIT | 已响应，等确认 |
| O3 SERIAL | REGISTER_REQUEST | 已上报身份 |
| O4 RANGING | （含在 REGISTER 里） | 测距 |
| O5 OPERATION | REGISTERED | 正常运营 |
| O6 POPUP | （超时重试） | 失步恢复 |
| O7 EMERGENCY | DENIED | 异常停止 |

**这说明可以设计一套统一的状态机框架，GPON/EPON 各填各的状态映射。**
这正是用户「gpon 和 epon 一起适配」的技术基础。

### 8.2 共用代码的可能性

| 模块 | 可共用 | 说明 |
|---|---|---|
| 寄存器访问 `readl/writel` | ✅ | 只换 base |
| 定时器框架 | ✅ | TO1/TO2 vs MPCP timeout |
| 状态机调度骨架 | ✅ | 状态枚举不同但流程一致 |
| 事件上报 `xmcs_report_event` | ✅ | 两者都调 |
| 退避计数 | ✅ | 逻辑相同 |
| **PLOAM / MPCP 报文格式** | ❌ | 完全不同 |
| **初始化寄存器序列** | ❌ | 各 112 条 / 112 条，内容不同 |
| **中断位定义** | ❌ | GPON 30 位 vs EPON 两套 |

### 8.3 建议的适配架构

```
airoha-xpon-core.c          ← 状态机框架、定时器、事件、退避（共用）
    ├── airoha-xpon-gpon.c  ← PLOAM、GPON 寄存器、GPON 中断
    └── airoha-xpon-epon.c  ← MPCP、EPON 寄存器、EPON 中断（改造上游）
```

而不是现在上游的「EPON 和 XGPON 各自独立实现、main.c 里二元判断」。

---

## 9. 产物与验证

```
docs/13-EPON寄存器表逆向.md     五张表 349 条（含与上游交叉验证）
docs/14-EPON-MPCP协议逆向.md    本篇
build/epon-tables.txt           寄存器表数据
tools/re_epon_tables.py         表提取器
```

---

## 10. 下一步

| 优先级 | 内容 |
|---|---|
| P0 | `eponDetectPhyLosHandler` / `eponDetectPhyReady` —— 这是 GPON `gpon_detect_los_lof` 缺口的直接参照 |
| P1 | `eponMacSwReset` / `eponMacRestart` —— 复位流程 |
| P1 | `epon_set_queue_threshold` —— DBA 门限 |
| P2 | `eponMacIoctl`（2828B）—— 用户态接口全集 |

**LOS 检测是当前最高价值的目标**：GPON 那边这一块是空白，而 EPON 有完整实现，
两边的 PHY 层是同一颗芯片（EN7572），逻辑应该可以直接迁移。
