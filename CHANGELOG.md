# 更新日志

本文件记录 Gemtek Brightspeed 固件仓库的重要功能、稳定性和维护变更。常规的
ImmortalWrt 上游合并不逐项展开，仅记录会影响本设备构建或运行行为的内容。

## 2026-11-17（v1.0.2）

### 修复登录页样式回退、双列列表断线

固件 `immortalwrt-xm-snapshot-airoha-an7581-gemtek_xg2010g-ubi-squashfs-sysupgrade.itb`
（90.3 MB），SHA256 `3cf5917ec9ee070e5a9ef635513616b6c58924c476abd0466fdbec6be3baa267`。

#### 登录页退回默认样式（v1.0.1 引入）

v1.0.1 在 argon 的 `footer_login.ut` 里用 `{% if (firmware_info) %}` 包裹版本链接，
`{% }` 不是合法的 ucode 模板结束符，模板渲染抛异常后 `dispatcher.uc` 会回退渲染
通用 `sysauth` 模板 —— 于是 Argon 的居中卡片登录页变成左上角带侧边栏的默认页面。

现改为**只在该 `{% %}` 代码块内计算变量，完全不改动 HTML 结构**：

- 逐段兜底 `distname` / `distversion` / `distrevision`，任一缺失不再拼出 `null`
- `disturl` 缺失时回退到本仓库地址
- 已用 host 侧 `ucode` 实际执行代码块验证，输出 `ImmortalWrt Xm XG2010G SNAPSHOT`

#### ONU 状态页「线路事件」断线

双列网格（`.pon-list--split`）按行填充，分组里记录数为奇数时最后一行右侧留空，
导致倒数第二行右半边没有下边框，看起来像少了两根线。

- `status.js`：`detailList()` 在 `split` 且记录数为奇数时补一对不可见的占位单元格
- `onu.css`：`.pon-pad` 只保留上边框，高度与内边距归零，不额外占一行

## 2026-11-17（v1.0.1）

### 版本号改为 Xm，修复登录页空版本号、光信号状态矛盾、TX 误判

固件 `immortalwrt-xm-snapshot-airoha-an7581-gemtek_xg2010g-ubi-squashfs-sysupgrade.itb`
（90.3 MB / 397 包），SHA256 `75d6c9e242633445de9428c46817d88d641b72637a9e0de54f2693df531e0be8`。

#### 版本号 naoki66 → Xm

- `2010.config`：`CONFIG_VERSION_DIST="ImmortalWrt Xm XG2010G"`
- `scripts/set-build-version.sh`：默认发行版名与 `detect_model_from_dist()` 的后缀解析
- `include/version.mk`：`VERSION_HOME_URL` 默认值改为本仓库地址

固件文件名、`/etc/os-release` 的 `NAME` / `ID` / `PRETTY_NAME`、
LuCI 页脚与「状态 → 概览」的固件版本随之变为 Xm。

#### 登录页显示 `null null-null`

`dispatcher.uc` 渲染登录页时传给模板的 `scope` 缺少 `version`（未认证登录与
2FA 两处），导致 `version.distname` / `distversion` / `distrevision` 全为 null。

- 新增 `patches/feeds/luci/modules/luci-base/ucode/100-pass-version-to-login-scope.patch`
- 新增 `patches/feeds/argon/ucode/template/themes/argon/100-fix-null-version-footer.patch`：
  模板逐段兜底，缺哪段省哪段，全缺时不渲染版本链接

#### 光信号状态矛盾（有光却报无光）

现象：Rx 光功率 -23.47 dBm 正常、「已检测到光信号：是」，但「线路状态」
显示「无光 / 等待光信号」。

根因：**GPON 驱动从不更新 `xpon->lifecycle`**（仅 EPON 会更新），GPON 模式
永远停在 `main.c` 初始化的 `AIROHA_XPON_WAIT_OPTICAL_SIGNAL`。

