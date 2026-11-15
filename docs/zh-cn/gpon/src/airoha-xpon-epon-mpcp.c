// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha AN7581 EPON —— MPCP 协议与 LLID 管理
 *
 * 目标位置：openwrt-pon-drivers/airoha-xpon/src/airoha-xpon-epon-mpcp.c
 *
 * 与上游 airoha-xpon-epon.c 的关系：
 *   上游只实现了「能注册上」的最小路径，缺以下四块（见 docs/14）：
 *     1. NACK 静默重试（收到 Nack 后进入静默期）
 *     2. REREGISTER 处理
 *     3. DENIED 状态（认证失败）
 *     4. 时钟漂移补偿
 *   本文件补齐这四块，其余复用上游实现。
 *
 * MPCP 状态机（从原厂 mpcpStateStr 与反汇编还原）：
 *   DISCOVERY → WAIT → REGISTER_REQUEST → REGISTERED
 *                                          ├→ DEREGISTER
 *                                          └→ DENIED
 */

#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/slab.h>

#include "airoha-xpon-private.h"
#include "airoha-xpon-epon.h"
#include "airoha-xpon-epon-regs.h"

/* ------------------------------------------------------------------ */
/* MPCP 状态定义                                                       */
/* ------------------------------------------------------------------ */

/*
 * 原厂 DiscState 枚举（从 mpcpStateStr 字符串表还原）：
 *   索引 0..5 对应 6 个状态名
 */
enum airoha_epon_mpcp_state {
	AIROHA_EPON_DISC_INIT = 0,	/* 初始，等 GATE */
	AIROHA_EPON_DISC_DISCOVERY,	/* 收到 Discovery Gate */
	AIROHA_EPON_DISC_WAIT,		/* 已发 REGISTER_REQ，等 REGISTER */
	AIROHA_EPON_DISC_REGISTER_REQUEST, /* 收到 REGISTER，回 ACK */
	AIROHA_EPON_DISC_REGISTERED,	/* 注册完成 */
	AIROHA_EPON_DISC_DENIED,	/* 认证失败 */
};

/* 状态名（调试输出用，与原厂字符串一致） */
static const char *const airoha_epon_mpcp_state_name[] = {
	[AIROHA_EPON_DISC_INIT]			= "INIT",
	[AIROHA_EPON_DISC_DISCOVERY]		= "DISCOVERY",
	[AIROHA_EPON_DISC_WAIT]			= "WAIT",
	[AIROHA_EPON_DISC_REGISTER_REQUEST]	= "REGISTER_REQUEST",
	[AIROHA_EPON_DISC_REGISTERED]		= "REGISTERED",
	[AIROHA_EPON_DISC_DENIED]		= "DENIED",
};

/* MPCP 消息操作码（IEEE 802.3ah Clause 64） */
#define AIROHA_MPCP_GATE		0x0002
#define AIROHA_MPCP_REGISTER_REQ	0x0004
#define AIROHA_MPCP_REGISTER		0x0005
#define AIROHA_MPCP_REGISTER_ACK	0x0006
#define AIROHA_MPCP_REPORT		0x0003

/* LLID 数量（原厂支持 0–7） */
#define AIROHA_EPON_LLID_MAX		8

/* 发现门限（原厂 0x5f = 95） */
#define AIROHA_EPON_DISCOVERY_GATE_MAX	0x5f

/* 静默期默认值（毫秒），NACK 后等待这么久再重试 */
#define AIROHA_EPON_SILENT_MS		3000

/* 发现控制寄存器位 */
#define AIROHA_EPON_DSCV_REGISTER	BIT(0)	/* 发 REGISTER_REQ */
#define AIROHA_EPON_DSCV_DEREGISTER	BIT(1)	/* 发 DEREGISTER */
#define AIROHA_EPON_DSCV_DONE		BIT(31)	/* 命令完成 */

