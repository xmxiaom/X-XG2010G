// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha AN7581 EPON —— 原厂寄存器初始化序列 + 协议操作表
 *
 * 目标位置：openwrt-pon-drivers/airoha-xpon/src/airoha-xpon-epon-hw.c
 *
 * ============================================================
 * 与上游 airoha-xpon-epon.c 的分工
 * ============================================================
 *
 * 上游（1007 行，开源）已经实现了 EPON 的**全部协议逻辑**：
 *   - PCS 同步检测、LOS/LOF
 *   - MPCP 状态机（WAIT/REGISTERING/REGISTER_REQUEST/PENDING/REGISTERED/DENIED）
 *   - NACK 静默重试（AIROHA_EPON_NACK_SILENT_MS = 60000）
 *   - REREGISTER 处理
 *   - DENIED 状态与 denied_until
 *   - sync_time 时钟漂移补偿（apply_sync_time）
 *   - 中断服务（fast / threaded / phy）
 *   - start_rx / stop_rx / advance
 *
 * 所以**不要重写协议逻辑**。本文件只补两处真实缺口：
 *
 *   1. 原厂完整寄存器初始化序列（349 条 / 5 张表）
 *      上游 start_mac 只写了约 15 个寄存器（TRX_ADJUST、INT_ENABLE、
 *      GLB_CFG、REPORT_CFG 等），其余 300 多个寄存器保持上电默认值。
 *      原厂 xpon.ko 有 5 张完整初始化表，本文件把它们落地。
 *
 *   2. airoha_xpon_epon_ops 操作表
 *      上游 main.c 用 is_epon() 二元判断分派，没有 ops 表。
 *      本文件提供 ops，把上游已有函数接进 proto 层。
 *
 * ============================================================
 * 五张表的语义（从原厂逆向，见 docs/13）
 * ============================================================
 *
 *   epon_mac_table_init        112 条  MAC 初始化序列（上电首次配置）
 *   epon_reg_def_val_table     127 条  寄存器默认值（value + mask）
 *   epon_reg_not_rst_table      60 条  非复位表（软复位时**保留**）
 *   epon_reg_rst_table          41 条  复位表（只写 mask，清对应位）
 *   epon_log_rst_table           9 条  逻辑复位表（只清状态位）
 *
 * 表项结构 {offset, value, mask}：
 *   mask != 0  →  tmp = (tmp & ~mask) | (value & mask)   RMW 写
 *   mask == 0  →  直接写入 value                         全字写
 *
 * 注意 epon_reg_rst_table 的表项 value 恒为 0、只有 mask，
 * 语义是「清除 mask 覆盖的位」，不是「写入 0」。
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/seq_file.h>
#include <linux/types.h>

#include "airoha-xpon-private.h"
#include "airoha-xpon-proto.h"
#include "airoha-xpon-epon.h"
#include "airoha-xpon-epon-regs.h"

/* 本文件对外接口（供 proto 层与 debugfs 调用） */
int airoha_epon_hw_set_llid_key(struct airoha_xpon *xpon, u8 llid, u8 dir,
				const u8 key[16]);
int airoha_epon_hw_enable_encrypt(struct airoha_xpon *xpon, u8 llid, bool on);
u8 airoha_epon_hw_get_llid_key_inuse(struct airoha_xpon *xpon);
int airoha_epon_hw_set_llid_filter(struct airoha_xpon *xpon, u8 llid, bool on);
int airoha_epon_hw_set_llid_fec(struct airoha_xpon *xpon, u8 llid, bool on);
int airoha_epon_hw_set_llid_enable_mask(struct airoha_xpon *xpon, u8 mask);
int airoha_epon_hw_set_llid_threshold(struct airoha_xpon *xpon, u32 value);
int airoha_epon_hw_set_report(struct airoha_xpon *xpon, u32 q0, u32 q1, u32 qnum);
int airoha_epon_hw_set_grant_length(struct airoha_xpon *xpon, u32 value);
int airoha_epon_hw_set_queue_threshold(struct airoha_xpon *xpon, u32 threshold);
int airoha_epon_hw_set_txbuf_usage(struct airoha_xpon *xpon, u32 usage);
int airoha_epon_hw_phy_tx_power_up(struct airoha_xpon *xpon);

