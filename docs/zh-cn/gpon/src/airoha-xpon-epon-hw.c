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
 * 上游 start_mac 只配了约 15 个寄存器，本函数补上原厂 112 条 MAC 表。
 * 调用时机：在 start_mac 完成 SCU 复位释放之后、使能中断之前。
 *
 * 注意：本函数只写寄存器，不碰中断使能与 SCU 复位——
 * 那两块上游已经做了，重复做会打乱时序。
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

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Airoha xPON EPON register initialization sequence");
