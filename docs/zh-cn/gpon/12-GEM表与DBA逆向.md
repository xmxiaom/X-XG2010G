# GEM Port 表与 DBA 逆向

> 对象：`gponDevSetGemInfo` / `gponDevGetGemInfo` / `gponDevResetGemInfo` 与 `gpon_qos_init`
> 作用：GEM Port 是所有业务数据的承载，OMCI 下发的每条业务最终都落成 GEM 表项

---

## 1. GEM 表访问协议（核心）

原厂用**「索引 → 命令 → 轮询完成」**的三步握手访问 GEM 表：

```
0x0040  GEM PORT INDEX      写入操作码 + 端口号 + 标志
0x0044  GEM PORT INDEX STATUS   bit31 = 完成标志，bit0 = 结果
```

### 1.1 写操作（`gponDevSetGemInfo`）

```c
int gponDevSetGemInfo(u16 gem_port, u8 dir, u8 encrypt)
{
        u32 cmd = 0;
        int retry = 3000;                            // 0xbb8

        /* 端口号必须 12 位以内 */
        if ((gem_port & 0xfffff000) != 0)
                return -EINVAL;                     // -0xe

        cmd |= BIT(31);                              // 有效/写命令
        cmd |= (dir == 1)     ? BIT(16) : 0;         // 方向
        cmd |= (encrypt == 1) ? BIT(17) : 0;         // 加密
        cmd = (cmd & ~0xfff) | (gem_port & 0xfff);   // 端口号低 12 位

        set_xpon_data(BASE + 0x4040, cmd);

        /* 轮询完成 */
        do {
                status = get_xpon_data(BASE + 0x4044);
                if (status & BIT(31))
                        return 0;                    // 完成且成功
                __const_udelay(0x1102c);             // ≈ 68ms？
        } while (--retry);

        return -EIO;                                 // -0x3e，超时
}
```

**关键点**：
- `cmds` 的 **bit31 是「有效」位**，bit16 是方向，bit17 是加密
- 端口号只用低 12 位（0–4095）
- 完成后 `0x0044` 的 **bit31 置位表示完成**，此时 `bit0` 表示成功/失败
- 轮询上限 **3000 次**，每次 `__const_udelay(0x1102c)`，超时返回 `-EIO`

### 1.2 读操作（`gponDevGetGemInfo`）

```c
int gponDevGetGemInfo(u16 gem_port)
{
        cmd = (gem_port & 0xfff);        // bit31 = 0 表示读
        set_xpon_data(BASE + 0x4040, cmd);

        status = get_xpon_data(BASE + 0x4044);
        if (!(status & BIT(31)))
                delay1us(...);           // 等完成
        /* 结果在 status 的低位 */
        return (status & 1) ? 成功 : 失败;
}
```

读和写的唯一区别是 **bit31**：读=0，写=1。

---

## 2. 相关寄存器

| 偏移 | 名称 | 作用 |
|---|---|---|
| `0x0040` | GEM PORT INDEX | 写：命令+端口号；读：端口号 |
| `0x0044` | GEM PORT INDEX STATUS | bit31 完成，bit0 结果 |
| `0x0048` | OMCI GEM PORT ID | OMCI 通道专用 GEM 号 |
| `0x004c` | GEM TABLE INITIALIZATION CONTROL | 表整体复位 |
| `0x0140` | GEM PORT INDEX TABLE ACCESS CONTROL FOR MIB | MIB 表访问 |
| `0x0144` | GEM PORT INDEX TABLE ACCESS STATUS | MIB 表状态 |
| `0x0148` | GEM PORT INDEX TABLE INITIALIZATION CONTROL | MIB 表复位 |
| `0x0100` | TX FCS TABLE INITIALIZATION CONTROL | FCS 表复位 |
| `0x0300` | RX GEM COUNTER | 收到的 GEM 帧数 |
| `0x030c` | TX GEM COUNTER | 发出的 GEM 帧数 |
| `0x0430` | GEM HEADER ONE BIT ERROR COUNTER | GEM 头 1 bit 错误 |
| `0x0434` | GEM HEADER TWO BIT ERROR COUNTER | GEM 头 2 bit 错误 |
| `0x0438` | GEM HEADER UNCORRECTABLE ERROR COUNTER | GEM 头不可纠错 |

