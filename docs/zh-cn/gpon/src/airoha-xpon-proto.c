// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha AN7581/AN7583 xPON —— 协议操作表实现
 *
 * 目标位置：openwrt-pon-drivers/airoha-xpon/src/airoha-xpon-proto.c
 *
 * 这一层只做一件事：把「按协议分派」从散落的 if/else 收敛成查表。
 * 具体协议实现在 airoha-xpon-gpon.c / airoha-xpon-xgpon.c /
 * airoha-xpon-epon.c 里。
 */

#include <linux/module.h>

#include "airoha-xpon-private.h"
#include "airoha-xpon-proto.h"

#include "airoha-xpon-gpon.h"
#include "airoha-xpon-xgpon.h"
#include "airoha-xpon-epon.h"

/* ------------------------------------------------------------------ */
/* 模式 → 操作表                                                       */
/* ------------------------------------------------------------------ */

const struct airoha_xpon_proto_ops *
airoha_xpon_proto_for_mode(enum airoha_xpon_mode mode)
{
	switch (mode) {
	case AIROHA_XPON_MODE_GPON:
		return &airoha_xpon_gpon_ops;
	case AIROHA_XPON_MODE_XGPON:
	case AIROHA_XPON_MODE_XGSPON:
	case AIROHA_XPON_MODE_EPON_10G_1G:
	case AIROHA_XPON_MODE_EPON_10G_10G:
		return &airoha_xpon_xgpon_ops;
	case AIROHA_XPON_MODE_EPON_1G_1G:
		return &airoha_xpon_epon_ops;
	default:
		return NULL;
	}
}
EXPORT_SYMBOL_GPL(airoha_xpon_proto_for_mode);

const struct airoha_xpon_proto_ops *
airoha_xpon_proto(struct airoha_xpon *xpon)
{
	if (!xpon || !READ_ONCE(xpon->active_mode_valid))
		return NULL;

	return airoha_xpon_proto_for_mode(READ_ONCE(xpon->active_mode));
}
EXPORT_SYMBOL_GPL(airoha_xpon_proto);

/* ------------------------------------------------------------------ */
/* XG-PON 操作表                                                       */
/*                                                                     */
/* XG-PON / XGS-PON / 10G-EPON 共用同一套 MAC 实现，                   */
/* 差异在 PCS 档位而非 MAC 逻辑，所以共用一个 ops。                     */
/* ------------------------------------------------------------------ */

static void xgpon_control_rx(struct airoha_xpon *xpon, struct sk_buff *skb,
			     const void *info)
{
	airoha_xpon_omci_receive(xpon->omci, skb, info);
}

static bool xgpon_control_match(struct airoha_xpon *xpon, const void *info)
{
	const struct airoha_pon_ctrl_rx_info *rx = info;
	u16 onu_id = READ_ONCE(xpon->onu_id);

	/* 默认 OMCC XGEM Port-ID 跟随 active ONU-ID */
	return xpon->omci && onu_id != AIROHA_XGPON_BROADCAST_ID &&
	       rx->id == onu_id;
}

static void xgpon_control_wake_tx(struct airoha_xpon *xpon)
{
	airoha_xpon_omci_wake_tx(xpon->omci);
}

static bool xgpon_los(struct airoha_xpon *xpon)
{
	return airoha_xpon_xgpon_los(xpon);
}

const struct airoha_xpon_proto_ops airoha_xpon_xgpon_ops = {
	.mode		= AIROHA_XPON_MODE_XGPON,
	.name		= "xgpon",

	.prepare	= airoha_xpon_xgpon_prepare,
	.start_rx	= airoha_xpon_xgpon_start_rx,
	.stop_rx	= airoha_xpon_xgpon_stop_rx,
	.mask_irqs	= airoha_xpon_xgpon_mask_irqs,

	.irq		= airoha_xpon_xgpon_irq,
	.phy_irq	= airoha_xpon_xgpon_phy_irq,
	/* XG-PON 的 fast IRQ 直接走线程，没有独立实现 */
	.irq_fast	= NULL,

	.los		= xgpon_los,
	.pcs_synced	= NULL,

	.advance	= airoha_xpon_xgpon_advance,

	.control_rx	= xgpon_control_rx,
	.control_match	= xgpon_control_match,
	.control_wake_tx = xgpon_control_wake_tx,

	.recover	= NULL,
};
EXPORT_SYMBOL_GPL(airoha_xpon_xgpon_ops);

/* ------------------------------------------------------------------ */
/* EPON 操作表                                                         */
/* ------------------------------------------------------------------ */

static void epon_control_rx(struct airoha_xpon *xpon, struct sk_buff *skb,
			    const void *info)
{
	airoha_xpon_oam_receive(xpon->oam, skb, info);
}

static bool epon_control_match(struct airoha_xpon *xpon, const void *info)
{
	/*
	 * QDMA 在描述符里标记 EPON OAM，
	 * 这里不需要按 ONU-ID 匹配，直接交给 OAM 层。
	 */
	return false;
}

static void epon_control_wake_tx(struct airoha_xpon *xpon)
{
	airoha_xpon_oam_wake_tx(xpon->oam);
}

const struct airoha_xpon_proto_ops airoha_xpon_epon_ops = {
	.mode		= AIROHA_XPON_MODE_EPON_1G_1G,
	.name		= "epon",

	.prepare	= NULL,		/* EPON 在 start_rx 里一并做 */
	.start_rx	= airoha_xpon_epon_start_rx,
	.stop_rx	= airoha_xpon_epon_stop_rx,
	.mask_irqs	= airoha_xpon_epon_mask_irqs,

	.irq_fast	= airoha_xpon_epon_irq_fast,
	.irq		= airoha_xpon_epon_irq,
	.phy_irq	= airoha_xpon_epon_phy_irq,

	.los		= airoha_xpon_epon_los,
	.pcs_synced	= airoha_xpon_epon_pcs_synced,

	.advance	= airoha_xpon_epon_advance,

	.control_rx	= epon_control_rx,
	.control_match	= epon_control_match,
	.control_wake_tx = epon_control_wake_tx,

	.debug_pcs	= airoha_xpon_epon_debug_pcs,
	.debug_mac	= airoha_xpon_epon_debug_mac,

	.recover	= NULL,
};
EXPORT_SYMBOL_GPL(airoha_xpon_epon_ops);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Airoha xPON protocol dispatch layer");