- `airoha-xpon-gpon.c`：新增 `airoha_xpon_gpon_sync_lifecycle()`，在
  `airoha_xpon_gpon_set_state()` 尾部把 O1–O7 映射到通用 lifecycle：
  O1 有光→`WAIT_LINE_SYNC`、无光→`WAIT_OPTICAL_SIGNAL`；O2→`WAIT_LINE_SYNC`；
  O3/O4/O7→`PROTOCOL_ACTIVATING`；O5→`OPERATIONAL`；O6→`WAIT_OPTICAL_SIGNAL`
- `airoha-xpon-main.c`：光信号采样后刷新 lifecycle，覆盖「光插拔但不触发
  状态切换」的场景
- `luci-app-pon-status` / `luci-app-onu`：前端以实测 `optical_signal` 为准，
  有光但未同步时显示「同步中」而非「无光」

补丁：`patches/feeds/pon_drivers/.../0001-add-gpon-epon-support.patch`（更新）、
`patches/feeds/onu_config/luci-app-onu/.../101-fix-lifecycle-optical-signal.patch`、
`patches/feeds/onu_config/luci-app-pon-status/.../100-fix-lifecycle-and-tx-state.patch`

#### TX 未发光被误判为故障

激光器在 O1/O2 未使能时读数为 -40 dBm，原判定 `>= -5 ? 预警 : 故障` 会把它
标成红色「差/故障」，与旁边正常的 Rx 读数矛盾。改为 `<-10` → 「未发光」，
RX `<-35` → 「无读数」。

#### 前端适配核对

- ONU 板块：GPON 与 EPON 双分支对称，覆盖 ONU 状态 O1–O7、GTC/XGTC 同步、
  ONU-ID、LLID、内核数据路径、服务就绪、上行突发、帧计数器、MPCP 状态、光模块
- 状态概览：6 种模式波长全映射（gpon / xgpon / xgspon / epon-1g /
  epon-10g-1g / epon-10g-10g），Rx/Tx 量程与温度、电压、偏置电流

#### 未集成：蒲公英异地组网

`OrayOS/OpenOray` 的 `pgyvpn` 为预编译二进制，实测依赖
`libssl.so.1.1` / `libcrypto.so.1.1`（`OPENSSL_1_1_0`），
而本固件 openssl 3.5.9 仅提供 `libssl.so.3`，二进制不兼容。
集成需让 pgyvpn 私有链接 openssl 1.1 且不干扰系统 openssl 3。

## 2026-11-17（v1.0.0）

### 首次完整编译出固件，修复 13 处构建阻断问题

固件 `immortalwrt-naoki66-snapshot-airoha-an7581-gemtek_xg2010g-ubi-squashfs-sysupgrade.itb`
（90.3 MB，397 个包）编译成功并通过上机实测。

#### GPON/EPON 驱动（`patches/feeds/pon_drivers/airoha-xpon`）

- `airoha-xpon-main.c`：3 个控制器回调使用 `ops` 却未声明，补齐
  `const struct airoha_xpon_proto_ops *ops;`（`rx_control`、`match_control`、
  `advance_rx_locked`）。
- `airoha-xpon-proto.c`：上游模式枚举无 `AIROHA_XPON_MODE_EPON_1G_1G`，改用
  `AIROHA_XPON_MODE_EPON_1G`；补 `airoha-xpon-omci.h`、`airoha-xpon-oam.h`，
  以及 `<linux/airoha-eth.h>`（`struct airoha_pon_ctrl_rx_info` 完整定义）；
  `airoha_xpon_xgpon_advance()` 返回 `void` 与 ops 的 `int` 签名不符，新增
  `xgpon_advance()` 包装返回 0。
- `airoha-xpon-gpon.c`：补 `<linux/regmap.h>`（`regmap_set_bits/clear_bits`）
  与 `<linux/iopoll.h>`（`readl_poll_timeout_atomic`）；`airoha_xpon_get_hir()`
  从未实现，改为本地 `airoha_xpon_gpon_hir()` 返回 0 走保守分支（DBA SMALL +
  执行 WAN 选择），寄存器窗口内确无版本寄存器；`udelay()` 两处触发
  `__bad_udelay` 链接错误——运行期变量改为常量循环助手
  `airoha_xpon_gpon_udelay()`，`udelay(0x8958)`（35 ms，超 arm64 单次 20 ms
  上限）改为 `msleep(35)`；`airoha_xpon_gpon_rogue_handle()` 只有声明无实现，
  补为提示日志并跳过（当前寄存器映射无流氓 ONU 检测窗口）。
