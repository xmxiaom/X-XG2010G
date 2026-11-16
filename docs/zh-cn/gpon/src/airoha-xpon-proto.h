/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Airoha AN7581/AN7583 xPON —— 协议操作表（core 抽象层）
 *
 * 背景：上游 airoha-xpon-main.c 里有 17 处
 *     if (airoha_xpon_mode_is_epon(mode))
 *             return airoha_xpon_epon_xxx(xpon);
 *     return airoha_xpon_xgpon_xxx(xpon);
 * 的二元判断。加入 GPON 后变成三元，这种写法会失控。
 *
 * 本层把「按协议分派」收敛成一次查表：
 *     const struct airoha_xpon_proto_ops *ops = airoha_xpon_proto(xpon);
 *     return ops->start_rx(xpon);
 *
 * 三个协议各自实现一份 ops，main.c 不再关心具体是哪个协议。
 */

#ifndef _AIROHA_XPON_PROTO_H_
#define _AIROHA_XPON_PROTO_H_

#include <linux/interrupt.h>
#include <linux/types.h>

struct airoha_xpon;
struct seq_file;

/**
 * struct airoha_xpon_proto_ops - 单个 PON 协议的操作集
 *
 * 每个函数都对应上游 main.c 里的一处分派点。
 * 未实现的槽位可以为 NULL，调用方需检查。
 */
struct airoha_pon_ctrl_rx_info;

struct airoha_xpon_proto_ops {
	/* 协议标识 */
	enum airoha_xpon_mode mode;
	const char *name;

	/*
	 * 生命周期
	 */
	int  (*prepare)(struct airoha_xpon *xpon);
	int  (*start_rx)(struct airoha_xpon *xpon);
	void (*stop_rx)(struct airoha_xpon *xpon);
	void (*mask_irqs)(struct airoha_xpon *xpon);

	/*
	 * 中断
	 */
	irqreturn_t (*irq_fast)(struct airoha_xpon *xpon);
	irqreturn_t (*irq)(struct airoha_xpon *xpon);
	irqreturn_t (*phy_irq)(struct airoha_xpon *xpon);

	/*
	 * 链路状态
	 */
	bool (*los)(struct airoha_xpon *xpon);
	bool (*pcs_synced)(struct airoha_xpon *xpon);

	/*
	 * 状态机推进（由 link_work 周期调用）
	 * delay 返回下次调用的间隔（毫秒），0 表示用默认值
	 */
	int  (*advance)(struct airoha_xpon *xpon, u32 phy_events,
			unsigned long *delay);

	/*
	 * 控制通道（OMCI / OAM）
	 * OMCI 走 omci 对象，OAM 走 oam 对象，由协议自己决定
	 */
	void (*control_rx)(struct airoha_xpon *xpon, struct sk_buff *skb,
			   const struct airoha_pon_ctrl_rx_info *info);
	bool (*control_match)(struct airoha_xpon *xpon,
			      const struct airoha_pon_ctrl_rx_info *info);
	void (*control_wake_tx)(struct airoha_xpon *xpon);

	/*
	 * 调试
	 */
	void (*debug_pcs)(struct airoha_xpon *xpon, struct seq_file *m);
	void (*debug_mac)(struct airoha_xpon *xpon, struct seq_file *m);

	/*
	 * 掉线恢复（GPON 有，EPON 可选）
	 */
	int  (*recover)(struct airoha_xpon *xpon);
};

/* 三个协议的操作表 */
extern const struct airoha_xpon_proto_ops airoha_xpon_gpon_ops;
extern const struct airoha_xpon_proto_ops airoha_xpon_xgpon_ops;
extern const struct airoha_xpon_proto_ops airoha_xpon_epon_ops;

/**
 * airoha_xpon_proto - 按当前激活模式取操作表
 * @xpon: XPON 设备
 *
 * 返回对应协议的操作表；模式非法或未激活时返回 NULL。
 * 调用方必须检查返回值。
 */
const struct airoha_xpon_proto_ops *
airoha_xpon_proto(struct airoha_xpon *xpon);

/**
 * airoha_xpon_proto_for_mode - 按模式取操作表
 * @mode: 模式枚举
 */
const struct airoha_xpon_proto_ops *
airoha_xpon_proto_for_mode(enum airoha_xpon_mode mode);

/**
 * airoha_xpon_proto_is_itu - 判断是否为 ITU-T 协议（GPON / XG-PON / XGS-PON）
 * @mode: 模式枚举
 *
 * ITU 协议用 OMCI 控制通道，IEEE 协议（EPON）用 OAM。
 * 这个判断替代上游散落的 is_epon() 取反用法。
 */
static inline bool airoha_xpon_proto_is_itu(enum airoha_xpon_mode mode)
{
	return mode == AIROHA_XPON_MODE_GPON ||
	       mode == AIROHA_XPON_MODE_XGPON ||
	       mode == AIROHA_XPON_MODE_XGSPON;
}

#endif /* _AIROHA_XPON_PROTO_H_ */
