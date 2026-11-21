<img src="https://avatars.githubusercontent.com/u/53193414?s=200&v=4" alt="logo" width="200" height="200" align="right">

# XMXG2010G

**ImmortalWrt for Gemtek / AXON XG2010G** · Airoha AN7581 + EN7572

[![Build](https://img.shields.io/github/actions/workflow/status/xmxiaom/XMXG2010G/build-firmware.yml?branch=master&label=Build)](https://github.com/xmxiaom/XMXG2010G/actions/workflows/build-firmware.yml)
[![Upstream](https://img.shields.io/badge/upstream-immortalwrt-blue)](https://github.com/immortalwrt/immortalwrt)
[![Kernel](https://img.shields.io/badge/kernel-6.18-red)](https://www.kernel.org/)
[![SoC](https://img.shields.io/badge/SoC-Airoha%20AN7581-orange)]()
[![License](https://img.shields.io/badge/license-GPL--2.0-green)](https://spdx.org/licenses/GPL-2.0-only.html)

> 本仓库只维护 **XG2010G** 一个机型。
> 由 [naoki66/ImmortalWrt-for-Gemtek-brightspeed](https://github.com/naoki66/ImmortalWrt-for-Gemtek-brightspeed) 派生后**已独立**，不再跟随后者的多机型路线，也不再与上游同步设备集合。
>
> 同时承载**国内四大运营商 GPON 适配**的研究与移植工作（见文末）。

**文档基线**：`master`，内核 `6.18.60`（[`target/linux/generic/kernel-6.18`](target/linux/generic/kernel-6.18)）。

---

## 支持设备

| 项目 | XG2010G |
|---|---|
| 定位 | XG(S)-PON 网关 |
| SoC | Airoha AN7581 / EN7581 |
| 内存 | 1 GiB DDR4（自适应 2 GiB 改版） |
| 闪存 | 512 MiB SPI-NAND（W25N04KV） |
| 以太网 | 2×10G RTL8261N + 1×2.5G EN8811H + 1G 交换端口 |
| 光接入 | EN7572 xPON 前端 |
| 无线 | 无 |
| 设备树 | `an7581-gemtek-xg2010g-ubi.dts` |
| 种子配置 | [`2010.config`](2010.config) |
| profile | `gemtek_xg2010g-ubi` |
| 引导 | [naoki66/uboot-an758x](https://github.com/naoki66/uboot-an758x)（`xg2010g` / `xg2010g-2g`） |

> **已移除的机型**：XR1710G、Nokia XG-040G-MD / XG-040G-TF / Valyrian、Gemtek W1700K、
> Quantum Q1000K、AN7581 EVB。它们的 `*.config` 种子文件、设备树、镜像定义与 Nokia U-Boot 补丁
> 均已从当前分支删除；历史提交中仍可追溯。

## 快速开始

**1. 下载并校验镜像**

在 Releases 下载 `*-gemtek_xg2010g-ubi-squashfs-sysupgrade.itb` 与同批次的
`sha256sums-2010.config`，然后：

```sh
sha256sum -c sha256sums-2010.config
```

**2. 刷写**

- 已有系统升级：LuCI「系统 → 备份/升级 → 刷写固件」选择镜像。
- **首次从原厂固件刷入**：必须先通过串口确认启动状态，并对原厂 NAND 关键分区做只读备份与校验。

**3. 首次登录后立即修改凭据**

默认地址 `http://192.168.1.1` 或 `http://immortalwrt.lan`，用户名 `root`、无密码。
先设管理密码，再按需配置网络。

> 出厂凭据是公开信息。接入公网前请检查防火墙与 SSH 设置。

## 构建

```sh
git clone https://github.com/xmxiaom/XMXG2010G.git
cd XMXG2010G
./scripts/feeds update -a
./scripts/feeds install -a
bash scripts/fix-stale-golang-host.sh
cp 2010.config .config
make defconfig
bash scripts/set-build-version.sh .config
make -j$(nproc) world 2>&1 | tee build.log
bash scripts/summarize-build-errors.sh build.log
```

构建环境：GNU/Linux（Debian 11+），AMD64，≥4 GB RAM，≥25 GB 可用磁盘。

> `make defconfig` 与 `make menuconfig` 只会改写 `.config`。
> 若执行过这两条命令，编译前需重跑 `bash scripts/set-build-version.sh .config`
> 恢复 `CONFIG_VERSION_*`、`CONFIG_TARGET_HOSTNAME` 等构建信息。

产物命名：

```
immortalwrt-<name>-YYYYMMDD-<repo-hash>-airoha-an7581-gemtek_xg2010g-ubi-squashfs-sysupgrade.itb
```

## 刷写与升级注意事项

> ⚠️ **首次刷写**：必须通过串口确认当前启动状态，并完成原厂 NAND / 关键分区的只读备份与校验。
> 只允许升级匹配的 `ubi` 分区；**必须保留 `bootloader`、`uenv`、`dsd` 分区**。
> 引导使用 [naoki66/uboot-an758x](https://github.com/naoki66/uboot-an758x)。

> ⚠️ **跨版本升级**：LuCI 的「保留配置」不会保留额外安装的软件包。
> 升级前备份配置并用 `apk list --installed` 记录已装包，升级后重装；
> 不要恢复旧固件的 `kmod-*` 内核模块。

## GPON 适配（国内运营商）

面向**中国电信 / 联通 / 移动 / 广电 GPON 线路**的分析与移植：

| 内容 | 位置 |
|---|---|
| 原厂 GPON 实现分析（`xpon.ko` 逆向证据链） | [`docs/zh-cn/gpon/01-原厂GPON实现分析.md`](docs/zh-cn/gpon/01-原厂GPON实现分析.md) |
| 四大运营商认证 / VLAN / OMCC 差异 | [`docs/zh-cn/gpon/02-四运营商GPON认证差异.md`](docs/zh-cn/gpon/02-四运营商GPON认证差异.md) |
| 移植路线与 GPL 溯源 | [`docs/zh-cn/gpon/03-GPL溯源与移植路线.md`](docs/zh-cn/gpon/03-GPL溯源与移植路线.md) |
| GPL 源码索取函（中英双版） | [`docs/zh-cn/gpon/04-GPL源码索取函.md`](docs/zh-cn/gpon/04-GPL源码索取函.md) |
| 驱动接入点清单（精确到行号） | [`docs/zh-cn/gpon/05-驱动接入点清单.md`](docs/zh-cn/gpon/05-驱动接入点清单.md) |
| **GPON MAC 寄存器表（110 个，逆向还原）** | [`docs/zh-cn/gpon/06-逆向成果-GPON寄存器表.md`](docs/zh-cn/gpon/06-逆向成果-GPON寄存器表.md) |
| 状态机逆向进度 | [`docs/zh-cn/gpon/07-状态机逆向进度.md`](docs/zh-cn/gpon/07-状态机逆向进度.md) |
| 驱动骨架与寄存器头文件 | [`docs/zh-cn/gpon/src/`](docs/zh-cn/gpon/src) |
| 逆向工具（capstone + pyelftools） | [`tools/re_xpon.py`](tools/re_xpon.py) · [`tools/re_ehframe.py`](tools/re_ehframe.py) |

> 当前状态：寄存器层已完整还原并通过交叉验证；函数边界已由 `.eh_frame` 的 FDE 恢复（1634 个函数）。
> 用户态 OMCI 已支持 CTC LOID ME，四大运营商的认证配置无需额外开发。

### 内核实现现状（v1.1.0）

GPON 内核侧已经跑通"注册 → 业务面 → OMCI 管理"的完整软件链路，尚未在真实 OLT 上验证。

| 层次 | 实现 | 说明 |
|---|---|---|
| **GTC** | 硬件 | 帧同步、BWmap 解析、突发组帧（保护位/前导/定界符）、超帧计数全部由 AN7581 MAC 完成，驱动只写参数、收中断、累加 MIB，与原厂 `xpon.ko` 分工一致 |
| **PLOAM 下行** | 12 条 | `0x01/03/04/05/06/08/09/0a/0c/0d/0e/0f`；`0x02`、`0x07` 与原厂一致留空 |
| **PLOAM 上行** | 5 类 | Serial_Number(0x01)、Password(0x02)、Key_Report(0x05)、Key_Switching_Time(0x09)、Dying_Gasp |
| **注册链路** | 完整 | SN 每 200 ms 周期上报（O2/O3）→ Assign_ONU_ID（VSSN 比对）→ EqD 测距 → O5 |
| **加密** | 完整 | AES-128 影子密钥（反序 4 段写入）+ 超帧定时软切换 |
| **FEC** | 完整 | `xgpon+0x868` bit[1:0]，GPON 默认"仅上行"（原厂 `gpon_dev_init` 同值） |
| **OMCI 通道** | 完整 | OMCC GEM 默认 1，O5 时置 ready；QDMA 按描述符 bit30 识别 OMCI 帧 |
| **数据面** | 硬件直通 | GEM 由硬件解出走 PPE，驱动只维护 GEM 解密表与 Alloc-ID |

**已知未覆盖**：`0x0c PEE` / `0x0e BER_Interval` 只记录不回应；`0x0b`（如果有）与 BER 主动上报未实现。
GPON 全部验证均在主机侧模拟完成（3 轮 × 438 断言），真机行为待上机确认。

### 诊断接口

| 接口 | 内容 |
|---|---|
| `/sys/kernel/debug/airoha-xpon-pon0/line` | FEC 模式、省电（PEE/PST/BER）、OMCI 通道（omcc_gem / ready / rx / tx / dropped） |
| `/sys/kernel/debug/airoha-xpon-pon0/mac` | GPON 状态机、ONU-ID、EqD、OMCC GEM |
| `/sys/class/net/<pon>/gpon_omcc_gem` | 读写 OMCC GEM Port ID（默认 1，现场可调，无需重编） |
| `/sys/class/net/<pon>/gpon_sn_passwd` | PLOAM Password（10 字符） |
| `ponctl status --json` | 以上字段的 JSON 形式（`line` 组） |
| LuCI「状态 → 概览 → PON 光模块」 | "链路增强"卡片：FEC 模式 / 省电 / OMCI 状态 |

## 致谢

- [immortalwrt/immortalwrt](https://github.com/immortalwrt/immortalwrt) · [immortalwrt/luci](https://github.com/immortalwrt/luci) · [immortalwrt/packages](https://github.com/immortalwrt/packages)
- [naoki66/ImmortalWrt-for-Gemtek-brightspeed](https://github.com/naoki66/ImmortalWrt-for-Gemtek-brightspeed) — 本仓库的派生来源
- [naoki66/uboot-an758x](https://github.com/naoki66/uboot-an758x) — XG2010G 的 U-Boot / UBI 引导实现
- [naoki66/openwrt-pon-drivers](https://github.com/naoki66/openwrt-pon-drivers) — PON 内核驱动（GPON 目前为 `RESERVED`）
- [naoki66/OpenWrt_ONU_CONFIG](https://github.com/naoki66/OpenWrt_ONU_CONFIG) — ONU 用户态 OMCI / LuCI 栈
- [naoki66/luci-app-airoha](https://github.com/naoki66/luci-app-airoha) — Airoha 设备专属 LuCI 应用

## 许可证

[GPL-2.0-only](https://spdx.org/licenses/GPL-2.0-only.html)（继承 ImmortalWrt）。