- `airoha-xpon-stats.c`：补 `<linux/netdevice.h>`（`netdev_name`）。

#### 打包层

- `package/base-files/Makefile`：版本号原为
  `$(PKG_RELEASE)~$(lastword $(subst -, ,$(REVISION)))`，在无 `.git` 的环境下
  变成 `261115.64147~unknown`，apk 拒绝 `~` 分隔符（`version is invalid`）。
  改为纯 `$(PKG_RELEASE)`，两处（包元数据与 `base-files.version`）。
- `package/kernel/airoha-voice/Makefile`：`PKG_BUILD_DIR` 名为
  `airoha-voice-4222576d`，而 `vendor/airoha-voip-4222576d.tar.gz` 顶层目录是
  `airoha-voip-4222576d`，补丁因找不到文件全部失败。改为 `airoha-voip-4222576d`。
  同时 vendor 源码统一转 LF——原包为 CRLF，补丁为 LF，`patch` 报
  `different line endings` 导致 9 个 hunk 全失败。
- `package/network/services/asterisk-chan-en75xx/Makefile`：原用
  `git clone https://github.com/Sirherobrine23/airoha_voip.git`，GitHub API
  触发 403 限流后克隆不完整、哈希不符。改为
  `PKG_SOURCE_URL:=file://$(TOPDIR)/vendor` + 内置 tarball。

#### 构建环境（不在仓库内，需本机配置）

- WSL 默认 7 GB 内存，LLVM 编译 OOM，需 `.wslconfig` 设 `memory=12GB, swap=12GB`。
- WSL 默认把 Windows `PATH` 注入，其中含空格路径（`/mnt/d/Program Files/...`），
  导致 `find -execdir` 以「相对路径不安全」拒绝执行，Rust 安装步骤失败。需
  `/etc/wsl.conf` 设 `[interop] appendWindowsPath = false`。
- root 编译需 `export FORCE_UNSAFE_CONFIGURE=1`。
- `gn` 需要 `clang++` 且使用 `-std=c++23`，Ubuntu 22.04 自带 clang-15 只认
  `c++2b`，需装 clang-18 并软链到 `/usr/local/bin`。
- Rust 包需配 cargo 镜像（crates.io 国内不可达）；注意 OpenWrt 用
  `dl/cargo` 作为独立 `CARGO_HOME`，配置要同时写到那里。
- 编译中断会在 `openwrt/tmp/` 残留大量临时文件（`a.out`、`ar*`、`cc*.s`），
  使 `make prepare-tmpinfo` 从 23 秒退化到 9 分钟以上，重编前需清理。

#### 并发建议

本机 4 核 8 线程 / 16 GB，`-j10` 会让 WSL 周期性失去响应（`WSAETIMEDOUT`），
`-j6` 稳定。

## 2026-10-08

### 同步 ImmortalWrt 上游至 6.18.54

- 合并 `immortalwrt/immortalwrt` 的 306 个上游提交，内核补丁基线从 6.18.52
  推进到 6.18.54。
- 上游将 RTL8261C/D 的 LED 支持从 `hack-6.18/750-*` 迁出，改为新增
  `pending-6.18/721-02-net-phy-realtek-add-LED-support-for-RTL8261C-D.patch`
  并补齐亮度、极性和使能位处理，因此删除本地已无用的 `hack-6.18/750-*`。
- `pending-6.18/737-02` phylink PCS 补丁改用上游版本：本地此前的 v15 写法
  与上游 v3 写法功能完全等价，而上游版本已按 6.18.54 基线校订行号。
- `pending-6.18/743` RTL8261N USXGMII SerDes 补丁保留本地的 `sds_work`
  竞态修复语义（`READ_ONCE`/`WRITE_ONCE` 发布顺序，以及每次寄存器访问前和
  重新排队前的 disable 检查），叠加在上游的行号框架之上。

### 修正路由式 IPTV 组播丢包（引入 PonWrt #29）