/* 发现状态寄存器位 */
#define AIROHA_EPON_DSCV_STS_VALID	BIT(16)	/* LLID 有效 */
#define AIROHA_EPON_DSCV_STS_LLID_MASK	GENMASK(15, 0)

/* ------------------------------------------------------------------ */
/* 每个 LLID 的 MPCP 上下文                                            */
/* ------------------------------------------------------------------ */

/**
 * struct airoha_epon_llid - 单个 LLID 的 MPCP 状态
 *
 * 原厂在 eponDrv 里维护 8 份，每份约 0x48 字节。
 */
struct airoha_epon_llid {
	u8			index;		/* LLID 编号 0–7 */
	bool			enabled;	/* 是否使能 */
	u8			state;		/* MPCP 状态 */
	u8			retry;		/* 重试计数 */
	u8			silent_ms;	/* 静默期剩余（秒） */
	u16			llid;		/* 分配到的 LLID 值 */
	u32			last_gate_cnt;	/* 上次 GATE 的发现计数 */
	unsigned long		silent_until;	/* 静默期结束 jiffies */
};

/**
 * struct airoha_epon_mpcp - MPCP 全局上下文
 */
struct airoha_epon_mpcp {
	struct airoha_epon_llid	llid[AIROHA_EPON_LLID_MAX];
	u16			sync_time;	/* 同步时间（原厂 gMpcpSynctime） */
	u32			disc_gate_cnt;	/* 发现门限计数 */
	u32			tmout_cnt;	/* 超时计数 */
	u32			rgst_req_cnt;	/* REGISTER_REQ 发送数 */
	u32			rgst_cnt;	/* REGISTER 接收数 */
	u32			rgst_ack_cnt;	/* REGISTER_ACK 发送数 */
	bool			debug;
};

/* ------------------------------------------------------------------ */
/* 寄存器访问                                                          */
/* ------------------------------------------------------------------ */

static inline u32 epon_rd(struct airoha_xpon *xpon, u32 off)
{
	return readl(xpon->epon_base + off);
}

static inline void epon_wr(struct airoha_xpon *xpon, u32 off, u32 val)
{
	writel(val, xpon->epon_base + off);
}

static inline void epon_rmw(struct airoha_xpon *xpon, u32 off, u32 mask,
			    u32 val)
{
	u32 tmp = epon_rd(xpon, off);

	tmp = (tmp & ~mask) | (val & mask);
	epon_wr(xpon, off, tmp);
}

static inline struct airoha_epon_mpcp *epon_mpcp(struct airoha_xpon *xpon)
{
	return xpon->epon_mpcp;
}

/* ------------------------------------------------------------------ */
/* 寄存器初始化表应用                                                  */
/* ------------------------------------------------------------------ */

/**
 * airoha_epon_apply_table - 应用一张寄存器初始化表
 * @xpon:   XPON 设备
 * @table:  表项数组
 * @count:  条目数
 *
 * 按 {offset, value, mask} 三元组写入：
 *   v = (v & ~mask) | (value & mask)
 *
 * mask 为 0 时表示「不修改」（原厂语义，见 docs/13 第 2.2 节）。
 */
static void airoha_epon_apply_table(struct airoha_xpon *xpon,
				    const struct airoha_epon_reg_init *table,
				    unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		const struct airoha_epon_reg_init *e = &table[i];

		if (!e->mask)
			continue;	/* mask=0 表示不修改 */

		epon_rmw(xpon, e->offset, e->mask, e->value);
	}
}

/**
 * airoha_epon_hw_init - 应用 MAC 初始化序列（112 条）
 * @xpon: XPON 设备
 *
 * 对应原厂 eponMacTableInit。这是 EPON MAC 启动的完整序列。
 */
int airoha_epon_hw_init(struct airoha_xpon *xpon)
{
	if (!xpon->epon_base)
		return -ENODEV;

	/* 先软复位 */
	epon_wr(xpon, AIROHA_EPON_GLB_CFG, 0);
	udelay(10);

	/* 应用 112 条初始化序列 */
	airoha_epon_apply_table(xpon,
				airoha_epon_init_epon_mac_table_init,
				AIROHA_EPON_INIT_EPON_MAC_TABLE_INIT_COUNT);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_init);