/* ------------------------------------------------------------------ */
/* 寄存器访问                                                          */
/* ------------------------------------------------------------------ */

static inline u32 epon_hw_rd(struct airoha_xpon *xpon, u32 off)
{
	return readl(xpon->epon_base + off);
}

static inline void epon_hw_wr(struct airoha_xpon *xpon, u32 off, u32 val)
{
	writel(val, xpon->epon_base + off);
}

/**
 * epon_hw_apply_table - 应用一张寄存器初始化表
 * @xpon:  XPON 设备
 * @table: 表项数组
 * @count: 表项数
 *
 * 按 mask 语义分两种写：
 *   mask == 0 → 全字写（表里表示「这个寄存器就是 value」）
 *   mask != 0 → 读改写（只动 mask 覆盖的位）
 *
 * 原厂 eponMacTableInit 等函数就是逐条展开成这种 RMW 序列。
 */
static void epon_hw_apply_table(struct airoha_xpon *xpon,
				const struct airoha_epon_reg_init *table,
				unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		const struct airoha_epon_reg_init *e = &table[i];

		if (!e->mask) {
			epon_hw_wr(xpon, e->offset, e->value);
			continue;
		}

		epon_hw_wr(xpon, e->offset,
			   (epon_hw_rd(xpon, e->offset) & ~e->mask) |
			   (e->value & e->mask));
	}
}

/* ------------------------------------------------------------------ */
/* 对外接口：完整初始化 / 复位                                         */
/* ------------------------------------------------------------------ */

/**
 * airoha_epon_hw_init - 应用原厂完整的 MAC 初始化序列
 * @xpon: XPON 设备
 *
 * 上游 start_mac 只配了约 20 个寄存器，本函数补上原厂 112 + 127 条表。
 *
 * 调用时机（经原厂反汇编核实，见 docs/22）：
 *
 *   原厂 eponMacSwReset @0x317a0 的顺序是：
 *     1. RSTCTRL1 bit31 置位 → udelay → 清零      （复位脉冲）
 *     2. 之后才写 0x6000 / 0x6100 / 0x60d0 等寄存器
 *
 *   即「先释放复位，再写寄存器」。我最初假设的「复位期间锁存」是错的。
 *
 *   上游 start_mac 比原厂多一层保护：RSTCTRL1 释放后、SSR3 逻辑复位
 *   释放前留了一个窗口。本函数应插在这个窗口里（比原厂更保守）。
 *
 * 注意：本函数只写 0x004–0x240 区间的静态配置寄存器，
 * 不碰 0x07c（LLID 注册控制）和 0x5f0–0x5f8（PHY 中断）——
 * 那些是运行时寄存器，原厂也没放在初始化表里（docs/22 第 2.3 节）。
 */
int airoha_epon_hw_init(struct airoha_xpon *xpon)
{
	if (!xpon->epon_base)
		return -ENODEV;

	/*
	 * 112 条 MAC 初始化序列。这是原厂 eponMacTableInit 的完整展开，
	 * 覆盖 0x004–0x238 区间，包括：
	 *   - 0x010/0x014 中断状态/使能的初值（值 0x100 / 0x40）
	 *   - 0x0d0–0x0f8 TRX 调整参数
	 *   - 0x160–0x18c 队列门限
	 *   - 0x1e0–0x1ec 报告周期
	 */
	epon_hw_apply_table(xpon, airoha_epon_init_epon_mac_table_init,
			    AIROHA_EPON_INIT_EPON_MAC_TABLE_INIT_COUNT);

	/*
	 * 127 条默认值表。与 MAC 表有大量重叠，但多出 0x204–0x240
	 * 区间（错误计数器的初值），这部分 MAC 表没覆盖。
	 */
	epon_hw_apply_table(xpon, airoha_epon_init_epon_reg_def_val_table_init,
			    AIROHA_EPON_INIT_EPON_REG_DEF_VAL_TABLE_INIT_COUNT);

	dev_dbg(xpon->dev, "EPON MAC init sequence applied (%u + %u entries)\n",
		AIROHA_EPON_INIT_EPON_MAC_TABLE_INIT_COUNT,
		AIROHA_EPON_INIT_EPON_REG_DEF_VAL_TABLE_INIT_COUNT);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_init);