- 引入 `pbs05/ponwrt#29` 的 `airoha: restore opt-in fraglist GRO` 修复。上游
  `hack-6.18/600` 把 `NETIF_F_GRO_FRAGLIST` 设为默认开启后，fraglist GRO 会把
  同流连续 UDP 报文合并成保留 DF 标志的超大 skb，该 skb 经 IPv4 组播路由转发
  时在 `ipmr_prepare_xmit()` 被静默丢弃，导致高码率 IPTV 流大量丢包（组播不可
  分片、无法收到 ICMP 错误，只有 `FragFails` 递增）。
- 新增 `target/linux/airoha/patches-6.18/980-revert-fraglist-gro-by-default.patch`，
  在 airoha target 内把 fraglist GRO 恢复为 opt-in 默认关闭，不改动通用补丁。
- 该修复只影响 `NETIF_F_GRO_FRAGLIST` 软件路径，与 XR1710G 的
  `NETIF_F_GRO_HW` 硬件 GRO（916-02 补丁）互不影响。

## 2026-10-01

### XG2010G FIT 动态扩容与产物检查

- 修正将当前 339-LEB `fit` 动态卷误当成固定分区上限的问题。U-Boot 和
  sysupgrade 都会保留 `fip`、`ubootenv`、`ubootenv2`、`factory`，删除
  `rootfs_data` 后按新镜像大小重建 `fit`；因此将构建上限调整为 U-Boot
  `0x90000000`/`0x94000000` 双缓冲允许的 64 MiB。
- GitHub Actions 和远程 world 构建现在逐一检查所选设备的 sysupgrade ITB，
  防止 `check-size` 删除超限镜像后仍把仅生成 manifest 的构建报告为成功。

## 2026-09-30

### XG2010G 固件生成与 ONU 包改名

- 将 XG2010G 配置选择从 `luci-app-pon` 更新为改名后的 `luci-app-onu`，并同步
  XR1710G 的隔离规则和文档引用。
- 禁用 XG2010G 的 initramfs 目标，避免并行构建时 initramfs 内核覆盖普通内核，
  随后又在 FIT 中追加 squashfs，造成固件尺寸重复并被 `IMAGE_SIZE` 检查删除。
- XG2010G 不再选择 `luci-theme-glass`，XR1710G 配置继续提供该主题。
- profile 隔离检查现在要求 `luci-app-onu`，并拒绝 XG2010G 配置重新启用
  initramfs。

## 2026-09-28

### XG2010G PON 用户态与 LuCI

- 将 `pon_userspace` feed 从 `pbs05/openwrt-pon-userspace` 切换到
  `naoki66/openwrt-pon-userspace`。
- 使用新版 `luci-app-pon` 的“网络 → ONU”菜单，按状态、硬件身份、认证配置、
  IPTV、语音和诊断组织页面。
- IPTV 页面、ACL、UCI 配置和应用服务已并入 `luci-app-pon`，从 XG2010G
  配置中移除已废弃的独立 `luci-app-iptv` 包选择。
- `pon_userspace` feed 和 `luci-app-pon` 仅由 XG2010G 的 `2010.config` 启用，
  XR1710G 的 `1710.config` 明确保持禁用。

## 2026-09-23

本条目覆盖 XR1710G 从 `20260916-e8702ccc61` 到 `b94f6f29b3` 的变更。同期
XG2010G 专属的 PON、ToD、BoB 和 PCM/语音功能不计入 XR1710G 运行面。

### 上游同步

- 合并 ImmortalWrt `master` 至 `b80b090e8e`，本地 merge commit 为
  `b94f6f29b3`；此前还通过 `669668ec3e` 和 `ba2d9bc4f3` 引入上游。
- Linux 6.18 从 `.44` 更新到 `.52`，包含 6.18.45 至 6.18.52 的稳定版
  修复，并同步刷新 Airoha、Realtek PHY 和通用内核补丁上下文。
- 引入 Airoha 上游改动，包括 Quantum Fiber Q1000K、RX ring 扩容、
  AN7583 PCIe Gen3 PHY，以及 phylink/PCS 相关修复。