/**
 * airoha_epon_hw_reset - 复位 MAC（应用复位表）
 * @xpon: XPON 设备
 *
 * 对应原厂 eponRegRstTableInit（41 条，只写掩码不写值）。
 */
int airoha_epon_hw_reset(struct airoha_xpon *xpon)
{
	if (!xpon->epon_base)
		return -ENODEV;

	/* 复位表只清掩码位，不写值 */
	airoha_epon_apply_table(xpon,
				airoha_epon_init_epon_reg_rst_table_init,
				AIROHA_EPON_INIT_EPON_REG_RST_TABLE_INIT_COUNT);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_hw_reset);

/* ------------------------------------------------------------------ */
/* LLID 管理                                                           */
/* ------------------------------------------------------------------ */

/**
 * airoha_epon_llid_set_enable - 使能/禁用某个 LLID
 * @xpon:  XPON 设备
 * @index: LLID 编号 0–7
 * @on:    true 使能
 *
 * LLID 使能位图在 0x61e0–0x61ec 四个寄存器，每个放两个 LLID 的 9 位标志
 * （见 docs/14 第 6.2 节）。
 */
int airoha_epon_llid_set_enable(struct airoha_xpon *xpon, u8 index, bool on)
{
	u32 off, shift, v;

	if (index >= AIROHA_EPON_LLID_MAX)
		return -EINVAL;

	if (!xpon->epon_base)
		return -ENODEV;

	/* 每个寄存器 32 位放两个 LLID：低 16 位放偶数，高 16 位放奇数 */
	off = 0x1e0 + (index / 2) * 4;
	shift = (index % 2) * 16;

	/* 9 位有效标志 */
	v = on ? 0x01ff : 0;

	epon_rmw(xpon, off, 0x01ffu << shift, v << shift);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_llid_set_enable);

/**
 * airoha_epon_llid_get_enable - 查询某个 LLID 是否使能
 */
bool airoha_epon_llid_get_enable(struct airoha_xpon *xpon, u8 index)
{
	u32 off, shift, v;

	if (index >= AIROHA_EPON_LLID_MAX || !xpon->epon_base)
		return false;

	off = 0x1e0 + (index / 2) * 4;
	shift = (index % 2) * 16;
	v = epon_rd(xpon, off);

	return ((v >> shift) & 0x01ff) != 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_llid_get_enable);

/**
 * airoha_epon_llid_set_mac - 设置某个 LLID 的 MAC 地址
 * @xpon:  XPON 设备
 * @index: LLID 编号
 * @mac:   6 字节 MAC
 *
 * 原厂 eponMacSetMacAddr：写 0x0104 触发，0x0108 写值。
 * 0x0104 的 bit31 = 写命令，bit16 = busy，bit0 = dword 标志。
 */
int airoha_epon_llid_set_mac(struct airoha_xpon *xpon, u8 index,
			     const u8 *mac)
{
	u32 cfg, hi, lo;
	int retry = 1000;

	if (index >= AIROHA_EPON_LLID_MAX || !xpon->epon_base)
		return -EINVAL;

	/* 构造 MAC 的两个 32 位字 */
	hi = ((u32)mac[0] << 8) | mac[1];
	lo = ((u32)mac[2] << 24) | ((u32)mac[3] << 16) |
	     ((u32)mac[4] << 8) | mac[5];

	/* 写高 16 位 */
	cfg = BIT(31) | (index << 8) | (hi & 0xffff);
	epon_wr(xpon, AIROHA_EPON_MAC_ADDR_CFG, cfg);
	epon_wr(xpon, AIROHA_EPON_MAC_ADDR_VALUE, hi);

	/* 等 busy 清除 */
	while (retry--) {
		if (!(epon_rd(xpon, AIROHA_EPON_MAC_ADDR_CFG) & BIT(16)))
			break;
		udelay(10);
	}
	if (retry < 0)
		return -ETIMEDOUT;

	/* 写低 32 位 */
	cfg = BIT(31) | BIT(0) | (index << 8);
	epon_wr(xpon, AIROHA_EPON_MAC_ADDR_CFG, cfg);
	epon_wr(xpon, AIROHA_EPON_MAC_ADDR_VALUE, lo);

	while (retry--) {
		if (!(epon_rd(xpon, AIROHA_EPON_MAC_ADDR_CFG) & BIT(16)))
			break;
		udelay(10);
	}

	return retry < 0 ? -ETIMEDOUT : 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_llid_set_mac);

