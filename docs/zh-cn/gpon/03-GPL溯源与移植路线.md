# 03 · GPL 溯源与 GPON 移植路线

---

## 1. 先纠正一个常见误解

「把原厂 GPON 驱动加进去」这句话在技术上不成立，原因不是能力问题，是**二进制不通用**：

```
原厂 xpon.ko   vermagic = 5.4.55 SMP mod_unload aarch64
本项目内核      vermagic = 6.18.60 SMP mod_unload aarch64
               → insmod 直接 invalid module format
```

即使强行绕过 vermagic 校验，它依赖的 `econet_qdma` / `econet_ppe` / `tccicmd` 等符号在 ImmortalWrt 的 `airoha-eth` / `airoha-ppe` 里根本不存在，链接阶段就会崩。

**所以只有两条路：拿到源码移植，或者自己重写。**

---

## 2. 推荐路径：依 GPL 索取源码（不是逆向）

`xpon.ko` 的 modinfo 里明确写着：

```
license = GPL
```

这意味着：

- 该模块是 GPL-2.0 代码，其**对应源码必须可获取**（GPL-2.0 §3）
- 权利方是 **Gemtek / AXON Networks**（固件定制方）或 **EcoNet / Airoha**（SDK 原始方）
- 固件里能看到源码路径：`.../xpon_1g/src/gpon/gpon_dev.c`，包名 `feeds/airoha/package/airoha/drivers/xpon_1g`

### 索取话术（可直接用）

> 主题：GPL source code request — AXON XG2010G firmware XG2010G_fw_1.0.0.13
>
> 本人持有 AXON XG2010G（AN7581）设备，其固件中包含 GPL 许可的内核模块
> `/lib/modules/5.4.55/xpon.ko`（modinfo: license=GPL，包名 kmod-xpon_1g，
> 源码路径 .../xpon_1g/src/gpon/gpon_dev.c）。
> 依据 GPL-2.0 第 3 条，请提供该模块及其配套模块
> （xpon_10g / xponmap / ponvlan / gpon_flow / gpon_igmp / ponmacfilter / en7572）
> 的完整对应源码，或获取方式。

三条渠道并行发：设备厂商技术支持、AXON 法务/GPL 合规邮箱、卖家（OEM 渠道）。

### 附带好处

拿到 `xpon_1g` 源码后，`docs/01` 里的寄存器操作、GTC 帧格式、PLOAM 时序全部变成可读代码，
移植从「逆向工程」降级为「内核 API 适配」，工作量差一个数量级。

---

## 3. 放弃逆向的理由

纯逆向 `xpon.ko`（1.17 MB，aarch64，已 strip）需要：

- 还原 AN7581 GPON MAC 的寄存器映射（数百个偏移）
- 还原 GTC 帧组装/解析、DBA 报文格式
- 还原 PLOAM 状态机时序与超时参数
- 每一步都无法验证，只能在真实 GPON 线路上试错

预估 **6 个月起**，且成功率不保证。仅作为「索要源码失败」时的兜底。

---

## 4. 许可与仓库策略（重要）

本项目继承 ImmortalWrt，许可证 **GPL-2.0-only**。

**不要把厂商二进制提交进本仓库**：

| 放进 git 的后果 | 说明 |
|---|---|
| 许可冲突 | GPL 仓库内分发闭源 `.ko` / `.so`，构成再分发，法律风险明确 |
| DMCA 风险 | 厂商可对仓库发起下架 |
| 无任何功能收益 | 5.4.55 的模块在 6.18 内核上跑不起来 |

正确做法：

- 分析结论、符号清单、寄存器笔记 → **以文档形式入库**（本仓库现状）
- 原始二进制 → 由使用者自行从自有设备的固件备份中提取，不入库
- 移植出的新驱动 → **必须开源**（GPL-2.0），放 `openwrt-pon-drivers` 侧

---

## 5. 分阶段实施计划

### P0 · 立项与取证（1 周）

- [x] 原厂固件取证：GPON 实现存在性、模块清单、双模切换方式（见 `docs/01`）
- [x] 四运营商差异梳理（见 `docs/02`）
- [ ] 发出 GPL 源码索取函
- [ ] 确认测试环境：一条可用的 GPON 线路 + 可回滚的 OLT 端口（**没有这条，后面全部做不了**）

### P1 · 源码适配（2–4 周，前提：拿到 xpon_1g 源码）

- 剥离 `gpon/` 子目录，去掉对 econet QDMA/PPE/tccicmd 的依赖
- 把 Linux 5.4 API 迁到 6.18：
  - `net_device` / `netif_napi_add` 签名变化
  - `timer_setup`、`proc_create` 系列
  - `dma_alloc_coherent` / `dma_map_single` 参数
  - `skb` 与 `napi_gro_receive` 路径
- 输出：一个能编译通过、能 `insmod` 的空壳 `airoha-xpon-gpon.ko`

### P2 · 接入现有 frontend 抽象（2–3 周）

上游 `airoha-pon-frontend-core.ko` 已经定义了模式枚举，只是 GPON 是 `RESERVED`：

```c
AIROHA_PON_FRONTEND_MODE_RESERVED_GPON   ← 要把它变成真模式
AIROHA_PON_FRONTEND_MODE_XGPON
AIROHA_PON_FRONTEND_MODE_XGSPON
```

工作项：

- 新增 `AIROHA_PON_FRONTEND_MODE_GPON`，接上 PCS 的 `AIROHA_PCS_PON_MODE_GPON`（SerDes 侧已支持 2.5G/1.25G 速率档）
- 新增 `airoha_xpon_gpon_*` 一套回调（对应现有 `_xgpon_*` / `_epon_*`）
- `ponctl mode set gpon` 打通（现在会报 `this build has no GPON 2.5G/1.25G or EPON 1G/1G line mode`）

### P3 · 线路联调（4–8 周，必须实机）

按 `docs/02` 第 5 节逐级验证：

1. 光功率与波长 → 2. O2/O3（Upstream_Overhead）→ 3. O4（EqD 测距）
4. O5（Assign_ONU_ID + Configure_PortID）→ 5. OMCI MIB Upload → 6. 业务 VLAN 通

每家运营商至少验证一种认证方式。

### P4 · 上游化（持续）

- 提交 PR 到 `naoki66/openwrt-pon-drivers`，把 GPON 模式从 `RESERVED` 变成 `✅`
- 补齐 `link_mode auto` 探测（GPON / XGS-PON 自适应）

---

## 6. 短期方案（不改内核也能用）

如果目标只是「让这台机器在国内 GPON 线路上跑起来」，**保留原厂固件**即可：

- 原厂固件 GPON 实现完整（`docs/01`）
- CLI 已暴露全套配置：`omcicfgCmd set loid / set sn / set operator_id_auth CTC / set omcc_version`
- 需要先解决 root shell（串口 `avec_console` → `/bin/login`）

代价是没有 ImmortalWrt 的可玩性；收益是**今天就能上线**。

两条路线不冲突：短期用原厂，长期等 P1–P3 完成后再切 ImmortalWrt。