- 引入 netifd、mac80211、procd、odhcpd、dnsmasq、dropbear 和 comgt
  等共享软件包更新；其中 mt76 TX worker CPU 绑定和流卸载修复对 XR1710G
  的无线与转发路径有直接影响。

### XR1710G 设备与网络

- 为 XR1710G 的 Airoha I2C 控制器设置 `airoha,airoha-i2c` 兼容项和
  400 kHz 总线频率，继续支持 NCT7802 硬件监控。
- 增加 AN7581 10G PCS link bring-up 修复，覆盖 JCPLL/TCLVAR、PCS
  restart 和按接口跟踪 PCS 状态。
- 同步 phylink PCS 到 v15 API：PCS provider 使用引用计数式
  acquire/release，PCS list 由 state mutex 保护，并补齐 PCS disable、
  link down 和 major configuration 强制重建路径。
- 修复 RTL8261BE/RTL8261N USXGMII SerDes reset work 与 PHY teardown
  的竞态：PHY 离开 running 状态时禁止并停用 delayed work，阻止旧 work
  在设备关闭后继续访问 SerDes 或重新排队。
- Airoha MAC 在共享 QDMA 停止后断开 PHY，避免重新打开接口时复用已被
  teardown 的 link state。
- 将 MT7996 板级默认值、无线缓冲区、PPE reload 和 packet steering
  移入 `airoha-an7581-mt7996-board`，由 XR1710G/W1700K 选择。
- 明确排除 PON firmware/manager、xPON、GPON IGMP、PON VLAN、ToD 和
  PCM/语音组件，避免 XG2010G 功能进入 XR1710G 镜像或内核配置。
- 对照 `immortalwrt_pon` 的 `675-01`、`675-02` 和 `675-09` 补充桥接
  conntrack 的 PPPoE、PPPoE-in-Q 和双层 VLAN（内层 802.1Q，外层
  802.1Q/802.1ad）跟踪；修复非零 network offset 下的 L3/L4 校验和计算。
- 在 XR1710G 的 XFRM/SOE flow offload 补丁之后检查 `nft_thoff()`：
  未解析 L4 偏移时跳过卸载，保留软件转发，避免双层 VLAN/PPPoE 流量
  被错误绑定到 PPE；现有 `meta l4proto { tcp, udp }` firewall4 规则不受影响。

### LuCI 与 Mesh

- `luci-app-airoha` 统一 NPU 与 FlowSense 页面，状态和端口拓扑按设备树
  生成，补充 CPU governor/max_freq 持久化、刷新时间、暗色主题和中英文翻译。
- `luci-app-mesh-conf` 增加 DAWN 客户端引导、802.11k/v/r 支持和 6 GHz
  频段补丁，并修复同步服务自愈、执行位和 SAE key 被空值覆盖的问题。
- 新栈 `airoha-ponctl`、`airoha-pond`、`luci-app-pon`、ToD PHC 和 EN7581
  PCM-SPI 语音功能仅进入 XG2010G 配置，不随 XR1710G 构建。

### 构建与版本

- 新增 `1710.config` 与 `2010.config` 双配置构建入口，GitHub Actions 可选择
  设备配置，release 名称和文件名会包含对应型号。
- 固件版本改为根据 `CONFIG_TARGET_PROFILE` 自动识别 XR1710G/XG2010G。
- 新增 Gemtek profile 隔离检查，在构建前检查配置，在构建后检查 kernel
  config 和 image manifest，防止 XR1710G 与 XG2010G 包集合互相污染。
- 远程构建脚本支持选择 1710 或 2010 配置，构建失败时会尝试带 `V=s`
  重新编译并保留详细日志。

### 验证

- `git diff --check` 通过。
- XR1710G 和 XG2010G 的 profile 隔离检查均通过。
- 本轮未执行完整固件构建。

关键提交：`4b4ed05f79`、`31eda9dc56`、`4ebc4c2c9b`、`759359070c`、
`b94f6f29b3`。

## 2026-09-14

### 上游同步

- 合并 ImmortalWrt `master` 至 `3e246256ce`。
- 对照 OpenWrt `main` `0d7bfcb7e31e` 复核 Airoha AN7581/AN7583 补丁集。

