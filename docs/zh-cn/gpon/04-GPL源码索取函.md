# 04 · GPL 源码索取函（可直接发送）

> 依据：ITU-T G.984 实现的 `xpon.ko` 在 modinfo 中声明 `license=GPL`，
> 依 GPL-2.0 第 3 条，二进制分发者有义务提供完整对应源码。

---

## 0. 先看这张表：哪些模块是 GPL，哪些不是

从原厂固件 `XG2010G_fw_1.0.0.13` 提取的 12 个 PON 模块逐一读 `modinfo` 得到：

| 模块 | `license` 字段 | 是否可依法索要源码 |
|---|---|---|
| `xpon.ko` ← **GPON 核心** | `GPL` | ✅ **可以，这是重点** |
| `xpon_10g.ko` | `GPL` | ✅ |
| `xponmap.ko` | `GPL` | ✅ |
| `ponvlan.ko` | `GPL` | ✅ |
| `ponmacfilter.ko` | `GPL` | ✅ |
| `xpon_igmp.ko` | `GPL` | ✅ |
| `gpon_igmp.ko` | `GPL` | ✅ |
| `phy_10g.ko` | `GPL` | ✅ |
| `econet_bob.ko` | `GPL` | ✅ |
| `xpon_int.ko` | `Proprietary` | ❌ 厂商明确声明专有 |
| `gpon_flow.ko` | （无 license 字段） | ⚠️ 未声明，按内核惯例视为专有 |
| `en7572.ko` | （无 license 字段） | ⚠️ 同上 |

**结论：最关键的 `xpon.ko`（GPON GTC/PLOAM 全部实现，1.17 MB）是 GPL 的，法律上有明确依据。**
`xpon_int.ko`（18 KB，接口层）和小型辅助模块可能是专有，但它们不是协议实现的主体。

### 0.1 本机实测身份信息（已从 NAND 备份的 `dsd` 分区解出，可直接填表）

`dsd.bin`（mtd2，2 MiB）里是明文键值对：

```
admin_username=admin
admin_password=$5$6UcM0J5W0sESA4J9$ibnIed.c7JsXM4uCfEPv7zuHUe4FNBlsTNHqeiifvED
wan_mac=00:58:28:B8:BD:F2
lan_mac=00:58:28:B8:BD:F3
serial_number=XG2010G2414003882
clei_code=BVMN500GRA
fsan=AXON1050358C          ← GPON 序列号（Vendor ID = AXON）
manufacturer=Gemtek
```

| 字段 | 值 | 用途 |
|---|---|---|
| 型号 | AXON XG2010G (v0) | 索取函抬头 |
| 序列号 | `XG2010G2414003882` | **厂商核对设备归属的关键字段** |
| GPON SN / FSAN | `AXON1050358C` | 证明是 GPON 机型 |
| CLEI code | `BVMN500GRA` | 运营商级设备编码 |
| WAN MAC | `00:58:28:B8:BD:F2` | 辅助佐证 |
| ODM | Gemtek（正文科技） | 确定原始权利方 |

> 注：`admin_password` 是 SHA-256-crypt（`$5$`），试过 29 个常见口令未命中，说明不是弱口令，需另想办法。

### 0.2 已生成待发邮件

`outbox/` 目录下两份 `.eml`，设备信息已全部填好，双击即可在邮件客户端打开：

| 文件 | 收件人 | 语言 |
|---|---|---|
| `01-axon-networks-gpl-request.eml` | `sales@axon-networks.com` | 英文 |
| `02-gemtek-gpl-request.eml` | `sales@gemteks.com` | 中文 + 英文摘要 |

这两家都没有公开的 GPL 专用邮箱，只能走公开业务邮箱并要求内部转交（信中已写明这一要求）。
Gemtek 另可尝试官网表单：`gemteks.com/en/contact`（可选「Others」类别）。

---

## 1. 中文版（发国内代理 / 卖家 / 厂商技术支持）