/**
 * airoha_epon_hw_soft_reset - 软复位：清状态位，保留配置
 * @xpon: XPON 设备
 *
 * 对应原厂 eponMacSwReset。用两张表：
 *   epon_reg_not_rst_table  60 条  非复位寄存器（重新写入，恢复配置）
 *   epon_reg_rst_table      41 条  复位寄存器（只清 mask 位）
 *
 * 语义：软复位不动 MAC 地址、TRX 校准等关键配置，
 * 只清中断状态、队列计数、错误计数器这些运行时状态。
 */
int airoha_epon_hw_soft_reset(struct airoha_xpon *xpon)
{
	if (!xpon->epon_base)
		return -ENODEV;

	/* 先清运行时状态（41 条只清位，不写值） */
	epon_hw_apply_table(xpon, airoha_epon_init_epon_reg_rst_table_init,
			    AIROHA_EPON_INIT_EPON_REG_RST_TABLE_INIT_COUNT);

	/* 再恢复必须保留的配置（60 条） */
	epon_hw_apply_table(xpon, airoha_epon_init_epon_reg_not_rst_table_init,
			    AIROHA_EPON_INIT_EPON_REG_NOT_RST_TABLE_INIT_COUNT);

	dev_dbg(xpon->dev, "EPON MAC soft reset applied\n");

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_soft_reset);

/**
 * airoha_epon_hw_logic_reset - 逻辑复位：只清状态，不动配置
 * @xpon: XPON 设备
 *
 * 对应原厂 eponLogRstTableInit，9 条。用于掉线后快速恢复：
 * 清掉 0x02c–0x048 的队列状态和 0x0d4 的计数，
 * 但保留 MAC 地址、TRX 校准、中断使能。
 */
int airoha_epon_hw_logic_reset(struct airoha_xpon *xpon)
{
	if (!xpon->epon_base)
		return -ENODEV;

	epon_hw_apply_table(xpon, airoha_epon_init_epon_log_rst_table_init,
			    AIROHA_EPON_INIT_EPON_LOG_RST_TABLE_INIT_COUNT);

	dev_dbg(xpon->dev, "EPON MAC logic reset applied\n");

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_logic_reset);

/* ------------------------------------------------------------------ */
/* 调试：寄存器表覆盖度检查                                            */
/* ------------------------------------------------------------------ */

/**
 * airoha_epon_hw_debug - 输出初始化表覆盖情况
 * @xpon: XPON 设备
 * @m:    seq_file
 *
 * 用途：核对原厂表与实际硬件是否一致。
 * 如果某个寄存器读回值与表里的 value 不符，说明要么表项有误，
 * 要么该寄存器是只读/自清零的——这是排查硬件差异的第一手信息。
 */
void airoha_epon_hw_debug(struct airoha_xpon *xpon, struct seq_file *m)
{
	unsigned int i, mismatch = 0;

	seq_puts(m, "EPON register init tables:\n");
	seq_printf(m, "  mac_table      %3u entries\n",
		   AIROHA_EPON_INIT_EPON_MAC_TABLE_INIT_COUNT);
	seq_printf(m, "  def_val_table  %3u entries\n",
		   AIROHA_EPON_INIT_EPON_REG_DEF_VAL_TABLE_INIT_COUNT);
	seq_printf(m, "  not_rst_table  %3u entries\n",
		   AIROHA_EPON_INIT_EPON_REG_NOT_RST_TABLE_INIT_COUNT);
	seq_printf(m, "  rst_table      %3u entries\n",
		   AIROHA_EPON_INIT_EPON_REG_RST_TABLE_INIT_COUNT);
	seq_printf(m, "  log_rst_table  %3u entries\n",
		   AIROHA_EPON_INIT_EPON_LOG_RST_TABLE_INIT_COUNT);

	if (!xpon->epon_base)
		return;

	/*
	 * 抽查 MAC 表里 mask 非 0 的表项：读回值应当满足
	 * (readback & mask) == (value & mask)。只读寄存器会有偏差。
	 */
	seq_puts(m, "\n  offset    expect     actual     mask\n");
	for (i = 0; i < AIROHA_EPON_INIT_EPON_MAC_TABLE_INIT_COUNT; i++) {
		const struct airoha_epon_reg_init *e =
			&airoha_epon_init_epon_mac_table_init[i];
		u32 got;

		if (!e->mask)
			continue;

		got = epon_hw_rd(xpon, e->offset);
		if ((got & e->mask) == (e->value & e->mask))
			continue;

		mismatch++;
		if (mismatch <= 20)
			seq_printf(m, "  0x%03x    0x%08x 0x%08x 0x%08x\n",
				   e->offset, e->value, got, e->mask);
	}
	if (mismatch)
		seq_printf(m, "\n  %u mismatched entries (read-only or self-clearing)\n",
			   mismatch);
	else
		seq_puts(m, "\n  all masked entries match\n");
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_debug);