### Airoha 网络修复

- 移植 OpenWrt `002faeed3c91792a02abdb6f39b1a6b74052e992`：
  `net: airoha: grow the small RX rings`。
- 将强制送 CPU 的 RX ring 4 从 16 个 descriptor 扩大到 128 个，降低 PPPoE
  Discovery、LCP、IPCP、CHAP、DHCPv6 和 LLDP 等突发流量耗尽 RX ring 的风险。
- 将其他小型 RX ring 的默认 descriptor 数从 16 提高到 32，与厂商 SDK 默认值一致。
- 刷新 `310-10`、`916-02`、`920-12`、`920-13` 的补丁上下文，使其适配新的
  `RX_DSCP_NUM()` 定义；未改变这些补丁原有功能。

### 补丁清理

- 删除无消费者的 `303-01` 和 `303-02`。这两项只为未引入的 Airoha PHY
  软件校准系列准备导出符号、共享寄存器头文件和校准等待函数。
- 删除与上游 `221-01` 重复的 `0403` PM Domain Kconfig 修复，保留 `221-01`
  作为 `ARCH_AIROHA` 启用 `AIROHA_CPU_PM_DOMAIN` 的唯一实现。
- 保留 XR1710G 所需的 RTL826x SerDes、AN7581 USXGMII、PCIe 3.0 x2、NPU、
  PPE/flowtable、GPIO、MIB 统计和无 BL31 启动兼容补丁。
- 未引入 OpenWrt 的 `602-04`，因为该补丁仅提供 AN7583 PCIe PHY 驱动，
  XR1710G 的 AN7581 配置未启用该驱动。

### 验证

- 新增 RX ring 补丁与 OpenWrt 官方文件的 Git blob 均为
  `3a21a65022acaba07679f01663678afbccc9b640`。
- `git diff --check` 通过，Airoha 6.18 补丁目录未引入 BOM 或 CRLF。
- 按本次维护要求未执行固件或内核编译。

相关提交：`a6b69e04dc`。

## 2026-09-08

- 为 ucode 增加 `msleep`，并替换原有 `uloop.sleep` 调用，避免不必要的事件循环阻塞。

相关提交：`4974641d84`。

## 2026-09-07

- 新增 `scripts/set-build-version.sh`，在构建配置中写入日期、仓库提交和上游提交信息。
- 调整 Airoha NPU 与 FlowSense 页面：统一 VLAN 标签卸载、PPPoE 透传卸载文案，
  增加设备模式检测，并限制路由模式下不适用的桥接卸载操作。
- 移除 NPU 页面中的超频功能。
- 新增并完善 `luci-app-airoha-factory`，支持原厂序列号、MAC/BSSID 查看与修改，
  修复 MTD 分区识别、RPC 脚本权限和表单交互问题。
- 修复 MT7996 电源同步事件解析和 EEPROM 发射功率处理。
- 更新 RTL826x SerDes 与 Linux 6.18.44 相关补丁上下文。

对应版本标签：`20260907-ea01178178`。

## 2026-08-31

- 修复 XR1710G 的 AN7581 USXGMII 速率适配、RX 校准和可选 TX FIR 参数。
- 恢复并稳定 RTL826x 主机侧 SerDes 配置，改善 10G PHY 与 AN7581 PCS 的协商。
- 修复 Airoha MIB 统计丢失、AN7581 GPIO mux 和桥接本地流量的 PPE 分类。
- 完善 VLAN ingress、桥接 L2 fallback、DHCP 客户端标识和镜像内 `px5g` 支持。

对应版本标签：`20260831-131ef84fe9`。

## 2026-08-20

- 合并 YYH XR1710G Linux 6.18 集成，统一 AN7581/AN7583 内核补丁基线。
- 清理已进入 Linux 6.18.44 基线的回移补丁，并刷新仍需保留的 Airoha 补丁上下文。
- 保留 XR1710G 设备树、PCIe 3.0 x2、NPU/Wi-Fi 卸载、SOE/XFRM 和本地 LuCI 定制。
- 修复上游合并后自定义固件版本元数据被覆盖的问题。

对应版本标签：`20260820-a60889b870`。
