# 01 · 原厂 XG2010G 固件里的 GPON 实现分析

数据来源：`XG2010G_fw_1.0.0.13` 原厂 NAND 备份 → `tclinux.bin`（偏移 `0x366940`，squashfs4）
分析对象：`/lib/modules/5.4.55/xpon.ko`（1,176,304 字节，`license=GPL`）

---

## 1. 原厂是 GPON / XGS-PON 双模

`/etc/init.d/xponconfig`（START=00，最早加载）：

```sh
XPONMODE=`cat /proc/tc3162/sys_xpon_mode`
if [ "$XPONMODE" = "1" ] ; then          # 1 = GPON 2.5G/1.25G
    cp /tmp/en7572_bob_0.conf /etc/lddla/en7572_bob.conf
    insmod /lib/modules/xpon.ko            # ← GPON MAC
    BBF247_FLAG_PROC="/proc/gpon/bbf247Flag"
else                                     # 0 = XGS-PON 10G
    cp /tmp/en7572_bob_1.conf /etc/lddla/en7572_bob.conf
    insmod /lib/modules/xpon_10g.ko
    BBF247_FLAG_PROC="/proc/xgpon/bbf247Flag"
fi
insmod /lib/modules/5.4.55/phy_10g.ko
insmod /lib/modules/5.4.55/xponmap.ko
insmod /lib/modules/5.4.55/ponvlan.ko
insmod /lib/modules/5.4.55/gpon_flow.ko
insmod /lib/modules/5.4.55/ponmacfilter.ko
ifconfig pon up
ifconfig omci up
```

关键点：**同一套硬件、同一个光模块，只换 `xpon.ko` 与一份 BOB 校准表**。
说明 AN7581 的 PON MAC 硬件本身支持 GPON 速率，缺的纯粹是驱动实现。

## 2. `xpon.ko` 的 GPON 证据

模块信息：

```
name      = xpon
license   = GPL
vermagic  = 5.4.55 SMP mod_unload aarch64
opkg 包   = kmod-xpon_1g  ("Kernel module for econet xpon_1g")
源码路径  = .../xpon_1g/src/gpon/gpon_dev.c
```

字符串统计：`gpon` ×345、`ploam` ×97、`gtc` ×11、`gem port` ×20、`omci` ×29。

### 2.1 PLOAM 消息全集（ITU-T G.984.3）

```
PLOAM: Rx Upstream_Overhead.(...)            state O%d
PLOAM: Send password message.(...)
PLOAM: Receive Assign_ONU_ID message.(...)
PLOAM: Receive Ranging_Time message.(...)
PLOAM: Receive Configure_PortID message.(...)
PLOAM: Receive Encrypted_PortID message.(...)
PLOAM: Receive Request_Key message.(...)
PLOAM: Receive Request_Password message.(...)
PLOAM: Receive Deactivate_OnuID message.(...), SPF:0x%08x
PLOAM: ONU detected LOS/LOF in O4
Send REI PLOAM message successful
ploam_recv_deactivate_onu
```

### 2.2 激活状态机 O1–O5 与定时器

```
TO1 timer timeout. Current State:O%d, Next State:O2
TO2 timer timeout. Current State:O%d, Next State:O1
Set the O3/O4 type 3 preamble offset %d successful.
PLOAM: main path EqD in O5, %x, deltaEqd = %x.
PLOAM: Setting the main path EqD in O5, ByteDelay:%x, BitDelay:%x.
PLOAM: Adjust the main path EqD in O4, Offset:%x.
Modify the EqD offset = %x at ranging_time PLOAM in O4
Disable Ranging in O5! / Enable Ranging in O5!
PLOAM: May be SUF (The Ranging of this ONU has failed)
```

### 2.3 GTC / GEM / 加密

```
DBG_RX_GTC_CNT (0xBFB64308)
DBG_DS_GTC_EXTR_ETH_CNT (0xBFB64300)
GEM ID:%d, Alloc ID:%d, If Idx:%d, Loopback:%s, Encryption:%s, valid is %d, chn:%d
Gem Port:%d, SW Tx:%u, SW Rx:%u, HW Tx:%llu, HW Rx:%llu
gponRecovery.gemPort[%d].gemPortId / .allocId / .channel / .encryption / .ani
Set omci gem port : %d
Send REI PLOAM message successful
```

### 2.4 掉线恢复

```
gpon_recover_create_allocId / delete / backup
gpon_recover_create_gemport / delete / backup
------ bakAllocId ------   ------ bakGemport ------
```

## 3. 配套模块职责

| 模块 | 作用 | 与 GPON 的关系 |
|---|---|---|
| `xpon.ko` | PON MAC 主驱动 | **GPON 的核心，缺它就上不了线** |
| `xponmap.ko` | GEM Port ↔ Alloc-ID 映射 | GPON 下发规则表 |
| `gpon_flow.ko` | GPON 数据流分类转发 | GPON 专属（EPON 不用） |
| `gpon_igmp.ko` | GPON 侧 IGMP snooping | IPTV 组播 |
| `ponvlan.ko` | PON VLAN / 802.1p | 业务 VLAN 隔离 |
| `ponmacfilter.ko` | MAC 过滤 | 防私接 |
| `en7572.ko` | EN7572 光前端 | GPON/XGS-PON 共用 |
| `econet_bob.ko` | BOSA 校准 | 按 `bob_0/bob_1` 切换 |

## 4. 对移植工作的含义

1. **不需要从零发明协议** —— `xpon.ko` 是 GPL 的，源码可依法索取（见 `docs/03`）。
2. **不需要重写 OMCI** —— 本项目用户态 `airoha-pond` 已经支持 CTC LOID ME。
3. **需要重写的是内核侧 `airoha-xpon.ko` 的线路层**：把 `_xgpon_*` / `_epon_*` 之外补一条 `_gpon_*` 分支（GTC 帧、PLOAM 状态机、DBA/T-CONT、GEM、AES、FEC）。
4. **寄存器映射必须拿到** —— `xpon.ko` 可以直接告诉你要写哪些寄存器，比读硬件手册可靠。