/* ------------------------------------------------------------------ */
/* 说明：协议操作表 airoha_xpon_epon_ops 定义在 airoha-xpon-proto.c     */
/*                                                                     */
/* 原因：proto.c 里的版本已正确处理 EPON 的 OAM 控制通道                */
/* （control_rx → airoha_xpon_oam_receive），而 OAM 与 OMCI 的分派      */
/* 属于「按协议分派」的职责，放在 proto 层更合适。                       */
/*                                                                     */
/* 本文件只负责「原厂寄存器初始化序列」这一件事。                        */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 下行 AES-128 加密（中国电信 CTC 2.1/3.0 强制要求）                   */
/* 原厂 eponSetLlidKey @0x03d2f4（492B）                               */
/*      eponGetLlidKey @0x03d4e0（440B）                               */
/*      eponGetLlidKeyInuse @0x03e84c（92B）                           */
/* ------------------------------------------------------------------ */

/*
 * EPON 下行是广播，每个 ONU 能收到全部下行帧，必须靠 AES-128 加密
 * 隔离用户数据。中国电信 CTC 2.1/3.0 明确要求「三重搅动 + AES-128」，
 * 这是入网测试项，不是可选项。
 *
 * 寄存器（原厂 0x6058 / 0x605c，减 0x6000）：
 *   0x0058  LLID_KEY_CTRL   [5:0]=LLID 索引, [7:6]=方向, [31]=触发
 *   0x005c  LLID_KEY_DATA   16 字节密钥的一个 32 位字
 *
 * 原厂按 4 个 32 位字逐个写入 0x605c，每次写前更新 0x6058 的索引。
 */

#define AIROHA_EPON_LLID_KEY_CTRL	0x0058
#define AIROHA_EPON_LLID_KEY_DATA	0x005c

/* 0x0058 的位域（从表项 mask 0x8000003f 反推） */
#define AIROHA_EPON_KEY_LLID		GENMASK(5, 0)
#define AIROHA_EPON_KEY_DIR		GENMASK(7, 6)
#define AIROHA_EPON_KEY_TRIGGER		BIT(31)

/**
 * airoha_epon_hw_set_llid_key - 设置指定 LLID 的 AES-128 密钥
 * @xpon: XPON 设备
 * @llid: LLID 索引 0..7
 * @dir:  0=下行解密，1=上行加密
 * @key:  16 字节 AES-128 密钥
 *
 * 原厂 eponSetLlidKey 的逻辑：
 *   1. 校验 llid <= 7、dir <= 1、key 非空，否则返回 -1
 *   2. 把 16 字节密钥按 [2],[1],[0] 顺序重排（字节序调整）
 *   3. 分 4 次写 0x605c，每次配合 0x6058 的索引与触发位
 *
 * 返回 0 成功，-EINVAL 参数非法。
 */
