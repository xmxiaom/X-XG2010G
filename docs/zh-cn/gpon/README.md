# XMXG2010G

> 基于 [naoki66/ImmortalWrt-for-Gemtek-brightspeed](https://github.com/naoki66/ImmortalWrt-for-Gemtek-brightspeed) 的 ImmortalWrt 固件项目
> 目标：让 Gemtek / AXON **XG2010G**（Airoha AN7581 + EN7572）在**国内四大运营商 GPON 线路**上可用

---

## 现状一句话

固件的**用户态已经具备国内运营商能力**（CTC LOID ME、VLAN、IPTV、语音），
唯一缺口在**内核侧 GPON 的 GTC / PLOAM MAC 实现** —— 上游 `openwrt-pon-drivers` 里 `GPON` 是 `RESERVED`，只实现了 XG-PON / XGS-PON / 10G-EPON。

## 目录

- [`docs/01-原厂GPON实现分析.md`](docs/01-原厂GPON实现分析.md) — 从原厂 `xpon.ko` 挖出的 GPON 实现证据链
- [`docs/02-四运营商GPON认证差异.md`](docs/02-四运营商GPON认证差异.md) — 电信 / 联通 / 移动 / 广电 的认证与 OMCI 差异，以及本项目对应能力
- [`docs/03-GPL溯源与移植路线.md`](docs/03-GPL溯源与移植路线.md) → **推荐路径**：原厂驱动是 GPL，依法可索要源码，比逆向划算得多
- [`docs/04-GPL源码索取函.md`](docs/04-GPL源码索取函.md) — 中英双版索取函 + 模块许可证核对表
- [`docs/05-驱动接入点清单.md`](docs/05-驱动接入点清单.md) — **精确到文件与行号**的 GPON 接入方案（含寄存器窗口对照）
- [`docs/06-逆向成果-GPON寄存器表.md`](docs/06-逆向成果-GPON寄存器表.md) — **从原厂 `xpon.ko` 还原出 110 个寄存器 + 466 个函数**
- [`src/airoha-xpon-gpon-regs.h`](src/airoha-xpon-gpon-regs.h) — 逆向产出的寄存器定义，可直接用于移植
- [`tools/re_xpon.py`](tools/re_xpon.py) — 逆向工具（capstone + pyelftools，可重复执行）
- [`outbox/`](outbox) — 已填好设备信息的待发邮件（`.eml`，双击即可在邮件客户端打开）
- [`scripts/setup-and-push.sh`](scripts/setup-and-push.sh) — 拿到凭据后一键：克隆上游 → 改名 → 建仓 → 推送

## 核心结论

| 层 | 状态 |
|---|---|
| 硬件（AN7581 PCS / 1G MAC / EN7572） | ✅ 支持 GPON 2.5G/1.25G |
| 前端驱动 | ✅ 已有 `gpon-a2` 启动配置 |
| 模式框架 | ✅ 已就绪，只差 `mac_supported = true` 一个字段 |
| **GPON 寄存器映射** | ✅ **已从原厂 `xpon.ko` 逆向还原 110 个寄存器** |
| **GTC / PLOAM 协议实现** | ❌ **唯一实质缺口，需新增约 900–1000 行** |
| 用户态 OMCI（CTC LOID / VLAN / IPTV） | ✅ 已具备 |

**不是"缺驱动"，是"缺一个文件"。** 寄存器表已经有了，剩下的是时序与状态机。具体见 `docs/05`、`docs/06`。

## 为什么不直接"把原厂 GPON 驱动塞进来"

原厂 `xpon.ko` 是 **Linux 5.4.55 / aarch64** 的闭源二进制，本项目内核是 **6.18.60**：

| | 原厂 | 本项目 |
|---|---|---|
| vermagic | `5.4.55 SMP mod_unload aarch64` | `6.18.60 SMP mod_unload aarch64` |
| 依赖符号 | econet QDMA / PPE / tccicmd | Airoha `airoha-eth` / `airoha-ppe` |
| 加载结果 | — | `invalid module format`，必然失败 |

**放进去只能当参考，不能让 GPON 跑起来。** 真正可用的路径见 `docs/03`。

## 许可证

GPL-2.0-only（继承 ImmortalWrt）。
`docs/` 下的分析文档为原创，同样以 GPL-2.0-only 发布。
**本仓库不包含任何厂商闭源二进制**（`.ko` / `.so` / 光模块固件），原因见 `docs/03` 第 4 节。