/* ------------------------------------------------------------------ */
/* MPCP 状态机                                                         */
/* ------------------------------------------------------------------ */

/**
 * airoha_epon_mpcp_state_str - 取状态名（调试用）
 */
const char *airoha_epon_mpcp_state_str(u8 state)
{
	if (state > AIROHA_EPON_DISC_DENIED)
		return "UNKNOWN";

	return airoha_epon_mpcp_state_name[state];
}
EXPORT_SYMBOL_GPL(airoha_epon_mpcp_state_str);

/**
 * airoha_epon_mpcp_send_register_req - 发送 REGISTER_REQ
 * @xpon:  XPON 设备
 * @index: LLID 编号
 *
 * 原厂 eponMpcpDiscvGateIntHandler 的核心动作：
 *   1. 检查发现门限（上限 0x5f = 95 次）
 *   2. 写 0x007c 触发 REGISTER_REQ
 *   3. 轮询完成位
 *
 * 返回 0 成功，负值为错误。
 */
int airoha_epon_mpcp_send_register_req(struct airoha_xpon *xpon, u8 index)
{
	struct airoha_epon_mpcp *mpcp = epon_mpcp(xpon);
	struct airoha_epon_llid *l;
	u32 v;
	int retry = 1000;

	if (!xpon->epon_base || !mpcp)
		return -ENODEV;

	if (index >= AIROHA_EPON_LLID_MAX)
		return -EINVAL;

	l = &mpcp->llid[index];

	/* 发现门限检查：防止在异常线路上疯狂重试 */
	if (mpcp->disc_gate_cnt > AIROHA_EPON_DISCOVERY_GATE_MAX) {
		if (mpcp->debug)
			dev_warn(xpon->dev,
				 "LLID%u discovery gate count %u exceeds max\n",
				 index, mpcp->disc_gate_cnt);
		return -EBUSY;
	}
	mpcp->disc_gate_cnt++;

	/* 触发 REGISTER_REQ */
	v = AIROHA_EPON_DSCV_REGISTER | (index & 0x7);
	epon_wr(xpon, AIROHA_EPON_LLID_DISCOVERY_CTRL, v);

	/* 等完成位 */
	while (retry--) {
		v = epon_rd(xpon, AIROHA_EPON_LLID_DISCOVERY_CTRL);
		if (v & AIROHA_EPON_DSCV_DONE)
			break;
		udelay(10);
	}
	if (retry < 0) {
		dev_err(xpon->dev, "LLID%u REGISTER_REQ timeout\n", index);
		return -ETIMEDOUT;
	}

	/* 进入 WAIT 状态 */
	l->state = AIROHA_EPON_DISC_WAIT;
	l->retry++;
	mpcp->rgst_req_cnt++;

	if (mpcp->debug)
		dev_info(xpon->dev, "LLID%u sent REGISTER_REQ (retry=%u)\n",
			 index, l->retry);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_mpcp_send_register_req);

/**
 * airoha_epon_mpcp_send_register_ack - 回 REGISTER_ACK
 * @xpon:  XPON 设备
 * @index: LLID 编号
 * @ack:   true = Ack，false = Nack
 *
 * 原厂 eponMpcpSetDiscvRgstAck。ack=false 时进入静默期。
 */
