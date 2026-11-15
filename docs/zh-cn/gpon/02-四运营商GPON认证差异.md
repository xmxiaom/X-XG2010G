# 02 · 国内四大运营商 GPON 认证与 OMCI 差异对照

> 各省公司策略差异很大，下表的「常见形态」只作工程参考，实际以当地 OLT 为准。

---

## 1. 认证方式对照

| 运营商 | 主流认证 | 备选 | 需要的 OMCI 能力 |
|---|---|---|---|
| **中国电信** | LOID + LOID 密码 | SN + 密码（36 位 Registration ID） | **CTC LOID Auth ME 0xFFF9**、CTC 扩展 |
| **中国联通** | SN + 密码（10 位 ASCII，可空） | LOID（少数省） | 标准 ONT-G 的 Password 属性 |
| **中国移动** | SN（厂商码 4 字节 + 序列号 8 位） | SN + 密码（部分省） | 标准 ONT-G |
| **中国广电** | SN 为主 | LOID（部分省网） | 标准 ONT-G，省网差异最大 |

要点：

- **只有电信强依赖 CTC 私有 ME**。联通 / 移动 / 广电都走标准 ITU-T G.984 OMCI，互通门槛低。
- 电信「SN + 36 位 Registration ID」在协议层等同标准 Password，但长度和字符集要求不同。
- 移动的 SN 厂商码决定 OLT 是否放行，常见 `CMCC` / `ZTEG` / `HWTC` / `FHTT` 等，**必须和当地入库型号一致**。

## 2. 业务 VLAN（常见形态，务必以实测为准）

| 运营商 | Internet | IPTV | VoIP |
|---|---|---|---|
| 电信 | 41（多数省） | 45 | 46 |
| 联通 | 各省差异大 | 各省差异大 | — |
| 移动 | 41 | 48 | — |
| 广电 | 各省差异大 | 各省差异大 | — |

GPON 侧一般要求：

- 上行 T-CONT 与 GEM Port 由 OLT 通过 OMCI 下发（`Configure_Port-ID` / `Set_T-CONT`）
- 业务 VLAN 由 ONU 侧打 tag（`ponvlan.ko` 负责），或在 OLT 侧已剥离
- IPTV 组播需要 IGMP snooping + 组播 GEM，电信还要求 CTC 扩展组播模板

## 3. 本项目能力对照（截至当前上游状态）

| 需求 | 归属 | 本项目状态 |
|---|---|---|
| CTC LOID ME 0xFFF9 | 用户态 OMCI | ✅ 已实现（`airoha-pond` 0.3.0，仅在有 LOID 时才上报） |
| Password / Registration-ID | 用户态 OMCI | ✅ 已实现（LOID 与 Password 显式二选一） |
| 标准 ONT-G（SN、Equipment-ID、Vendor-ID） | 用户态 OMCI | ✅ 已实现，`luci-app-onu` 可改 |
| 业务 VLAN / 802.1p | 用户态 + `ponvlan` | ✅ 已实现 |
| 上网（桥接 / DHCP / PPPoE） | 用户态 | ✅ 已实现 |
| IPTV 组播 / IGMP-Proxy | 用户态 + `xpon_igmp` | ✅ 已实现 |
| 语音（H.248 / SIP / IMS） | 用户态 | ✅ 已实现 |
| **GPON GTC 成帧 / PLOAM 状态机** | **内核态** | ❌ **缺失 —— 唯一的硬缺口** |
| XG-PON / XGS-PON | 内核态 | ✅（XGS-PON 未充分测试） |
| 10G-EPON / EPON | 内核态 | ✅ 10G/1G 已测，1G/1G 未实现 |

**结论：只要把内核侧 GPON 线路层补上，四大运营商的认证与业务配置基本不用再改。**

## 4. OMCC 版本

| 场景 | OMCC version |
|---|---|
| 电信 CTC 2.0/2.1 | `0xa0` |
| 电信 CTC 3.0 | `0xb0` |
| 联通 / 移动 | `0xb0` 多数可用，个别省要 `0xa0` |

本项目通过 `/etc/config/pon` 的 `omcc_version` 配置，无需改代码。

## 5. 常见上线失败排查顺序

1. 光功率：`ponctl status` 看 Rx/Tx 是否在 Class B+ 区间（收 -8 ~ -27 dBm，发 +1.5 ~ +5 dBm）
2. 波长/BOB 校准：GPON 必须用 `bob_0` 那套校准，用错会「有光不上线」
3. PLOAM：O2 → O3（收到 Upstream_Overhead）→ O4（Ranging）→ O5（Assign_ONU_ID + Configure_PortID）
4. OMCI：O5 之后 OMCC 建立，OLT 下发 MIB Upload；卡在这里多半是 ONT-G 的 Vendor-ID / Equipment-ID 不匹配
5. 认证：LOID/密码或 SN/密码填错，OLT 会直接踢回 O1 或反复掉线