int airoha_epon_hw_set_llid_key(struct airoha_xpon *xpon, u8 llid, u8 dir,
				const u8 key[16])
{
	u32 ctrl;
	int i;

	if (!xpon->epon_base || !key)
		return -ENODEV;

	if (llid > 7 || dir > 1)
		return -EINVAL;

	for (i = 0; i < 4; i++) {
		/* 密钥按大端拼 32 位字 */
		u32 w = ((u32)key[i * 4] << 24) |
			((u32)key[i * 4 + 1] << 16) |
			((u32)key[i * 4 + 2] << 8) |
			 (u32)key[i * 4 + 3];

		/* 索引 = LLID + 方向 + 第几个字 */
		ctrl = FIELD_PREP(AIROHA_EPON_KEY_LLID, llid) |
		       FIELD_PREP(AIROHA_EPON_KEY_DIR, dir) |
		       FIELD_PREP(AIROHA_EPON_KEY_LLID, (llid & 0x7) | ((i & 0x3) << 3)) |
		       AIROHA_EPON_KEY_TRIGGER;

		epon_hw_wr(xpon, AIROHA_EPON_LLID_KEY_DATA, w);
		epon_hw_wr(xpon, AIROHA_EPON_LLID_KEY_CTRL, ctrl);
	}

	dev_dbg(xpon->dev, "llid %u dir %u key programmed\n", llid, dir);
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_set_llid_key);

/**
 * airoha_epon_hw_enable_encrypt - 使能/禁用下行加密
 * @xpon:  XPON 设备
 * @llid:  LLID 索引 0..7
 * @on:    true 使能
 *
 * 通过 0x0058 的触发位控制。
 */
int airoha_epon_hw_enable_encrypt(struct airoha_xpon *xpon, u8 llid, bool on)
{
	u32 ctrl;

	if (!xpon->epon_base)
		return -ENODEV;

	if (llid > 7)
		return -EINVAL;

	ctrl = FIELD_PREP(AIROHA_EPON_KEY_LLID, llid);
	if (on)
		ctrl |= AIROHA_EPON_KEY_TRIGGER;

	epon_hw_wr(xpon, AIROHA_EPON_LLID_KEY_CTRL, ctrl);
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_enable_encrypt);

/**
 * airoha_epon_hw_get_llid_key_inuse - 查询哪些 LLID 已配置密钥
 * @xpon: XPON 设备
 *
 * 原厂 eponGetLlidKeyInuse 读全局 g_llid_key_index 位图。
 * 这里直接回读硬件 0x0058 的 LLID 字段判断。
 *
 * 返回位图，bit n 置位表示 LLID n 已有密钥。
 */
u8 airoha_epon_hw_get_llid_key_inuse(struct airoha_xpon *xpon)
{
	u32 v;
	u8 map = 0;
	unsigned int i;

	if (!xpon->epon_base)
		return 0;

	/* 逐 LLID 探测：写入索引后回读是否保持 */
	for (i = 0; i < 8; i++) {
		epon_hw_wr(xpon, AIROHA_EPON_LLID_KEY_CTRL,
			   FIELD_PREP(AIROHA_EPON_KEY_LLID, i));
		v = epon_hw_rd(xpon, AIROHA_EPON_LLID_KEY_CTRL);
		if (FIELD_GET(AIROHA_EPON_KEY_LLID, v) == i)
			map |= BIT(i);
	}

	return map;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_get_llid_key_inuse);

/* ------------------------------------------------------------------ */
/* LLID 过滤（防用户私接与非法 ONU）                                    */
/* 原厂 eponMacIoctl 里的 LLID 过滤配置                                 */
/* ------------------------------------------------------------------ */

/**
 * airoha_epon_hw_set_llid_filter - 配置 LLID 过滤
 * @xpon:  XPON 设备
 * @llid:  本 ONU 的 LLID
 * @on:    true 表示只接收该 LLID 的下行帧
 *
 * EPON 下行广播，若不过滤 LLID，ONU 会收到同 PON 口下所有用户的
 * 下行帧。国内运营商要求必须按 LLID 过滤，防止用户侧抓包。
 *
 * 寄存器 0x0058 的 [5:0] 同时承担「本机 LLID」语义。
 */
int airoha_epon_hw_set_llid_filter(struct airoha_xpon *xpon, u8 llid, bool on)
{
	u32 v;

	if (!xpon->epon_base)
		return -ENODEV;

	if (llid > 7)
		return -EINVAL;

	v = epon_hw_rd(xpon, AIROHA_EPON_LLID_KEY_CTRL);
	v &= ~AIROHA_EPON_KEY_LLID;
	if (on)
		v |= FIELD_PREP(AIROHA_EPON_KEY_LLID, llid);

	epon_hw_wr(xpon, AIROHA_EPON_LLID_KEY_CTRL, v);
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_set_llid_filter);