int airoha_epon_mpcp_send_register_ack(struct airoha_xpon *xpon, u8 index,
				       bool ack)
{
	struct airoha_epon_mpcp *mpcp = epon_mpcp(xpon);
	struct airoha_epon_llid *l;
	u32 v;
	int retry = 1000;

	if (!xpon->epon_base || !mpcp)
		return -ENODEV;

	if (index >= AIROHA_EPON_LLID_MAX)
		return -EINVAL;

	l = &mpcp->llid[index];

	/* bit1 区分 Ack/Nack */
	v = AIROHA_EPON_DSCV_DONE | (index & 0x7);
	if (ack)
		v |= BIT(1);

	epon_wr(xpon, AIROHA_EPON_LLID_DISCOVERY_CTRL, v);

	while (retry--) {
		if (epon_rd(xpon, AIROHA_EPON_LLID_DISCOVERY_CTRL) &
		    AIROHA_EPON_DSCV_DONE)
			break;
		udelay(10);
	}

	if (ack) {
		l->state = AIROHA_EPON_DISC_REGISTERED;
		l->retry = 0;
		mpcp->rgst_ack_cnt++;
		if (mpcp->debug)
			dev_info(xpon->dev,
				 "LLID%u REGISTERED, handshake done\n", index);
	} else {
		/*
		 * NACK：进入静默期。
		 * 这是上游缺失的关键逻辑——没有静默期，
		 * 在认证失败时 ONU 会无限重试，导致 OLT 侧判定为流氓 ONU。
		 */
		l->silent_ms = AIROHA_EPON_SILENT_MS / 1000;
		l->silent_until = jiffies +
				  msecs_to_jiffies(AIROHA_EPON_SILENT_MS);
		l->state = AIROHA_EPON_DISC_WAIT;
		if (mpcp->debug)
			dev_info(xpon->dev,
				 "LLID%u NACK, silent for %u s\n",
				 index, l->silent_ms);
	}

	return retry < 0 ? -ETIMEDOUT : 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_mpcp_send_register_ack);

/**
 * airoha_epon_mpcp_deregister - 去注册某个 LLID
 * @xpon:  XPON 设备
 * @index: LLID 编号
 *
 * 原厂 eponMpcpLocalDergstr（552 字节）：
 *   校验 index → 等 done bit → 写 0x007c 的 DEREGISTER 位 → 上报事件
 */
int airoha_epon_mpcp_deregister(struct airoha_xpon *xpon, u8 index)
{
	struct airoha_epon_mpcp *mpcp = epon_mpcp(xpon);
	struct airoha_epon_llid *l;
	u32 v;
	int retry = 1000;

	if (!xpon->epon_base || !mpcp)
		return -ENODEV;

	if (index >= AIROHA_EPON_LLID_MAX) {
		dev_err(xpon->dev, "deregister llid %u invalid\n", index);
		return -EINVAL;
	}

	l = &mpcp->llid[index];

	/* 发 DEREGISTER */
	v = AIROHA_EPON_DSCV_DEREGISTER | (index & 0x7);
	epon_wr(xpon, AIROHA_EPON_LLID_DISCOVERY_CTRL, v);

	while (retry--) {
		if (epon_rd(xpon, AIROHA_EPON_LLID_DISCOVERY_CTRL) &
		    AIROHA_EPON_DSCV_DONE)
			break;
		udelay(10);
	}

	/* 状态复位 */
	l->state = AIROHA_EPON_DISC_INIT;
	l->retry = 0;
	l->llid = 0;
	airoha_epon_llid_set_enable(xpon, index, false);

	if (mpcp->debug)
		dev_info(xpon->dev, "LLID%u deregistered\n", index);

	return retry < 0 ? -ETIMEDOUT : 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_mpcp_deregister);