---

## 3. T-CONT 与 Alloc-ID

### 3.1 Alloc-ID 寄存器（`0x0020`–`0x003c`）

16 个 T-CONT 的 Alloc-ID 分 8 个寄存器存放，**每寄存器放两个**：

| 偏移 | 内容 |
|---|---|
| `0x0020` | T-CONT0（低 16 位）+ T-CONT1（高 16 位） |
| `0x0024` | T-CONT2 + T-CONT3 |
| `0x0028` | T-CONT4 + T-CONT5 |
| `0x002c` | T-CONT6 + T-CONT7 |
| `0x0030` | T-CONT8 + T-CONT9 |
| `0x0034` | T-CONT10 + T-CONT11 |
| `0x0038` | T-CONT12 + T-CONT13 |
| `0x003c` | T-CONT14 + T-CONT15 |

写法：

```c
/* 设置第 n 个 T-CONT 的 Alloc-ID */
u32 off = AIROHA_GPON_ALLOC_ID_BASE + (n / 2) * 4;
u32 shift = (n % 2) * 16;
gpon_rmw(xpon, off, 0xffff << shift, (alloc_id & 0xffff) << shift);
```

### 3.2 主要函数

| 函数 | 地址 | 大小 | 作用 |
|---|---|---|---|
| `gponDevGetTcontInfo` | `0x022998` | 144 B | 读 T-CONT 配置 |
| `gponDevResetAllocId` | `0x01f43c` | 376 B | 复位所有 Alloc-ID |
| `gponDevDumpTcontInfo` | `0x023b8c` | 164 B | 调试转储 |
| `xmcs_create_tcont_info` | `0x063314` | 204 B | 上层创建 T-CONT |
| `xmcs_get_tcont_info` | `0x063190` | 236 B | 上层查询 |
| `xmcs_remove_tcont_info` | `0x06327c` | 152 B | 上层删除 |

---

## 4. DBA 相关寄存器

| 偏移 | 名称 | 说明 |
|---|---|---|
| `0x0220` | BW MAP FILTER CONTROL | BWMAP 过滤配置 |
| `0x0224` | BW MAP SFIFO STATUS | BWMAP 单帧 FIFO 状态 |
| `0x0230` | THE MAXIMUM USED ENTRIES OF TX FIFO | TX FIFO 峰值 |
| `0x0234` | THE RECEIVED BWM GRANT STATUS | 收到的授权状态 |
| `0x0250` | BW MAP BFIFO STATUS | BWMAP 突发 FIFO 状态 |
| `0x0340` | DBA_BACK_DOOR_FOR_SW | DBA 后门（软件写入） |
| `0x0344` | DBA_TOTAL_BUF_SIZE | DBA 总缓冲 |
| `0x0348` | DBA_GREEN_SIZE | 绿色（保证）带宽 |
| `0x034c` | DBA_YELLOW_SIZE | 黄色（超额）带宽 |
| `0x0350`/`0x0354` | DBG_SLIGHT_MODIFY_SIZE_1/2 | 微调 |
| `0x0360` | PLOAMD_FILTER_IN_O5 | O5 态 PLOAMd 过滤 |

### 4.1 `gpon_qos_init`（`0x0265a0`，252 字节）

初始化 DBA 的默认参数。原厂逻辑（按调用点推断）：

```
1. 写 DBA_TOTAL_BUF_SIZE 为总缓冲大小
2. 写 DBA_GREEN_SIZE / DBA_YELLOW_SIZE 为默认门限
3. 配置 BW MAP FILTER CONTROL
4. 清 BW MAP SFIFO / BFIFO 状态
```

具体数值由上层 OMCI 配置决定，驱动侧只需提供接口。

---

## 5. 掉线恢复

原厂有一套完整的 GEM/Alloc-ID 备份恢复机制，用于掉线重连后快速重建业务：