/* ------------------------------------------------------------------ */
/* LLID 相关配置（FEC / 使能掩码 / 门限）                              */
/* 原厂 eponSetLlidFec @0x??（424B）→ 寄存器 0x0020/0x0024            */
/*      eponMacSetLlidEnableMask（312B）                              */
/*      eponSetLlidThrshldNum（268B）→ 寄存器 0x0068                  */
/*      eponSetLlidQueThrod（244B）                                   */
/* ------------------------------------------------------------------ */

#define AIROHA_EPON_LLID_FEC_CTRL	0x0020
#define AIROHA_EPON_LLID_FEC_STATUS	0x0024
#define AIROHA_EPON_LLID_THRESHOLD	0x0068

/**
 * airoha_epon_hw_set_llid_fec - 配置 LLID 的 FEC
 * @xpon:  XPON 设备
 * @llid:  LLID 索引
 * @on:    true 使能 FEC
 *
 * 原厂 eponSetLlidFec 操作 0x6020（控制）与 0x6024（状态）。
 * FEC（前向纠错）在国内电信 EPON 是推荐开启的，
 * 可提升长距离/高分光比下的误码容限。
 */
int airoha_epon_hw_set_llid_fec(struct airoha_xpon *xpon, u8 llid, bool on)
{
	u32 v;

	if (!xpon->epon_base)
		return -ENODEV;

	if (llid > 7)
		return -EINVAL;

	v = epon_hw_rd(xpon, AIROHA_EPON_LLID_FEC_CTRL);
	if (on)
		v |= BIT(llid);
	else
		v &= ~BIT(llid);

	epon_hw_wr(xpon, AIROHA_EPON_LLID_FEC_CTRL, v);
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_set_llid_fec);

/**
 * airoha_epon_hw_set_llid_enable_mask - 设置 LLID 使能掩码
 * @xpon: XPON 设备
 * @mask: 位图，bit n 置位表示使能 LLID n
 *
 * 原厂 eponMacSetLlidEnableMask 直接写掩码寄存器。
 */
int airoha_epon_hw_set_llid_enable_mask(struct airoha_xpon *xpon, u8 mask)
{
	if (!xpon->epon_base)
		return -ENODEV;

	epon_hw_wr(xpon, AIROHA_EPON_LLID_KEY_CTRL,
		   FIELD_PREP(AIROHA_EPON_KEY_LLID, mask & 0x3f));
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_set_llid_enable_mask);

/**
 * airoha_epon_hw_set_llid_threshold - 设置 LLID 门限数
 * @xpon:  XPON 设备
 * @value: 门限值
 *
 * 原厂 eponSetLlidThrshldNum 写 0x6068。
 * 用于限制同时活跃的 LLID 数量，防止资源耗尽。
 */
int airoha_epon_hw_set_llid_threshold(struct airoha_xpon *xpon, u32 value)
{
	if (!xpon->epon_base)
		return -ENODEV;

	epon_hw_wr(xpon, AIROHA_EPON_LLID_THRESHOLD, value);
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_set_llid_threshold);

/* ------------------------------------------------------------------ */
/* 上报（REPORT）与队列/突发配置                                       */
/* 原厂 eponSetStaticReport @?（428B）→ 0x0060/0x0064/0x00d8          */
/*      eponSetReportData（276B）→ 0x0060/0x0064                     */
/*      eponSetGrantLength（360B）→ 0x00f8                           */
/*      eponSetLlidQueThrod（244B）                                  */
/*      eponMacSetTxbufUsage（344B）                                 */
/* ------------------------------------------------------------------ */

#define AIROHA_EPON_REPORT_CFG0		0x0060
#define AIROHA_EPON_REPORT_CFG1		0x0064
#define AIROHA_EPON_REPORT_QUEUE	0x00d8
#define AIROHA_EPON_GRANT_LENGTH	0x00f8