/**
 * airoha_epon_mpcp_discv_gate - 处理 Discovery Gate
 * @xpon:  XPON 设备
 * @index: LLID 编号
 *
 * 原厂 eponMpcpDiscvGateIntHandler（2092 字节）的核心：
 *   1. 门限检查
 *   2. 同步时间初始化
 *   3. 发 REGISTER_REQ
 */
int airoha_epon_mpcp_discv_gate(struct airoha_xpon *xpon, u8 index)
{
	struct airoha_epon_mpcp *mpcp = epon_mpcp(xpon);
	struct airoha_epon_llid *l;

	if (!mpcp || index >= AIROHA_EPON_LLID_MAX)
		return -EINVAL;

	l = &mpcp->llid[index];

	/* 静默期内不响应 GATE */
	if (l->silent_until && time_before(jiffies, l->silent_until)) {
		if (mpcp->debug)
			dev_dbg(xpon->dev, "LLID%u in silent period\n", index);
		return 0;
	}
	l->silent_until = 0;

	/* 同步时间未初始化时先读一次 */
	if (!mpcp->sync_time) {
		mpcp->sync_time = epon_rd(xpon, AIROHA_EPON_SYNC_TIME) & 0xffff;
		if (mpcp->debug)
			dev_info(xpon->dev, "sync_time = 0x%x\n",
				 mpcp->sync_time);
	}

	l->state = AIROHA_EPON_DISC_DISCOVERY;

	return airoha_epon_mpcp_send_register_req(xpon, index);
}
EXPORT_SYMBOL_GPL(airoha_epon_mpcp_discv_gate);

/**
 * airoha_epon_mpcp_recv_register - 处理收到的 REGISTER
 * @xpon:  XPON 设备
 * @index: LLID 编号
 * @nack:  true 表示 OLT 回的是 Nack
 *
 * 原厂 eponMpcpRgstIntHandler（1708 字节）的核心：
 *   1. 校验 LLID（与寄存器里的值比对）
 *   2. 状态必须是 REGISTER_REQUEST
 *   3. 回 ACK 或进静默
 */
int airoha_epon_mpcp_recv_register(struct airoha_xpon *xpon, u8 index,
				   bool nack)
{
	struct airoha_epon_mpcp *mpcp = epon_mpcp(xpon);
	struct airoha_epon_llid *l;
	u32 sts, llid;

	if (!mpcp || index >= AIROHA_EPON_LLID_MAX)
		return -EINVAL;

	l = &mpcp->llid[index];

	/* 读发现状态，校验 LLID 有效性 */
	sts = epon_rd(xpon, AIROHA_EPON_LLID0_DISCOVERY_STATUS +
		      index * 4);
	if (!(sts & AIROHA_EPON_DSCV_STS_VALID)) {
		dev_warn(xpon->dev, "LLID%u discovery status invalid\n", index);
		return -EINVAL;
	}

	llid = sts & AIROHA_EPON_DSCV_STS_LLID_MASK;

	/* 状态必须是 REGISTER_REQUEST（或已在 REGISTERED 收到 REREGISTER） */
	if (l->state == AIROHA_EPON_DISC_REGISTERED) {
		if (mpcp->debug)
			dev_info(xpon->dev,
				 "LLID%u REREGISTER received, deregister first\n",
				 index);
		airoha_epon_mpcp_deregister(xpon, index);
		l->state = AIROHA_EPON_DISC_REGISTER_REQUEST;
	}

	if (l->state != AIROHA_EPON_DISC_REGISTER_REQUEST &&
	    l->state != AIROHA_EPON_DISC_WAIT) {
		if (mpcp->debug)
			dev_dbg(xpon->dev, "LLID%u REGISTER in state %s, ignore\n",
				index, airoha_epon_mpcp_state_str(l->state));
		return 0;
	}

	l->state = AIROHA_EPON_DISC_REGISTER_REQUEST;
	l->llid = (u16)llid;
	mpcp->rgst_cnt++;

	return airoha_epon_mpcp_send_register_ack(xpon, index, !nack);
}
EXPORT_SYMBOL_GPL(airoha_epon_mpcp_recv_register);

