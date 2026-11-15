/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Airoha AN7581 GPON 线路层 —— 对外接口
 *
 * 目标位置：openwrt-pon-drivers/airoha-xpon/src/airoha-xpon-gpon.h
 *
 * 与 XG-PON 的差异（见 docs/05、docs/08）：
 *   - 独立寄存器窗口 gpon_base = 0x1fb64000（0x3e8 字节）
 *   - PLOAM 报文 12 字节（3 个 32 位字），XG-PON 是 44/52 字节
 *   - 21 项下行分派表（G.984.3 消息 ID 1–24）
 *   - 状态机 O1–O7
 */

#ifndef _AIROHA_XPON_GPON_H_
#define _AIROHA_XPON_GPON_H_

#include <linux/interrupt.h>
#include <linux/types.h>

struct airoha_xpon;
struct seq_file;

/* ---- 生命周期 ---- */
int  airoha_xpon_gpon_prepare(struct airoha_xpon *xpon);
int  airoha_xpon_gpon_start_rx(struct airoha_xpon *xpon);
void airoha_xpon_gpon_stop_rx(struct airoha_xpon *xpon);
void airoha_xpon_gpon_mask_irqs(struct airoha_xpon *xpon);

/* ---- 中断 ---- */
irqreturn_t airoha_xpon_gpon_isr(struct airoha_xpon *xpon);

/* ---- 链路状态 ---- */
bool airoha_xpon_gpon_los(struct airoha_xpon *xpon);

/* ---- 状态机 ---- */
int  airoha_xpon_gpon_advance(struct airoha_xpon *xpon, u32 phy_events,
			      unsigned long *delay);

/* ---- PLOAM ---- */
int  airoha_xpon_gpon_send_ploamu(struct airoha_xpon *xpon, const u8 *msg,
				  u32 count);
int  airoha_xpon_gpon_get_ploamd(struct airoha_xpon *xpon, u8 *out);
void airoha_xpon_gpon_recv_ploamd(struct airoha_xpon *xpon);
int  airoha_xpon_gpon_ploam_init(struct airoha_xpon *xpon);
int  airoha_xpon_gpon_dispatch_ploamd(struct airoha_xpon *xpon, const u8 *msg);
bool airoha_xpon_gpon_ploamd_dedup(struct airoha_xpon *xpon, const u8 *msg);

/* ---- 身份 ---- */
int  airoha_xpon_gpon_program_identity(struct airoha_xpon *xpon,
				       const u8 *vendor4, const u8 *vssn8);

/* ---- 状态机控制 ---- */
void airoha_xpon_gpon_set_state(struct airoha_xpon *xpon, u32 new_state);
void airoha_xpon_gpon_apply_cfg(struct airoha_xpon *xpon, u32 cmd, u32 param);

/* ---- 硬件命令 ---- */
int  airoha_xpon_gpon_cmd(struct airoha_xpon *xpon, u32 arg, u32 cmd);
void airoha_xpon_gpon_disable(struct airoha_xpon *xpon, bool keep_rx);
void airoha_xpon_gpon_detect_los_lof(struct airoha_xpon *xpon, u32 arg);
void airoha_xpon_gpon_connection_up(struct airoha_xpon *xpon);
bool airoha_xpon_gpon_backoff_ok(struct airoha_xpon *xpon);

/* ---- GEM / T-CONT ---- */
int  airoha_xpon_gpon_set_gem_info(struct airoha_xpon *xpon, u16 gem_port,
				   bool upstream, bool encrypt);
int  airoha_xpon_gpon_get_gem_info(struct airoha_xpon *xpon, u16 gem_port);
int  airoha_xpon_gpon_set_alloc_id(struct airoha_xpon *xpon, u8 tcont,
				   u16 alloc_id);
u16  airoha_xpon_gpon_get_alloc_id(struct airoha_xpon *xpon, u8 tcont);

/* ---- QoS / DBA ---- */
int  airoha_xpon_gpon_qos_init(struct airoha_xpon *xpon);
int  airoha_xpon_gpon_qos_deinit(struct airoha_xpon *xpon);
int  airoha_xpon_gpon_tcont_param(struct airoha_xpon *xpon, u8 tcont,
				  u32 green, u32 total, u32 yellow, u32 max);
int  airoha_xpon_gpon_tcont_release(struct airoha_xpon *xpon, u8 tcont);
void airoha_xpon_gpon_set_qdma_qos(struct airoha_xpon *xpon, bool reset);

/* ---- EqD ---- */
void airoha_xpon_gpon_eqd_adjust(struct airoha_xpon *xpon, u32 new_eqd);
void airoha_xpon_gpon_eqd_retry(struct airoha_xpon *xpon, u8 retry);

/* ---- 掉线恢复 ---- */
struct airoha_gpon_recovery_gem;
int  airoha_xpon_gpon_backup_gem(struct airoha_xpon *xpon,
				 const struct airoha_gpon_recovery_gem *e);
int  airoha_xpon_gpon_check_gem_exist(struct airoha_xpon *xpon, u16 gem_id);
int  airoha_xpon_gpon_recover(struct airoha_xpon *xpon);

/* ---- 计数器 ---- */
void airoha_xpon_gpon_counters_update(struct airoha_xpon *xpon);

/* ---- 芯片参数 ---- */
u32  airoha_xpon_gpon_get_hw_param(struct airoha_xpon *xpon, u32 reg);

/* ---- 调试 ---- */
void airoha_xpon_gpon_debug_mac(struct airoha_xpon *xpon, struct seq_file *m);

#endif /* _AIROHA_XPON_GPON_H_ */