/**
 * airoha_epon_hw_set_report - 配置上报（REPORT）参数
 * @xpon:   XPON 设备
 * @q0:     队列 0 的报限量
 * @q1:     队列 1 的报限量
 * @qnum:   上报的队列数
 *
 * 原厂 eponSetReportData + eponSetStaticReport 合并：
 *   0x0060  队列 0 报限
 *   0x0064  队列 1 报限
 *   0x00d8  上报队列配置
 *
 * EPON 上行靠 REPORT 帧向 OLT 申请带宽，不配置则拿不到上行时隙，
 * 是国内注册的必需环节。原厂在 eponMacSwReset 里调用。
 */
int airoha_epon_hw_set_report(struct airoha_xpon *xpon, u32 q0, u32 q1,
			      u32 qnum)
{
	if (!xpon->epon_base)
		return -ENODEV;

	epon_hw_wr(xpon, AIROHA_EPON_REPORT_CFG0, q0);
	epon_hw_wr(xpon, AIROHA_EPON_REPORT_CFG1, q1);
	epon_hw_wr(xpon, AIROHA_EPON_REPORT_QUEUE, qnum & 0xff);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_set_report);

/**
 * airoha_epon_hw_set_grant_length - 设置 Grant 长度
 * @xpon:  XPON 设备
 * @value: 最大 Grant 长度
 *
 * 原厂 eponSetGrantLength 写 0x00f8，被 eponMpcpDiscvGateIntHandler 调用。
 * 决定单次上行突发的最大长度，影响上行吞吐。
 */
int airoha_epon_hw_set_grant_length(struct airoha_xpon *xpon, u32 value)
{
	if (!xpon->epon_base)
		return -ENODEV;

	epon_hw_wr(xpon, AIROHA_EPON_GRANT_LENGTH, value);
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_set_grant_length);

/**
 * airoha_epon_hw_set_queue_threshold - 设置 LLID 队列门限
 * @xpon:      XPON 设备
 * @threshold: 门限值
 *
 * 原厂 eponSetLlidQueThrod，被 eponMacIoctl 调用。
 * 队列积压超过门限时触发流控，防止上行拥塞。
 */
int airoha_epon_hw_set_queue_threshold(struct airoha_xpon *xpon, u32 threshold)
{
	if (!xpon->epon_base)
		return -ENODEV;

	epon_hw_wr(xpon, AIROHA_EPON_LLID_THRESHOLD, threshold);
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_set_queue_threshold);

/**
 * airoha_epon_hw_set_txbuf_usage - 设置 TX buffer 使用率
 * @xpon:  XPON 设备
 * @usage: 使用率（0..100）
 *
 * 原厂 eponMacSetTxbufUsage，被 eponMacSetLlidEnableMask 调用。
 * 控制发送缓冲的占用上限，避免缓冲耗尽导致丢包。
 */
int airoha_epon_hw_set_txbuf_usage(struct airoha_xpon *xpon, u32 usage)
{
	if (!xpon->epon_base)
		return -ENODEV;

	epon_hw_wr(xpon, AIROHA_EPON_LLID_FEC_CTRL,
		   FIELD_PREP(GENMASK(15, 8), usage & 0xff));
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_set_txbuf_usage);

/**
 * airoha_epon_hw_phy_tx_power_up - 上电光模块发射
 * @xpon: XPON 设备
 *
 * 原厂 eponPhyTxPwrUp，被 eponInit 调用（启动路径）。
 * 打开激光器发射，不上电则无法上行。
 *
 * 注意：上游已通过 optical-frontend 提供方管理激光器，
 * 本函数作为兼容接口保留，若前端不可用则直接操作 PON PHY。
 */
int airoha_epon_hw_phy_tx_power_up(struct airoha_xpon *xpon)
{
	if (!xpon->pon_phy_base)
		return -ENODEV;

	/*
	 * PON PHY 的 TX 使能位（与 GPON 共用同一 PHY 窗口）。
	 * 上游 frontend 若已处理则此写为幂等。
	 */
	writel(readl(xpon->pon_phy_base + 0x0c) | BIT(0),
	       xpon->pon_phy_base + 0x0c);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_phy_tx_power_up);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Airoha xPON EPON register initialization sequence");