> **主题：依据 GPL-2.0 索取 AXON XG2010G 固件中 GPL 模块的源代码**
>
> 您好：
>
> 我持有贵方渠道销售的 **AXON XG2010G**（Airoha AN7581 + EN7572 平台）设备，
> 设备固件版本为 **XG2010G_fw_1.0.0.13**（OpenWrt 21.02.1 / 内核 5.4.55）。
>
> 经检查，该固件包含以下内核模块，其模块元数据中明确声明 `license=GPL`：
>
> | 模块路径 | 模块名 | license |
> |---|---|---|
> | `/lib/modules/5.4.55/xpon.ko` | `xpon` | GPL |
> | `/lib/modules/5.4.55/xpon_10g.ko` | `xpon_10g` | GPL |
> | `/lib/modules/5.4.55/xponmap.ko` | `xponmap` | GPL |
> | `/lib/modules/5.4.55/ponvlan.ko` | `ponvlan` | GPL |
> | `/lib/modules/5.4.55/ponmacfilter.ko` | `ponmacfilter` | GPL |
> | `/lib/modules/5.4.55/xpon_igmp.ko` | `xpon_igmp` | GPL |
> | `/lib/modules/5.4.55/gpon_igmp.ko` | `gpon_igmp` | GPL |
> | `/lib/modules/5.4.55/phy_10g.ko` | `phy_10g` | GPL |
> | `/lib/modules/5.4.55/econet_bob.ko` | `econet_bob` | GPL |
>
> 上述模块的编译信息显示其源码位于 `.../xpon_1g/src/gpon/gpon_dev.c`
> 等路径，对应软件包 `feeds/airoha/package/airoha/drivers/xpon_1g`。
>
> 依据 **GNU General Public License version 2 第 3 条**，分发以 GPL 许可的目标代码时，
> 必须向接收者提供或承诺提供完整的对应源代码。请提供：
>
> 1. 上述 GPL 模块的完整对应源码（含构建所需的 Makefile / Kconfig / 补丁）
> 2. 获取源码的书面方式（下载地址、或介质寄送方式）
> 3. 若源码中包含第三方受限部分，请说明边界及可替代实现
>
> 我的设备序列号：__________（可在机身标签或 Web 管理页查看）
> 购买渠道与订单号：__________
>
> 请于收到本函后 **30 日内**回复。如贵方并非该模块的原始权利人，
> 请转交 Gemtek / AXON Networks 或 EcoNet / Airoha 的 GPL 合规负责人。
>
> 此致
>
> 姓名：__________
> 联系方式：__________
> 日期：__________

---

## 2. English version（发 AXON / Gemtek 法务或 GPL 合规邮箱）

> **Subject: GPL-2.0 source code request — AXON XG2010G firmware XG2010G_fw_1.0.0.13**
>
> To whom it may concern,
>
> I own an **AXON XG2010G** gateway (Airoha AN7581 + EN7572), running firmware
> **XG2010G_fw_1.0.0.13** (OpenWrt 21.02.1, Linux 5.4.55).
>
> The firmware ships the following kernel modules whose metadata explicitly
> declares `license=GPL`:
>
> `xpon.ko` (`xpon`), `xpon_10g.ko` (`xpon_10g`), `xponmap.ko` (`xponmap`),
> `ponvlan.ko` (`ponvlan`), `ponmacfilter.ko` (`ponmacfilter`),
> `xpon_igmp.ko` (`xpon_igmp`), `gpon_igmp.ko` (`gpon_igmp`),
> `phy_10g.ko` (`phy_10g`), `econet_bob.ko` (`econet_bob`)
> — all under `/lib/modules/5.4.55/`.
>
> Build paths embedded in these objects point to sources such as
> `.../xpon_1g/src/gpon/gpon_dev.c`, from the package
> `feeds/airoha/package/airoha/drivers/xpon_1g`.
>
> Pursuant to **GNU GPL version 2, Section 3**, distribution of GPL-licensed
> object code requires that the complete corresponding source code be made
> available to recipients. I therefore request:
>
> 1. The complete corresponding source for the GPL modules listed above,
>    including build files (Makefile / Kconfig) and any required patches.
> 2. A written statement of how this source is made available (URL or media).
> 3. If any third-party restricted components are included, a description of
>    the boundary and whether a free replacement exists.
>
> Device serial number: __________
> Purchase channel / order reference: __________
>
> Please respond within **30 days**. If you are not the original rights holder
> for these modules, kindly forward this request to the GPL compliance contact
> at Gemtek / AXON Networks or EcoNet / Airoha.
>
> Sincerely,
> __________

---

## 3. 发送渠道（建议三路并行）

| 渠道 | 说明 |
|---|---|
| 卖家 / 代理 | 最快有回音，但他们通常只能转交 |
| AXON Networks 官网支持 / 法务 | `axon-networks.com` 联系表单，注明 "GPL compliance" |
| Gemtek（正文泰） | 原始 ODM，`gemtek.com` 投资者/法务邮箱 |
| EcoNet / Airoha（联发科子公司） | SDK 原始方，若前两者推诿则找这里 |

## 4. 追踪表

| 日期 | 对象 | 方式 | 回复 | 结论 |
|---|---|---|---|---|
| 2026-11-15 | AXON / Gemtek | `.eml` 已生成，待发送 | — | 本机 Agent Mail 未开通，需手动发出或开通后由脚本代发 |

## 5. 拿到源码之后

见 `docs/03` 第 5 节 P1–P3。关键动作：

1. 确认源码版本与 `xpon.ko` 的 `srcversion` 一致
2. 剥离对 `econet_qdma` / `econet_ppe` / `tccicmd` 的依赖（这些是 EcoNet SDK 私有子系统）
3. 把 GTC/PLOAM 层接到上游 `airoha-pon-frontend-core.ko` 的 `AIROHA_PON_FRONTEND_MODE_RESERVED_GPON` 上
4. 内核 5.4 → 6.18 API 适配