| 函数 | 地址 | 作用 |
|---|---|---|
| `xpon_exec_gem_recover` | `0x00cf04` | 恢复执行入口 |
| `gpon_recover_backup_allocId` | `0x0295e8` | 备份 Alloc-ID |
| `gpon_recover_create_allocId` | `0x0296a4` | 重建 Alloc-ID |
| `gpon_recover_delete_allocId` | `0x029768` | 删除 Alloc-ID |
| `gpon_recover_backup_gemport` | `0x0298ec` | 备份 GEM 端口 |
| `gpon_recover_create_gemport` | `0x029f2c` | 重建 GEM 端口（1040 字节，最复杂） |
| `gpon_recover_delete_gemport` | `0x02a33c` | 删除 GEM 端口 |
| `gpon_check_gem_exist` | `0x029d2c` | 检查 GEM 是否已存在 |
| `gpon_check_gem_encrypt` | `0x029de0` | 检查 GEM 加密状态 |

**这套机制对国内运营商很关键**：电信/联通的 OLT 在 ONU 掉线重连后会重新下发 OMCI，
如果 ONU 侧没保存业务配置，重新建立耗时长（几十秒）。原厂能在秒级恢复。

---

## 6. 上层业务接口

原厂有一套 `gwan_*` 和 `xmcs_*` 的函数族，是 OMCI 与驱动之间的中间层：

| 层级 | 函数族 | 作用 |
|---|---|---|
| OMCI → | `xmcs_create_gem_port` 等 | OMCI ME 翻译成驱动调用 |
| 中间层 | `gwan_create_new_gemport` / `gwan_config_gemport` | 业务配置（VLAN、QoS、ACL） |
| 驱动 → | `gponDevSetGemInfo` | 实际写寄存器 |

移植时的分层建议：

```
OMCI 消息 → airoha_xpon_gem_port_create() → airoha_xpon_gpon_set_gem_info() → 0x0040/0x0044
```

**不要照搬 `gwan_*` 那一层**——它耦合了 EcoNet 的 VLAN/QoS 引擎，
上游 ImmortalWrt 有 DSA / netdev 的等价机制，应该用上游的。

---

## 7. 移植实现建议

### 7.1 GEM 表访问（直接可写）

```c
int airoha_xpon_gpon_set_gem_info(struct airoha_xpon *xpon, u16 gem_port,
                                  bool upstream, bool encrypt)
{
        u32 cmd;
        int retry = AIROHA_GPON_GEM_POLL_MAX;   /* 3000 */

        if (gem_port & ~0xfff)
                return -EINVAL;

        cmd = BIT(31) |                          /* 写命令 */
              (upstream ? BIT(16) : 0) |
              (encrypt ? BIT(17) : 0) |
              (gem_port & 0xfff);

        gpon_wr(xpon, AIROHA_GPON_GEM_PORT_INDEX, cmd);

        do {
                u32 status = gpon_rd(xpon, AIROHA_GPON_GEM_PORT_INDEX_STATUS);

                if (status & BIT(31))
                        return (status & 1) ? 0 : -EIO;
                udelay(AIROHA_GPON_GEM_POLL_DELAY_US);
        } while (--retry);

        return -ETIMEDOUT;
}
```

### 7.2 注意事项

1. **轮询延时不能太短**：原厂用 `__const_udelay`，值较大（约几十微秒）
2. **必须检查 `status & 1`**：bit31 只表示「完成」，不代表「成功」
3. **失败返回 `-EIO` 而非 `-ETIMEDOUT`**：原厂完成但失败时返回 `-EIO`
4. **GEM 表操作要加锁**：多个 OMCI 消息可能并发访问同一个索引寄存器

---

## 8. 剩余未知

| 项 | 说明 |
|---|---|
| `gpon_qos_init` 具体写入值 | 需完整反汇编 252 字节 |
| `gpon_recover_create_gemport` | 1040 字节，恢复流程的核心，未细读 |
| `DBA_*` 寄存器的语义 | 表里的说明是按名字推断，需实测验证 |
| `xmcs_*` 与 OMCI ME 的映射 | 需对照 OMCI 规范逐项确认 |

这些在实现阶段按需深入，不影响 GEM 表访问这一核心协议。