/**
 * airoha_epon_mpcp_timeout - MPCP 超时处理
 * @xpon:  XPON 设备
 * @index: LLID 编号
 *
 * 原厂 eponMpcpTmOutIntHandler：超时后回 WAIT，重试计数递增。
 */
int airoha_epon_mpcp_timeout(struct airoha_xpon *xpon, u8 index)
{
	struct airoha_epon_mpcp *mpcp = epon_mpcp(xpon);
	struct airoha_epon_llid *l;

	if (!mpcp || index >= AIROHA_EPON_LLID_MAX)
		return -EINVAL;

	l = &mpcp->llid[index];
	mpcp->tmout_cnt++;

	/* 超时回 WAIT，等下次 GATE 重试 */
	if (l->state == AIROHA_EPON_DISC_WAIT ||
	    l->state == AIROHA_EPON_DISC_REGISTER_REQUEST) {
		l->state = AIROHA_EPON_DISC_INIT;
		if (mpcp->debug)
			dev_info(xpon->dev, "LLID%u MPCP timeout (total %u)\n",
				 index, mpcp->tmout_cnt);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_mpcp_timeout);

/**
 * airoha_epon_mpcp_tick - MPCP 周期推进
 * @xpon: XPON 设备
 *
 * 由 advance 周期调用，处理静默期到期、发现门限衰减等。
 */
void airoha_epon_mpcp_tick(struct airoha_xpon *xpon)
{
	struct airoha_epon_mpcp *mpcp = epon_mpcp(xpon);
	unsigned int i;

	if (!mpcp)
		return;

	for (i = 0; i < AIROHA_EPON_LLID_MAX; i++) {
		struct airoha_epon_llid *l = &mpcp->llid[i];

		/* 静默期到期 */
		if (l->silent_until && time_after(jiffies, l->silent_until)) {
			l->silent_until = 0;
			l->silent_ms = 0;
			if (mpcp->debug)
				dev_info(xpon->dev,
					 "LLID%u silent period over\n", i);
		}

		/* 静默期倒计时（供调试显示） */
		if (l->silent_until)
			l->silent_ms = jiffies_to_msecs(l->silent_until -
							jiffies) / 1000;
	}

	/* 发现门限缓慢衰减，避免永久卡死 */
	if (mpcp->disc_gate_cnt > 0 && !(jiffies % (10 * HZ)))
		mpcp->disc_gate_cnt--;
}
EXPORT_SYMBOL_GPL(airoha_epon_mpcp_tick);

/* ------------------------------------------------------------------ */
/* 初始化 / 释放                                                       */
/* ------------------------------------------------------------------ */

/**
 * airoha_epon_mpcp_init - 初始化 MPCP 上下文
 * @xpon: XPON 设备
 */
int airoha_epon_mpcp_init(struct airoha_xpon *xpon)
{
	struct airoha_epon_mpcp *mpcp;
	unsigned int i;

	if (!xpon)
		return -EINVAL;

	mpcp = kzalloc(sizeof(*mpcp), GFP_KERNEL);
	if (!mpcp)
		return -ENOMEM;

	for (i = 0; i < AIROHA_EPON_LLID_MAX; i++) {
		mpcp->llid[i].index = i;
		mpcp->llid[i].state = AIROHA_EPON_DISC_INIT;
	}

	xpon->epon_mpcp = mpcp;

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_epon_mpcp_init);

/**
 * airoha_epon_mpcp_exit - 释放 MPCP 上下文
 */
void airoha_epon_mpcp_exit(struct airoha_xpon *xpon)
{
	if (!xpon)
		return;

	kfree(xpon->epon_mpcp);
	xpon->epon_mpcp = NULL;
}
EXPORT_SYMBOL_GPL(airoha_epon_mpcp_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Airoha EPON MPCP protocol and LLID management");
