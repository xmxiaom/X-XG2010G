// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha AN7581 GPON 线路层
 *
 * 实现依据：原厂 xpon.ko（XG2010G_fw_1.0.0.13，Linux 5.4.55，aarch64）
 * 的完整逆向，详见：
 *   docs/06-逆向成果-GPON寄存器表.md   —— 110 个寄存器
 *   docs/08-PLOAM层逆向成果.md         —— PLOAM 收发与 21 项分派表
 *   docs/09-命令下发链路与中断分派.md  —— 命令分发器与模式分流
 *
 * 目标位置：openwrt-pon-drivers/airoha-xpon/src/airoha-xpon-gpon.c
 *
 * 与上游框架的接口约定（见 docs/05）：
 *   - airoha-xpon-mode.c 的 gpon 条目补 .mac_supported = true
 *   - airoha-xpon-main.c 增加 airoha_xpon_gpon_* 回调分发
 *   - airoha-xpon-private.h 增加 void __iomem *gpon_base
 *
 * 与上游的差异说明：
 *   原厂把硬件操作收敛到「命令码 → 分发器」，上层（状态机 / PLOAM）与
 *   下层（寄存器）通过命令码解耦。本实现沿用这一分层，因此
 *   airoha_xpon_gpon_cmd() 是唯一的硬件出口。
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/netdevice.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/swab.h>
#include <linux/timer.h>

#include "airoha-xpon-private.h"
#include "airoha-xpon-proto.h"
#include "airoha-xpon-gpon.h"
#include "airoha-xpon-gpon-regs.h"

/* ------------------------------------------------------------------ */
/* 常量与协议定义                                                       */
/* ------------------------------------------------------------------ */

/* G.984.3 ONU 激活状态（对应寄存器 0x00bc 低 3 位） */
enum airoha_gpon_state {
	AIROHA_GPON_O1_INITIAL		= 1,
	AIROHA_GPON_O2_STANDBY		= 2,
	AIROHA_GPON_O3_SERIAL		= 3,
	AIROHA_GPON_O4_RANGING		= 4,
	AIROHA_GPON_O5_OPERATION	= 5,
	AIROHA_GPON_O6_POPUP		= 6,
	AIROHA_GPON_O7_EMERGENCY	= 7,
};

/* 寄存器 0x00bc 只有低 3 位是状态，高位必须保留 */
#define AIROHA_GPON_STATE_MASK		0x7

/* PLOAMd FIFO 状态寄存器 0x0058 的低字节 = 待读条目数 */
#define AIROHA_GPON_PLOAMD_EMPTY	0
#define AIROHA_GPON_PLOAMD_BAD		1	/* 1、2 视为不完整 */
#define AIROHA_GPON_PLOAMD_BAD_MAX	2
#define AIROHA_GPON_PLOAMD_MAX		3	/* 一次最多处理到剩 3 条 */

/* PLOAM 报文固定 12 字节 = 3 个 32-bit 字 */
#define AIROHA_GPON_PLOAM_WORDS		3
#define AIROHA_GPON_PLOAM_BYTES		(AIROHA_GPON_PLOAM_WORDS * 4)

/* G.984.3 下行 PLOAM 消息 ID */
#define AIROHA_GPON_MSG_UPSTREAM_OVERHEAD	0x01
#define AIROHA_GPON_MSG_ASSIGN_ONU_ID		0x03
#define AIROHA_GPON_MSG_RANGING_TIME		0x04
#define AIROHA_GPON_MSG_PASSWORD		0x05
#define AIROHA_GPON_MSG_DEACTIVATE_ONU_ID	0x06
#define AIROHA_GPON_MSG_REQUEST_KEY		0x08
#define AIROHA_GPON_MSG_CONFIGURE_PORT_ID	0x09
#define AIROHA_GPON_MSG_ENCRYPTED_PORT_ID	0x0a
#define AIROHA_GPON_MSG_PEE			0x0c
#define AIROHA_GPON_MSG_PST			0x0d
#define AIROHA_GPON_MSG_BER_INTERVAL		0x0e
#define AIROHA_GPON_MSG_DYING_GASP		0x0f

/* 分派表下标上限（原厂 ploam_parser_down_message 里是 0x18） */
#define AIROHA_GPON_PLOAM_MAX_ID		0x18
#define AIROHA_GPON_PLOAM_TABLE_SIZE		0x19

/* ONU-ID 广播/未分配标记 */
#define AIROHA_GPON_ONU_ID_UNASSIGNED		0xff

/* 上行 PLOAM 的 Send 位 */
#define AIROHA_GPON_PLOAMU_SEND			BIT(7)

/* ------------------------------------------------------------------ */
/* 中断位定义（寄存器 0x0008 状态 / 0x000c 使能）                       */
/* 依据：gpon_isr @0x01bcc4 的位掩码提取，见 docs/10                   */
/* ------------------------------------------------------------------ */
#define AIROHA_GPON_IRQ_FRAME_SYNC	BIT(11)
#define AIROHA_GPON_IRQ_PLOAMD		BIT(8)	/* PLOAMd FIFO 非空 */
#define AIROHA_GPON_IRQ_DN_HEADER_ERR	BIT(4)
#define AIROHA_GPON_IRQ_DN_SYNC_LOSS	BIT(5)
#define AIROHA_GPON_IRQ_US_BURST_ERR	BIT(12)
#define AIROHA_GPON_IRQ_US_FEC		BIT(15)
#define AIROHA_GPON_IRQ_BWMAP		BIT(24)
#define AIROHA_GPON_IRQ_BWMAP_OVF	BIT(25)
#define AIROHA_GPON_IRQ_GRANT_LOSS	BIT(26)
#define AIROHA_GPON_IRQ_US_OVERHEAD	BIT(29)
#define AIROHA_GPON_IRQ_ONU_ID		BIT(28)
#define AIROHA_GPON_IRQ_ACT_CHANGE	BIT(27)
#define AIROHA_GPON_IRQ_DYING_GASP	BIT(31)
#define AIROHA_GPON_IRQ_CNT_HALF	BIT(7)
#define AIROHA_GPON_IRQ_CNT_OVF		BIT(6)
#define AIROHA_GPON_IRQ_GEM_HDR_ERR	BIT(20)
#define AIROHA_GPON_IRQ_GEM_FRAME_ERR	BIT(19)
#define AIROHA_GPON_IRQ_GEM_NO_MATCH	BIT(18)
#define AIROHA_GPON_IRQ_GEM_DROP	BIT(17)
#define AIROHA_GPON_IRQ_SUPERFRAME	BIT(30)
#define AIROHA_GPON_IRQ_KEY_SWITCH	BIT(21)
#define AIROHA_GPON_IRQ_KEY_SYNC	BIT(22)
#define AIROHA_GPON_IRQ_ENC_ERR		BIT(23)
#define AIROHA_GPON_IRQ_TOD		BIT(16)

/* 需要处理的中断位（其余只计数） */
#define AIROHA_GPON_IRQ_WANTED						\
	(AIROHA_GPON_IRQ_PLOAMD | AIROHA_GPON_IRQ_DYING_GASP |		\
	 AIROHA_GPON_IRQ_SUPERFRAME | AIROHA_GPON_IRQ_FRAME_SYNC |	\
	 AIROHA_GPON_IRQ_BWMAP | AIROHA_GPON_IRQ_GRANT_LOSS |		\
	 AIROHA_GPON_IRQ_ACT_CHANGE | AIROHA_GPON_IRQ_ONU_ID)

/* 调试位图（原厂 gpPonSysData->[+0x1b18]） */
#define AIROHA_GPON_DBG_STATE		BIT(0)
#define AIROHA_GPON_DBG_IRQ		BIT(2)
#define AIROHA_GPON_DBG_TRANS		BIT(3)
#define AIROHA_GPON_DBG_ENTRY		BIT(4)
#define AIROHA_GPON_DBG_ALL		BIT(12)

/*
 * 硬件命令码（对应原厂 0x0b620 命令分发器的 switch 分支）
 * 原厂命令码是 0x01–0x08 与 0x101–0x107，此处按语义重命名。
 */
enum airoha_gpon_cmd {
	AIROHA_GPON_CMD_LOS_DETECT	= 0x01,	/* 原厂 0x01/0x03/0x06/0x08 */
	AIROHA_GPON_CMD_ROGUE_START	= 0x101,/* 开始流氓 ONU 检测 */
	AIROHA_GPON_CMD_ROGUE_STOP	= 0x102,/* 停止 */
	AIROHA_GPON_CMD_LINK_SETUP	= 0x103,/* 置「连接建立中」标志 */
	AIROHA_GPON_CMD_TX_BUFF_INIT	= 0x104,/* QDMA TX 缓冲初始化 */
	AIROHA_GPON_CMD_CONN_UP		= 0x107,/* 连接建立完成 */
};

/* 状态机里 apply_state_config 的命令码 */
#define AIROHA_GPON_CFG_TIMER_A			0x09
#define AIROHA_GPON_CFG_TIMER_B			0x0a
#define AIROHA_GPON_CFG_STATE			0x12

/* 定时器超时值：原厂是 ms * 1000 / 8 */
#define AIROHA_GPON_MS_TO_JIFFIES(ms)		\
	((unsigned long)((u64)(ms) * 1000 / 8))

/* ------------------------------------------------------------------ */
/* 驱动私有结构（对应原厂 gpGponPriv 的字段布局）                        */
/* ------------------------------------------------------------------ */

struct airoha_xpon;

/* 掉线恢复表：GEM 表项上限 256（原厂 0x100） */
#define AIROHA_GPON_RECOVERY_GEM_MAX	0x100

/**
 * struct airoha_gpon_recovery_gem - 备份的单条 GEM 配置
 *
 * 原厂表项 12 字节：{gem_id, alloc_id, encrypt, direction, flags}
 */
struct airoha_gpon_recovery_gem {
	u16	gem_id;
	u16	alloc_id;
	u8	encrypt;
	u8	direction;
	u16	flags;
	u32	reserved;
};

/**
 * struct airoha_gpon_recovery - 掉线恢复表
 *
 * 原厂 gponRecovery 布局（0xc88 字节）：
 *   +0x000  alloc_id[32]
 *   +0x080  gem[256]（每项 12 字节）
 *   +0xc80  gem_count
 *   +0xc84  alloc_count
 *   +0xc88  debug
 */
struct airoha_gpon_recovery {
	u16				alloc_id[32];
	struct airoha_gpon_recovery_gem	gem[AIROHA_GPON_RECOVERY_GEM_MAX];
	u32				gem_count;
	u32				alloc_count;
	bool				debug;
};

/* 分派表函数类型：返回 0 表示已处理，负数表示错误 */
typedef int (*airoma_ploamd_fn)(struct airoha_xpon *xpon, const u8 *msg);

/**
 * struct airoha_gpon_priv - GPON 线路层私有数据
 *
 * 字段顺序与原厂 gpGponPriv 对应，便于对照逆向结论。
 * 实际接入上游时，若上游已有等价字段（如 xpon->mib），优先复用。
 */
/* T-CONT 数量：原厂 gpon_qos_init 的循环是 cmp w0, #0x1f，即 0..31 共 32 个 */
/* AIROHA_GPON_TCONT_MAX 定义在 gpon.h，stats.c 也要用 */

/**
 * struct airoha_gpon_tcont_cfg - 单个 T-CONT 的 DBA 参数
 *
 * 字段顺序照原厂 hook 0x29 的参数结构（从 gpon_qos_init 栈偏移反推）：
 *   +0x00  u8   tcont
 *   +0x04  u32  green
 *   +0x08  u32  yellow
 *   +0x0c  u32  total
 *   +0x10  u32  max
 */
struct airoha_gpon_tcont_cfg {
	u8	tcont;
	u32	green;		/* fixed 带宽（保证） */
	u32	yellow;		/* 非保证带宽 */
	u32	total;		/* assured 总保证 */
	u32	max;		/* 最大带宽 */
};

/* 告警环形缓冲深度 */
#define AIROHA_GPON_EMERGENCE_MAX	16

/**
 * struct airoha_gpon_emergence - 一条告警记录
 *
 * 原厂 gpon_record_emergence_info / gpon_show_emergence_info
 * 用环形缓冲记录最近告警，供掉线排查与运营商远程诊断。
 */
struct airoha_gpon_emergence {
	u32	code;		/* 告警码 */
	u32	param;		/* 附加参数 */
	u64	ts_ns;		/* 时间戳 */
};

struct airoha_gpon_priv {
	u32			to1_retry_cnt;		/* +0x00 */
	u32			state;			/* +0x04 当前 O1–O7 */
	struct timer_list	to1_timer;		/* +0x08 */
	u64			to1_expire;		/* +0x18 */
	u32			qdma_tx_state;		/* +0x20 */
	struct timer_list	to2_timer;		/* +0x30 */
	u64			to2_expire;		/* +0x40 */
	/* +0x108: PLOAMd 分派表 */
	airoma_ploamd_fn	ploamd_table[AIROHA_GPON_PLOAM_TABLE_SIZE];
	u8			last_ploamd[AIROHA_GPON_PLOAM_BYTES];	/* +0x1d0 */
	u8			vssn[8];		/* +0x1dd */
	u32			to1_ms;			/* +0x210 */
	bool			to1_expired;		/* TO1 超时标记 */
	bool			to2_expired;		/* TO2 超时标记 */
	u32			to2_ms;			/* +0x214 */
	u32			last_eqd;		/* +0x220 上次的 EqD 基准 */
	u32			eqd_pending;		/* +0x224 待写入的 EqD */
	u8			ranging_retry;		/* +0x228 测距余数 */
	u8			eqd_mode;		/* +0x22a 0=首次 / 非0=需校正 */
	u8			eqd_step;		/* EqD 步长（原厂从 0x22a 高位取） */
	u32			superframe;	/* 超帧计数器缓存 */
	u8			los_ignore_flag;	/* 原厂 +0x30c bit3：O5 忽略 LOS */
	/*
	 * per-T-CONT 的 DBA 参数。原厂经 EcoNet hook 0x29 下发给
	 * 硬件抽象层，上游暂无该 hook，这里保存下来：
	 *   - debugfs 可查看，便于与原厂固件对比
	 *   - 后续若接上 hook 可直接转发
	 */
	struct airoha_gpon_tcont_cfg tcont[AIROHA_GPON_TCONT_MAX + 1];
	/* QDMA TX buff 参数（原厂 gpon_init_qdma_tx_buff @0x14b94） */
	u32			qdma_tx_buff_enable;	/* +0x00 = 1 */
	u32			qdma_tx_buff_blocks;	/* +0x04 = 4 */
	u32			qdma_tx_buff_size;	/* +0x06 = 0xe0 */
	/* GPON AES-128 加密密钥（原厂 gponDevSetEncryptKey，16 字节） */
	u8			aes_key[16];
	/* 国内移动 PLOAM Password（10 字符 ASCII，不足补 0） */
	char			sn_passwd[10];
	bool			sn_passwd_set;
	/* BER / SD / SF 检测状态（原厂 gpon_ber_interval_expires） */
	bool			sd_flag;
	bool			sf_flag;
	u64			ber_window_errors;
	u64			ber_window_bits;
	/* TypeB 保护倒换的队列配置备份（原厂 gpon_dev_typeB_restore） */
	u32			typeb_queue_map[8];
	u32			typeb_queue_enable;
	u32			typeb_queue_threshold;
	u32			typeb_queue_ctrl;
	bool			typeb_backup_valid;
	/* 告警记录环形缓冲（原厂 gpon_record_emergence_info） */
	struct airoha_gpon_emergence emergence[AIROHA_GPON_EMERGENCE_MAX];
	u32			emergence_head;
	u32			emergence_count;
	s32			hw_param_cache;		/* +0x24c，初值 -1 */
};

/* 前置声明：状态机与命令分发互相调用 */
void airoha_xpon_gpon_set_state(struct airoha_xpon *xpon, u32 new_state);
int airoha_xpon_gpon_cmd(struct airoha_xpon *xpon, u32 arg, u32 cmd);
void airoha_xpon_gpon_apply_cfg(struct airoha_xpon *xpon, u32 cmd, u32 param);
void airoha_xpon_gpon_eqd_adjust(struct airoha_xpon *xpon, u32 new_eqd);
void airoha_xpon_gpon_eqd_retry(struct airoha_xpon *xpon, u8 retry);
void airoha_xpon_gpon_detect_los_lof(struct airoha_xpon *xpon, u32 arg);
void airoha_xpon_gpon_connection_up(struct airoha_xpon *xpon);
bool airoha_xpon_gpon_backoff_ok(struct airoha_xpon *xpon);
u32 airoha_xpon_gpon_get_hw_param(struct airoha_xpon *xpon, u32 reg);
void airoha_xpon_gpon_send_dying_gasp(struct airoha_xpon *xpon);
void airoha_xpon_gpon_set_ploamu_int(struct airoha_xpon *xpon, bool on);
void airoha_xpon_gpon_set_bwm_stop_int(struct airoha_xpon *xpon, bool on);
void airoha_xpon_gpon_get_superframe(struct airoha_xpon *xpon);
void airoha_xpon_gpon_rogue_handle(struct airoha_xpon *xpon, bool on);
void airoha_xpon_gpon_disable(struct airoha_xpon *xpon, bool keep_rx);
void airoha_xpon_gpon_report_event(struct airoha_xpon *xpon, u32 event,
				   u32 param);
void airoha_xpon_gpon_fiber_hotplug(struct airoha_xpon *xpon, bool present);
void airoha_xpon_gpon_hwnat_clean(struct airoha_xpon *xpon);
void airoha_xpon_gpon_disable_cpu_traffic(struct airoha_xpon *xpon);
int airoha_xpon_gpon_qos_init(struct airoha_xpon *xpon);
int airoha_xpon_gpon_qos_deinit(struct airoha_xpon *xpon);
int airoha_xpon_gpon_tcont_param(struct airoha_xpon *xpon, u8 tcont,
				 u32 green, u32 total, u32 yellow, u32 max);
int airoha_xpon_gpon_tcont_release(struct airoha_xpon *xpon, u8 tcont);
void airoha_xpon_gpon_set_qdma_qos(struct airoha_xpon *xpon, bool reset);
void airoha_xpon_gpon_init_qdma_tx_buff(struct airoha_xpon *xpon);
void airoha_xpon_gpon_set_encrypt_key(struct airoha_xpon *xpon,
				      const u8 key[16]);
int airoha_xpon_gpon_set_shadow_key(struct airoha_xpon *xpon, const u8 key[16]);
int airoha_xpon_gpon_aes_key_switch_by_sw(struct airoha_xpon *xpon);
int airoha_xpon_gpon_set_key_switch_time(struct airoha_xpon *xpon, u32 superframe);
int airoha_xpon_gpon_gem_mib_init(struct airoha_xpon *xpon);
int airoha_xpon_gpon_get_gem_counter(struct airoha_xpon *xpon, u16 gem_port,
				     u32 *rx_gem, u32 *tx_gem);
int airoha_xpon_gpon_clear_gem_counter(struct airoha_xpon *xpon, u16 gem_port);
int airoha_xpon_gpon_get_tcont_counter(struct airoha_xpon *xpon, u16 tcont,
				       u32 *rx, u32 *tx);
int airoha_xpon_gpon_get_error_status(struct airoha_xpon *xpon, u32 *grp0,
				      u32 *grp1);
void airoha_xpon_gpon_record_emergence(struct airoha_xpon *xpon, u32 code,
				       u32 param);
void airoha_xpon_gpon_show_emergence(struct airoha_xpon *xpon,
				     struct seq_file *m);
void airoha_xpon_gpon_set_dba_slight_modify(struct airoha_xpon *xpon, bool on);
void airoha_xpon_gpon_set_dba_shift_modify(struct airoha_xpon *xpon, bool on);
void airoha_xpon_gpon_set_dba_slight_value(struct airoha_xpon *xpon,
					   u32 green, u32 total, u32 yellow);
int airoha_xpon_gpon_typeb_reset(struct airoha_xpon *xpon, bool for_typeb);
int airoha_xpon_gpon_typeb_restore(struct airoha_xpon *xpon);
int airoha_xpon_gpon_typeb_backup(struct airoha_xpon *xpon);
void airoha_xpon_gpon_dump_csr(struct airoha_xpon *xpon);
void airoha_xpon_gpon_set_sniff(struct airoha_xpon *xpon, bool on);
void airoha_xpon_gpon_dbg_err_check(struct airoha_xpon *xpon);
/* BER / SD / SF 检测（原厂 gpon_ber_interval_expires @0x1afe4，660B） */
void airoha_xpon_gpon_ber_check(struct airoha_xpon *xpon);
void airoha_xpon_gpon_sd_sf_init(struct airoha_xpon *xpon);
/* 硬件参数配置（原厂 dev_init 路径上的 8 个函数） */
void airoha_xpon_gpon_set_rdi_err_ctrl(struct airoha_xpon *xpon, bool on,
				       u32 threshold);
void airoha_xpon_gpon_set_rdi_send_num(struct airoha_xpon *xpon, u32 num);
void airoha_xpon_gpon_set_counter_type(struct airoha_xpon *xpon, u32 type);
void airoha_xpon_gpon_set_response_time(struct airoha_xpon *xpon, u32 value);
void airoha_xpon_gpon_set_internal_delay_fine(struct airoha_xpon *xpon, u32 v);
void airoha_xpon_gpon_set_ploam_filter(struct airoha_xpon *xpon, u32 ext_burst,
				       u32 upstream_overhead);
/* 供 debugfs 统计层使用的访问器 */
void airoha_xpon_gpon_dump_csr_seq(struct airoha_xpon *xpon, struct seq_file *m);
u32 airoha_xpon_gpon_superframe_value(struct airoha_xpon *xpon);
const char *airoha_xpon_gpon_sn_passwd(struct airoha_xpon *xpon);
int airoha_xpon_gpon_set_sn_passwd(struct airoha_xpon *xpon, const char *pwd,
				   size_t len);
bool airoha_xpon_gpon_typeb_backup_valid(struct airoha_xpon *xpon);
int airoha_xpon_gpon_backup_gem(struct airoha_xpon *xpon,
				const struct airoha_gpon_recovery_gem *e);
int airoha_xpon_gpon_check_gem_exist(struct airoha_xpon *xpon, u16 gem_id);
int airoha_xpon_gpon_recover(struct airoha_xpon *xpon);
int airoha_xpon_gpon_prepare(struct airoha_xpon *xpon);
bool airoha_xpon_gpon_los(struct airoha_xpon *xpon);
int airoha_xpon_gpon_advance(struct airoha_xpon *xpon, u32 phy_events,
			     unsigned long *delay);
void airoha_xpon_gpon_debug_mac(struct airoha_xpon *xpon, struct seq_file *m);
int airoha_xpon_gpon_reset_ctrl(struct airoha_xpon *xpon, bool full);
int airoha_xpon_gpon_enable(struct airoha_xpon *xpon);
static void airoha_xpon_gpon_write_state(struct airoha_xpon *xpon, u32 state);
static u32 airoha_xpon_gpon_read_param(struct airoha_xpon *xpon, u32 reg);
static void airoha_xpon_scu_rstctrl1_set(struct airoha_xpon *xpon, u32 bits);
static void airoha_xpon_scu_rstctrl1_clear(struct airoha_xpon *xpon, u32 bits);
static int airoha_xpon_gpon_dev_init(struct airoha_xpon *xpon);
static void airoha_xpon_gpon_select_wan(struct airoha_xpon *xpon, u32 sel);
static void airoha_xpon_gpon_set_sniff_mode(struct airoha_xpon *xpon, bool on);
static void airoha_xpon_gpon_set_hw_dying_gasp(struct airoha_xpon *xpon, bool on);
static void airoha_xpon_gpon_set_idle_gem_threshold(struct airoha_xpon *xpon, u32 v);
static void airoha_xpon_gpon_mbi_stop(struct airoha_xpon *xpon, bool stop);
static void airoha_xpon_gpon_mpi_stop(struct airoha_xpon *xpon, bool stop);
static void airoha_xpon_gpon_disable_all_tcont(struct airoha_xpon *xpon);
static void airoha_xpon_gpon_sw_reset(struct airoha_xpon *xpon);
static int airoha_xpon_gpon_register_isr(struct airoha_xpon *xpon);

/* ------------------------------------------------------------------ */
/* 寄存器访问                                                          */
/* ------------------------------------------------------------------ */

static inline u32 gpon_rd(struct airoha_xpon *xpon, u32 off)
{
	return readl(xpon->gpon_base + off);
}

static inline void gpon_wr(struct airoha_xpon *xpon, u32 off, u32 val)
{
	writel(val, xpon->gpon_base + off);
}

static inline void gpon_rmw(struct airoha_xpon *xpon, u32 off, u32 mask, u32 val)
{
	u32 tmp = gpon_rd(xpon, off);

	tmp = (tmp & ~mask) | (val & mask);
	gpon_wr(xpon, off, tmp);
}

int airoha_xpon_gpon_priv_init(struct airoha_xpon *xpon);

static inline struct airoha_gpon_priv *gpon_priv(struct airoha_xpon *xpon)
{
	return xpon->gpon_priv;
}

/*
 * TO1/TO2 超时回调。
 *
 * TO1（O4→O5 等待）：OLT 下发 Registration 后等待 OMCI 建立。
 * TO2（O5 保持）：周期检查链路是否还在 O5。
 *
 * 原厂用 gpon_act_to1_timer_expires / gpon_act_to2_timer_expires
 * 两个独立回调，这里合并为一个，用定时器指针区分。
 *
 * 注意：定时器在 softirq 上下文运行，不能睡眠，因此只做状态标记，
 * 重活交给主状态机。
 */
static void airoha_xpon_gpon_to1_expires(struct timer_list *t)
{
	struct airoha_gpon_priv *priv = timer_container_of(priv, t, to1_timer);

	if (!priv)
		return;

	priv->to1_expired = true;
}

static void airoha_xpon_gpon_to2_expires(struct timer_list *t)
{
	struct airoha_gpon_priv *priv = timer_container_of(priv, t, to2_timer);

	if (!priv)
		return;

	priv->to2_expired = true;
}

/**
 * airoha_xpon_gpon_priv_init - 分配并初始化 GPON 私有数据
 * @xpon: XPON 设备
 *
 * 补上此前缺失的两件事：
 *   1. 分配 xpon->gpon_priv。此前该指针从未分配，恒为 NULL，
 *      导致所有 GPON 函数在 `if (!priv) return` 处直接退出，
 *      GPON 功能完全不可用。
 *   2. timer_setup 初始化 to1_timer / to2_timer。此前未初始化
 *      就直接 mod_timer，会在内核里触发 BUG。
 *
 * 返回 0 成功，负值失败。
 */
int airoha_xpon_gpon_priv_init(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv;

	if (!xpon)
		return -EINVAL;

	if (xpon->gpon_priv)
		return 0;	/* 已初始化，幂等 */

	priv = devm_kzalloc(xpon->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* 初始状态 O1，默认超时值（原厂默认） */
	priv->state = AIROHA_GPON_O1_INITIAL;
	priv->to1_ms = 10000;	/* 10 秒：等 OMCI 建立 */
	priv->to2_ms = 30000;	/* 30 秒：O5 保持检查 */
	priv->hw_param_cache = -1;
	priv->to1_expired = false;
	priv->to2_expired = false;

	timer_setup(&priv->to1_timer, airoha_xpon_gpon_to1_expires, 0);
	timer_setup(&priv->to2_timer, airoha_xpon_gpon_to2_expires, 0);

	xpon->gpon_priv = priv;

	dev_dbg(xpon->dev, "GPON private data initialized\n");
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_priv_init);

/* ------------------------------------------------------------------ */
/* PLOAMu 发送                                                         */
/* 原厂：gponDevSendPloamMsg @0x01df34（352 字节）                      */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_send_ploamu - 发送一条上行 PLOAM
 * @xpon:  XPON 设备
 * @msg:   12 字节报文，msg[0] 的 bit7 为 Send 位、bit0-6 为 ONU-ID，
 *         msg[1] 为消息 ID
 * @count: 报文条数，通常为 1
 *
 * 原厂实现要点：
 *   1. 掉电保护：已进入 Dying Gasp 且本条不是 Dying_Gasp 时直接返回
 *   2. 固定写 3 个 32-bit 字到 0x0054（PLOAMu FIFO），每个字都要 swab32
 *   3. 若本条是 Dying_Gasp，按 gp_dying_info 配置做延迟并清除掉电标记
 *
 * 返回 0 表示成功，负值为错误码。
 */
int airoha_xpon_gpon_send_ploamu(struct airoha_xpon *xpon, const u8 *msg,
				 u32 count)
{
	const u32 *words = (const u32 *)msg;
	u32 i, j;

	if (!xpon->gpon_base)
		return -ENODEV;

	/* 掉电保护：除 Dying_Gasp 外一律不再发送 */
	if (xpon->gpon_dying && msg[1] != AIROHA_GPON_MSG_DYING_GASP)
		return 0;

	for (i = 0; i < count; i++) {
		for (j = 0; j < AIROHA_GPON_PLOAM_WORDS; j++) {
			u32 w = words[i * AIROHA_GPON_PLOAM_WORDS + j];

			gpon_wr(xpon, AIROHA_GPON_PLOAMU_FIFO_DATA,
				swab32(w));
		}
	}

	/* Dying_Gasp 发送后清除掉电标记，防止重复上报 */
	if (msg[1] == AIROHA_GPON_MSG_DYING_GASP)
		xpon->gpon_dying = false;

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_send_ploamu);

/* ------------------------------------------------------------------ */
/* PLOAMd 接收                                                         */
/* 原厂：gponDevGetPloamMsg @0x01de8c（168 字节）                       */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_get_ploamd - 从 FIFO 取一条下行 PLOAM
 * @xpon: XPON 设备
 * @out:  12 字节输出缓冲
 *
 * 返回 FIFO 中剩余条目数；0 表示空，负值表示数据不完整。
 */
int airoha_xpon_gpon_get_ploamd(struct airoha_xpon *xpon, u8 *out)
{
	u32 status, j;

	if (!xpon->gpon_base)
		return -ENODEV;

	status = gpon_rd(xpon, AIROHA_GPON_PLOAMD_FIFO_STATUS);
	status &= 0xff;				/* 低字节是条目数 */

	if (status == AIROHA_GPON_PLOAMD_EMPTY)
		return 0;

	if (status <= AIROHA_GPON_PLOAMD_BAD_MAX)
		return -EINVAL;			/* 1、2 条视为不完整 */

	for (j = 0; j < AIROHA_GPON_PLOAM_WORDS; j++) {
		u32 w = gpon_rd(xpon, AIROHA_GPON_PLOAMD_FIFO_DATA);

		((u32 *)out)[j] = swab32(w);
	}

	return status;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_get_ploamd);

/* ------------------------------------------------------------------ */
/* 重复帧过滤                                                          */
/* 原厂：sub_0x1b410（120 字节）                                        */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_ploamd_dedup - 判断是否为重复帧
 * @xpon: XPON 设备
 * @msg:  新收到的 12 字节报文
 *
 * 返回 true 表示重复（应丢弃）。
 *
 * 原厂实现：与「上一条已收帧」做 memcmp。
 * Ranging_Time（ID 4）只比前 7 字节，因为 EqD 每次都会变，
 * 比全 12 字节会导致永远判不出重复。
 *
 * 注意：GPON 的 PLOAMd 不做 MIC 校验，下行完整性由 OLT 负责，
 * ONU 侧只做去重。因此本驱动无需实现 AES-CMAC。
 */
bool airoha_xpon_gpon_ploamd_dedup(struct airoha_xpon *xpon, const u8 *msg)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	size_t len;

	len = (msg[1] == AIROHA_GPON_MSG_RANGING_TIME) ? 7
						       : AIROHA_GPON_PLOAM_BYTES;

	return memcmp(priv->last_ploamd, msg, len) == 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_ploamd_dedup);

/* ------------------------------------------------------------------ */
/* 下行消息处理器（21 项，对应原厂分派表）                              */
/* ------------------------------------------------------------------ */

/* Upstream_Overhead：写 0x0090–0x00a4 六个开销寄存器 */
static int gpon_msg_upstream_overhead(struct airoha_xpon *xpon, const u8 *msg)
{
	u32 phy_overhead = ((u32)msg[3] << 24) | ((u32)msg[4] << 16) |
			   ((u32)msg[5] << 8) | msg[6];

	/* 报文布局（G.984.3 §9.2.1）：
	 *   byte 3-6  物理层开销时间
	 *   byte 7    保护位数
	 *   byte 8    Type1/2 前导位数
	 *   byte 9    Type3 前导位数
	 *   byte 10   定界符位数
	 *   byte 11   预分配延迟
	 */
	gpon_wr(xpon, AIROHA_GPON_PHYSICAL_LAYER_OVERHEAD_TIME, phy_overhead);
	gpon_wr(xpon, AIROHA_GPON_NUMBER_GUARD_BITS, msg[7]);
	gpon_wr(xpon, AIROHA_GPON_NUMBER_TYPE1_TYPE2_PREAMBLE_BITS, msg[8]);
	gpon_wr(xpon, AIROHA_GPON_NUNMBER_TYPE3, msg[9]);
	gpon_wr(xpon, AIROHA_GPON_NUMBER_DELIMETER_BITS, msg[10]);
	gpon_wr(xpon, AIROHA_GPON_PRE_ASSIGNED_DELAY, msg[11]);

	return 0;
}

/*
 * Assign_ONU_ID：上线关键
 * 原厂 0x015e58：
 *   1. msg[0] 必须是 0xff（未分配），否则不是给我们的
 *   2. memcmp(gpGponPriv+0x1dd, msg+3, 8) 必须相等（VSSN 比对）
 *   3. 校验通过 → 直接进 O5，写 ONU-ID 到 0x0004，初始化 QDMA TX
 */
static int gpon_msg_assign_onu_id(struct airoha_xpon *xpon, const u8 *msg)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	u32 v;

	if (msg[0] != AIROHA_GPON_ONU_ID_UNASSIGNED) {
		dev_dbg(xpon->dev, "Assign_ONU_ID: not for us (id=%u)\n",
			msg[0]);
		return -EINVAL;
	}

	/* VSSN 必须与本机一致，SN 写错就会卡在 O3 */
	if (memcmp(priv->vssn, msg + 3, sizeof(priv->vssn)) != 0) {
		dev_warn(xpon->dev,
			 "Assign_ONU_ID: VSSN mismatch, check serial number\n");
		return -EINVAL;
	}

	airoha_xpon_gpon_set_state(xpon, AIROHA_GPON_O5_OPERATION);

	/* 0x0004 的低字节是 ONU-ID */
	v = gpon_rd(xpon, AIROHA_GPON_ONU_GLOBAL_CONFIGURATION_REGISTER);
	v = (v & ~0xff) | (msg[0] & 0xff);
	gpon_wr(xpon, AIROHA_GPON_ONU_GLOBAL_CONFIGURATION_REGISTER, v);

	/* 原厂此处还调 gpon_init_qdma_tx_buff()，由 QoS 层实现 */
	return airoha_xpon_gpon_cmd(xpon, 0, AIROHA_GPON_CMD_TX_BUFF_INIT);
}

/*
 * Ranging_Time：测距核心
 * 原厂 0x01609c：
 *   状态必须是 O4 或 O5
 *   写 EqD 到 0x00a8
 *   O5 额外走 ploam_eqd_adjustment 做精细校正
 *   O4 推进到 O5
 */
static int gpon_msg_ranging_time(struct airoha_xpon *xpon, const u8 *msg)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	u32 state = priv->state;

	if (state != AIROHA_GPON_O4_RANGING &&
	    state != AIROHA_GPON_O5_OPERATION) {
		dev_dbg(xpon->dev, "Ranging_Time in state O%u, ignored\n",
			state);
		return 0;
	}

	/* EqD 的唯一落点 */
	gpon_wr(xpon, AIROHA_GPON_MAIN_PATH_EQUALIZATION_DELAY,
		priv->eqd_pending);

	if (state == AIROHA_GPON_O5_OPERATION)
		airoha_xpon_gpon_eqd_adjust(xpon, priv->eqd_pending);

	return 0;
}

/* Deactivate_ONU-ID：回 O1 */
static int gpon_msg_deactivate(struct airoha_xpon *xpon, const u8 *msg)
{
	airoha_xpon_gpon_set_state(xpon, AIROHA_GPON_O1_INITIAL);
	return 0;
}

/* ------------------------------------------------------------------ */
/* O5 精细测距校正                                                     */
/* 原厂：ploam_eqd_adjustment @0x014bd0（1052 字节）                    */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_eqd_adjust - O5 重测距时的 EqD 增量校正
 * @xpon:     XPON 设备
 * @new_eqd: OLT 新下发的 EqD 原始值
 *
 * 原厂 ploam_eqd_adjustment @0x014bd0（1052 字节）完整逻辑：
 *
 *   1. delta = new_eqd - priv->last_eqd（+0x220）
 *   2. 若 priv->eqd_mode（+0x22a）== 0：首次，直接记录 last_eqd 并返回
 *   3. 按 0x2fc 的 bit3 决定 delta 的符号修正：
 *        bit3 置位 → delta -= eqd_step（+0x22a）
 *        否则      → delta += eqd_step
 *   4. 读 0x035c（TX_SYNC_OFFSET）低 2 位，作为子偏移基准 sub_base
 *   5. delta > 0（需要增大延迟）：
 *        tmp   = delta + priv->ranging_retry（+0x228）
 *        hi    = tmp >> 3          （8 的整数倍）
 *        lo    = tmp & 7           （余数）
 *        adj   = (hi & ~3) + (sub_base + (hi & 3) > 3 ? 8 : 0) - (hi & 3)
 *        eqd_pending（+0x224） += adj << 3
 *        ranging_retry = lo
 *   6. delta < 0（需要减小延迟）：
 *        tmp   = (-delta) - ranging_retry
 *        hi    = tmp > 0 ? (tmp >> 3) + ((tmp & 7) ? 1 : 0) : 0
 *        lo    = (hi << 3) - tmp
 *        adj   = (hi & ~3) + (sub_base < (hi & 3) ? 8 : 0) - (hi & 3)
 *        eqd_pending -= adj << 3
 *        ranging_retry = lo
 *   7. 清 eqd_mode
 *   8. 把 eqd_pending 写入 0x00a8，然后用 ranging_retry 调一次子函数
 *
 * 这套算法本质是「把 EqD 量化到 8 的整数倍，余数留到下次补偿」，
 * 与 GPON 的 8-bit 测距精度一致。
 */
void airoha_xpon_gpon_eqd_adjust(struct airoha_xpon *xpon, u32 new_eqd)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	s32 delta, tmp, hi, lo, adj;
	u32 sub_base;
	u32 v;

	if (!xpon->gpon_base || !priv)
		return;

	delta = (s32)new_eqd - (s32)priv->last_eqd;

	/* 首次测距：只记录基准，不做校正 */
	if (priv->eqd_mode == 0) {
		priv->last_eqd = new_eqd;
		return;
	}

	/* 按 TX_SYNC_OFFSET 的 bit3 修正 delta 的符号 */
	if (xpon->gpon_bwm_stop_cfg & BIT(3))
		delta -= priv->eqd_step;
	else
		delta += priv->eqd_step;

	/* 子偏移基准：0x035c 低 2 位 */
	v = gpon_rd(xpon, AIROHA_GPON_TX_SYNC_OFFSET);
	sub_base = v & 0x3;

	if (delta > 0) {
		tmp = delta + priv->ranging_retry;
		hi = tmp >> 3;
		lo = tmp & 0x7;

		adj = (hi & ~0x3) +
		      (((sub_base + (hi & 0x3)) > 3) ? 8 : 0) -
		      (hi & 0x3);

		priv->eqd_pending += (u32)(adj << 3);
		priv->ranging_retry = (u8)lo;
	} else if (delta < 0) {
		tmp = -delta - priv->ranging_retry;
		if (tmp > 0) {
			hi = (tmp >> 3) + ((tmp & 0x7) ? 1 : 0);
		} else {
			hi = 0;
		}
		lo = (hi << 3) - tmp;

		adj = (hi & ~0x3) +
		      ((sub_base < (hi & 0x3)) ? 8 : 0) -
		      (hi & 0x3);

		priv->eqd_pending -= (u32)(adj << 3);
		priv->ranging_retry = (u8)lo;
	}

	/* 清模式标记 */
	priv->eqd_mode = 0;

	if (xpon->gpon_debug & (AIROHA_GPON_DBG_ALL | AIROHA_GPON_DBG_ENTRY))
		dev_dbg(xpon->dev,
			"EqD: base=0x%x pending=0x%x delta=%d hi=%d lo=%d adj=%d retry=%u\n",
			priv->last_eqd, priv->eqd_pending, delta, hi, lo, adj,
			priv->ranging_retry);

	/* 写回 0x00a8 */
	gpon_wr(xpon, AIROHA_GPON_MAIN_PATH_EQUALIZATION_DELAY,
		priv->eqd_pending);

	/* 原厂尾部按 ranging_retry 调一次子函数（0x14b24），
	 * 作用待确认，暂按「更新重试计数」处理。
	 */
	airoha_xpon_gpon_eqd_retry(xpon, priv->ranging_retry);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_eqd_adjust);

/**
 * airoha_xpon_gpon_eqd_retry - EqD 校正后的收尾处理
 * @xpon:  XPON 设备
 * @retry: 新的测距重试计数
 *
 * 原厂在 ploam_eqd_adjustment 末尾调 sub_0x14b24(ranging_retry)，
 * 该函数与状态机的 timer_ctrl 路径相邻，推测用于更新重试计数并
 * 决定是否重新触发测距。此处保留接口，具体行为待进一步确认。
 */
void airoha_xpon_gpon_eqd_retry(struct airoha_xpon *xpon, u8 retry)
{
	xpon->gpon_backoff_cnt = retry;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_eqd_retry);

/* Dying_Gasp：对端掉电，记录状态 */
static int gpon_msg_dying_gasp(struct airoha_xpon *xpon, const u8 *msg)
{
	dev_info(xpon->dev, "OLT sent Dying_Gasp\n");
	return 0;
}

/* 未实现的处理器统一返回 0，避免上层误判为错误 */
static int gpon_msg_unhandled(struct airoha_xpon *xpon, const u8 *msg)
{
	return 0;
}

/* ------------------------------------------------------------------ */
/* 分派表安装                                                          */
/* 原厂：ploam_init @0x018d34（788 字节）                               */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_ploam_init - 安装下行消息分派表
 * @xpon: XPON 设备
 *
 * 表结构：下标 = 消息 ID，范围 0..0x18，未注册项为 NULL。
 * 原厂在 ploam_init 里逐项赋值，本实现用静态数组一次性填充。
 */
int airoha_xpon_gpon_ploam_init(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);

	if (!priv)
		return -ENODEV;

	/* 用运行时赋值而非静态指定初始化器：
	 *   1. 避免 [IDX] = fn 这种 GCC 扩展在部分编译器下的兼容问题
	 *   2. 未注册的槽位自然为 NULL（memset 后不赋值）
	 */
	memset(priv->ploamd_table, 0, sizeof(priv->ploamd_table));

	priv->ploamd_table[AIROHA_GPON_MSG_UPSTREAM_OVERHEAD] =
		gpon_msg_upstream_overhead;
	priv->ploamd_table[AIROHA_GPON_MSG_ASSIGN_ONU_ID] =
		gpon_msg_assign_onu_id;
	priv->ploamd_table[AIROHA_GPON_MSG_RANGING_TIME] =
		gpon_msg_ranging_time;
	priv->ploamd_table[AIROHA_GPON_MSG_DEACTIVATE_ONU_ID] =
		gpon_msg_deactivate;
	priv->ploamd_table[AIROHA_GPON_MSG_DYING_GASP] =
		gpon_msg_dying_gasp;
	/* msg_id 2 / 7 / 11 原厂未注册，保持 NULL */

	priv->hw_param_cache = -1;		/* 原厂初值 -1 */

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_ploam_init);

/* ------------------------------------------------------------------ */
/* 下行消息分派                                                        */
/* 原厂：ploam_parser_down_message @0x014fec（296 字节）                */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_dispatch_ploamd - 按消息 ID 分派一条下行 PLOAM
 * @xpon: XPON 设备
 * @msg:  12 字节报文
 *
 * 返回处理器结果；ID 越界或未注册时返回 -ENOSYS（原厂返回 -0xe）。
 */
int airoha_xpon_gpon_dispatch_ploamd(struct airoha_xpon *xpon, const u8 *msg)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	airoma_ploamd_fn fn;
	u32 id = msg[1];

	if (id > AIROHA_GPON_PLOAM_MAX_ID) {
		dev_dbg(xpon->dev, "PLOAMd id 0x%02x out of range\n", id);
		return -ENOSYS;
	}

	fn = priv->ploamd_table[id];
	if (!fn) {
		dev_dbg(xpon->dev, "PLOAMd id 0x%02x not registered\n", id);
		return -ENOSYS;
	}

	return fn(xpon, msg);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_dispatch_ploamd);

/* ------------------------------------------------------------------ */
/* 收帧主循环                                                          */
/* 原厂：gpon_recv_ploam_message @0x01b610（204 字节）                  */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_recv_ploamd - 从 FIFO 连续取帧并分派
 * @xpon: XPON 设备
 *
 * 原厂循环条件：rc > 3，即一直处理到 FIFO 剩余不超过 3 条。
 */
void airoha_xpon_gpon_recv_ploamd(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	u8 msg[AIROHA_GPON_PLOAM_BYTES];
	int rc;

	if (!xpon->gpon_ploam_ready)
		return;

	do {
		rc = airoha_xpon_gpon_get_ploamd(xpon, msg);
		if (rc <= 0)
			break;

		/* 去重：与上一条相同则丢弃 */
		if (airoha_xpon_gpon_ploamd_dedup(xpon, msg))
			continue;

		memcpy(priv->last_ploamd, msg, sizeof(msg));
		airoha_xpon_gpon_dispatch_ploamd(xpon, msg);
	} while (rc > AIROHA_GPON_PLOAMD_MAX);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_recv_ploamd);

/* ------------------------------------------------------------------ */
/* 命令分发器                                                          */
/* 原厂：0x0b620（848 字节），经 __ECNT_HOOK(0x1a) 到达                 */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_cmd - 唯一的硬件命令出口
 * @xpon: XPON 设备
 * @arg:  命令参数
 * @cmd:  命令码
 *
 * 原厂把 GPON 的硬件操作全部收敛到这里，上层（状态机 / PLOAM）
 * 只发命令码，不直接碰寄存器。命令码见 enum airoha_gpon_cmd。
 */
int airoha_xpon_gpon_cmd(struct airoha_xpon *xpon, u32 arg, u32 cmd)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);

	switch (cmd) {
	case AIROHA_GPON_CMD_LOS_DETECT:
		airoha_xpon_gpon_detect_los_lof(xpon, arg);
		break;

	case AIROHA_GPON_CMD_ROGUE_START:
		airoha_xpon_gpon_rogue_handle(xpon, true);
		break;

	case AIROHA_GPON_CMD_ROGUE_STOP:
		airoha_xpon_gpon_rogue_handle(xpon, false);
		break;

	case AIROHA_GPON_CMD_LINK_SETUP:
		xpon->gpon_link_setup = true;
		break;

	case AIROHA_GPON_CMD_TX_BUFF_INIT:
		airoha_xpon_gpon_init_qdma_tx_buff(xpon);
		break;

	case AIROHA_GPON_CMD_CONN_UP:
		airoha_xpon_gpon_connection_up(xpon);
		break;

	default:
		dev_dbg(xpon->dev, "unknown gpon cmd 0x%x\n", cmd);
		break;
	}

	(void)priv;
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_cmd);

/* ------------------------------------------------------------------ */
/* LOS/LOF 检测                                                        */
/* 原厂：gpon_detect_los_lof @0x01ab04（788 字节）                      */
/* 详见 docs/15-LOS检测逆向.md                                          */
/* ------------------------------------------------------------------ */

/* gpPhyData->flags30 的位布局（GPON/EPON 共用） */
#define AIROHA_PHY_MODE_SHIFT		3
#define AIROHA_PHY_MODE_MASK		0x78		/* bit3-6 */
#define AIROHA_PHY_STATE_MASK		0x700		/* bit8-10 */
#define AIROHA_PHY_MODE_GPON		1

/* GEM 头错误计数器（LOS 时打印用） */
#define AIROHA_GPON_GEM_HDR_1BIT_ERR	0x0330
#define AIROHA_GPON_GEM_HDR_2BIT_ERR	0x0334
#define AIROHA_GPON_GEM_HDR_UNCORR	0x0338

/**
 * airoha_xpon_gpon_detect_los_lof - 处理 LOS/LOF 事件
 * @xpon: XPON 设备
 * @arg:  1 表示「忽略本次事件」（预期内的 LOS，如主动关光）
 *
 * 原厂逻辑（788 字节完整还原）：
 *
 *   O2/O3/O4（未上线）：
 *       直接关 MAC 重来，因为没有业务需要保护
 *
 *   O5（已上线）：
 *       进 O6 POPUP 等 TO1 超时，期间光恢复可无缝继续
 *       同时清硬件 NAT 表、停 CPU 上行流量、做 TypeB 保护倒换
 *
 *   其他状态：只清 LOS 标记
 *
 * 末尾统一清 priv->[+0x2fc] 的 bit2（LOS 标记）。
 */
void airoha_xpon_gpon_detect_los_lof(struct airoha_xpon *xpon, u32 arg)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	u32 state;

	if (!xpon->gpon_base || !priv)
		return;

	if (xpon->gpon_debug & (AIROHA_GPON_DBG_ALL | AIROHA_GPON_DBG_STATE))
		dev_info(xpon->dev, "GPON IRQ: detect los lof\n");

	if (xpon->gpon_debug & AIROHA_GPON_DBG_STATE) {
		dev_dbg(xpon->dev, "Rx Loss of GEM Delineation: %u\n",
			gpon_rd(xpon, AIROHA_GPON_GEM_HDR_1BIT_ERR));
		dev_dbg(xpon->dev, "Loss of GEM delineation interrupt: %u\n",
			gpon_rd(xpon, AIROHA_GPON_GEM_HDR_2BIT_ERR));
		dev_dbg(xpon->dev, "GEM header uncorrectable: %u\n",
			gpon_rd(xpon, AIROHA_GPON_GEM_HDR_UNCORR));
	}

	state = priv->state;

	if (state == AIROHA_GPON_O2_STANDBY ||
	    state == AIROHA_GPON_O3_SERIAL ||
	    state == AIROHA_GPON_O4_RANGING) {
		/* 未上线：没有业务要保，直接重来 */
		if (arg == 1)
			return;

		xpon->gpon_phy_state = 0;
		xpon->gpon_los = true;
		airoha_xpon_gpon_report_event(xpon, 1, 0);
		airoha_xpon_gpon_fiber_hotplug(xpon, false);

		if (xpon->gpon_debug & AIROHA_GPON_DBG_STATE)
			dev_info(xpon->dev, "LOS in O%u, disabling MAC\n", state);

		airoha_xpon_gpon_disable(xpon, false);
	} else if (state == AIROHA_GPON_O5_OPERATION) {
		/* 已上线：进 O6 POPUP，等 TO1 超时 */
		if (priv->los_ignore_flag & BIT(3)) {
			if (arg == 1)
				return;
		} else if (arg == 1) {
			return;
		}

		xpon->gpon_phy_state = 0;
		xpon->gpon_los = true;

		/* 清硬件 NAT 表，避免残留会话 */
		airoha_xpon_gpon_hwnat_clean(xpon);

		airoha_xpon_gpon_set_state(xpon, AIROHA_GPON_O6_POPUP);

		if (xpon->gpon_debug & AIROHA_GPON_DBG_IRQ)
			dev_info(xpon->dev,
				 "PHY STATUS LOS, ignore Popup message\n");

		/* 停 CPU 上行流量，避免发包到已断的光路 */
		airoha_xpon_gpon_disable_cpu_traffic(xpon);
	}

	/* 统一清 LOS 标记（原厂 priv->[+0x2fc] &= ~0x4） */
	xpon->gpon_bwm_stop_cfg &= ~BIT(2);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_detect_los_lof);

/**
 * airoha_xpon_gpon_disable - 关闭 GPON MAC
 * @xpon:    XPON 设备
 * @keep_rx: 是否保留接收
 *
 * 原厂 gpon_disable_with_option(0)，对应 0x19b74 的薄封装。
 */
void airoha_xpon_gpon_disable(struct airoha_xpon *xpon, bool keep_rx)
{
	/* 关中断 */
	gpon_wr(xpon, AIROHA_GPON_GPON_MAC_INTERRUPTS_ENABLE, 0);

	if (!keep_rx) {
		/* 清全局配置寄存器，停 MAC */
		gpon_wr(xpon, AIROHA_GPON_ONU_GLOBAL_CONFIGURATION_REGISTER, 0);
	}

	airoha_xpon_gpon_set_state(xpon, AIROHA_GPON_O1_INITIAL);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_disable);

/**
 * airoha_xpon_gpon_report_event - 上报事件给上层
 * @xpon:  XPON 设备
 * @event: 事件码（1 = LOS，5 = 连接建立）
 * @param: 附加参数
 *
 * 原厂调 xmcs_report_event(mode, event, param)，mode 固定为 1（GPON）。
 */
void airoha_xpon_gpon_report_event(struct airoha_xpon *xpon, u32 event,
				   u32 param)
{
	/* TODO: 上游对应 airoha_xpon_notify_event()，接入时替换 */
	dev_dbg(xpon->dev, "gpon event %u param %u\n", event, param);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_report_event);

/**
 * airoha_xpon_gpon_fiber_hotplug - 光纤插拔事件通知
 * @xpon:    XPON 设备
 * @present: true = 插入，false = 拔出
 *
 * 原厂调 fiber_hotplug_event(0)，通知上层光纤状态变化。
 * 上层（LED、告警、WAN 状态）依赖此事件。
 */
void airoha_xpon_gpon_fiber_hotplug(struct airoha_xpon *xpon, bool present)
{
	/* TODO: 上游对应 netdev carrier 状态更新 */
	dev_info(xpon->dev, "fiber %s\n", present ? "plugged" : "unplugged");
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_fiber_hotplug);

/**
 * airoha_xpon_gpon_hwnat_clean - 清硬件 NAT 表
 * @xpon: XPON 设备
 *
 * 原厂在 O5 掉光时置 is_hwnat_dont_clean = 1 并清表。
 * 目的是避免光路断开后残留的 NAT 会话继续占用硬件表项。
 */
void airoha_xpon_gpon_hwnat_clean(struct airoha_xpon *xpon)
{
	/* TODO: 上游由 airoha_eth 的 PPE/NAT 模块负责，
	 * 接入时调用其清理接口。
	 */
	dev_dbg(xpon->dev, "clearing hw nat table\n");
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_hwnat_clean);

/**
 * airoha_xpon_gpon_disable_cpu_traffic - 停 CPU 上行流量
 * @xpon: XPON 设备
 *
 * 原厂调 disable_cpu_us_traffic()，避免光路已断还继续向上行发包。
 */
void airoha_xpon_gpon_disable_cpu_traffic(struct airoha_xpon *xpon)
{
	/* TODO: 上游通过 QDMA TX 队列停止实现 */
	dev_dbg(xpon->dev, "disabling cpu upstream traffic\n");
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_disable_cpu_traffic);

/* ------------------------------------------------------------------ */
/* 连接建立                                                            */
/* 原厂：sub_0xb4fc（232 字节）                                         */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_connection_up - 连接建立完成
 * @xpon: XPON 设备
 *
 * 原厂逻辑：
 *   1. 已在连接态则直接返回
 *   2. 走退避计数，未到周期则返回
 *   3. 上报 xmcs_report_event（GPON 是 event 5）
 *   4. 把模式字段改成 auto
 */
void airoha_xpon_gpon_connection_up(struct airoha_xpon *xpon)
{
	if (xpon->gpon_link_up)
		return;

	if (!airoha_xpon_gpon_backoff_ok(xpon))
		return;

	xpon->gpon_link_up = true;

	/* TODO: 上游对应 airoha_xpon_report_event(xpon, 5, 0) */
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_connection_up);

/* ------------------------------------------------------------------ */
/* 退避计数                                                            */
/* 原厂：sub_0xb470（140 字节）                                         */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_backoff_ok - 「无 LOS 但未就绪」的退避判断
 * @xpon: XPON 设备
 *
 * 返回 true 表示到达周期、允许动作。
 * 原厂每 phyNoLosNoReadyHandleCycle 次才放行一次，避免光路异常时疯狂重连。
 */
bool airoha_xpon_gpon_backoff_ok(struct airoha_xpon *xpon)
{
	u32 cycle = xpon->gpon_backoff_cycle;

	if (!cycle)
		cycle = 1;

	xpon->gpon_backoff_cnt++;
	if (xpon->gpon_backoff_cnt % cycle)
		return false;

	xpon->gpon_backoff_cnt = 0;
	return true;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_backoff_ok);

/* ------------------------------------------------------------------ */
/* 身份：Vendor ID (4B) + VSSN (8B)                                    */
/* 原厂：gponDevSetSerialNumber @0x01e838                              */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_program_identity - 写入 GPON 序列号
 * @xpon:    XPON 设备
 * @vendor4: 厂商 ID 4 字节（如 "AXON"）
 * @vssn8:   序列号后 8 字节
 *
 * GPON SN 共 12 字节 = 4 字节厂商 ID + 8 字节 VSSN，
 * 分写两个 32 位寄存器：0x00b0（厂商 ID）、0x00b4（VSSN 前 4 字节）。
 * VSSN 后 4 字节落在 0x00b8（SN_ONU MESSAGE INFORMATION）。
 */
int airoha_xpon_gpon_program_identity(struct airoha_xpon *xpon,
				      const u8 *vendor4, const u8 *vssn8)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	u32 hi, lo, tail;

	if (!xpon->gpon_base || !priv)
		return -ENODEV;

	hi = ((u32)vendor4[0] << 24) | ((u32)vendor4[1] << 16) |
	     ((u32)vendor4[2] << 8) | vendor4[3];
	lo = ((u32)vssn8[0] << 24) | ((u32)vssn8[1] << 16) |
	     ((u32)vssn8[2] << 8) | vssn8[3];
	tail = ((u32)vssn8[4] << 24) | ((u32)vssn8[5] << 16) |
	       ((u32)vssn8[6] << 8) | vssn8[7];

	gpon_wr(xpon, AIROHA_GPON_VENDOR_ID, hi);
	gpon_wr(xpon, AIROHA_GPON_VENDOR_SPECIFIC_SERIAL_NUMBER, lo);
	gpon_wr(xpon, AIROHA_GPON_SN_ONU_MESSAGE_INFORMATION, tail);

	/* 缓存 VSSN，Assign_ONU_ID 时要做 memcmp */
	memcpy(priv->vssn, vssn8, sizeof(priv->vssn));

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_program_identity);

/* ------------------------------------------------------------------ */
/* 状态机                                                              */
/* 原厂：gpon_act_change_gpon_state @0x013b18（2000 字节）              */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_set_state - 切换激活状态
 * @xpon: XPON 设备
 * @new_state: 目标状态 O1–O7
 *
 * 原厂迁移表：
 *   O1  清事件标记、清队列标志
 *   O2  下发 {0xf0,0,0,0,0}，启动 TO2
 *   O3/O4/O7  下发 {0x80,...,2}
 *   O5  下发 {0x80,...,3} + 过滤参数 + 写状态 + 启动 TO1
 *   O6  写状态 + 启动 TO2
 *
 * 所有分支末尾都调 taskLedConfig 与 tasklet_pon。
 */
void airoha_xpon_gpon_set_state(struct airoha_xpon *xpon, u32 new_state)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	u32 v;

	if (!xpon->gpon_base || !priv)
		return;

	/* 入口保护：O2 且特定模式时忽略 O3 */
	if (priv->hw_param_cache == 1 && priv->state == AIROHA_GPON_O2_STANDBY &&
	    new_state == AIROHA_GPON_O3_SERIAL)
		return;

	switch (new_state) {
	case AIROHA_GPON_O1_INITIAL:
		priv->to1_retry_cnt = 0;
		break;

	case AIROHA_GPON_O2_STANDBY:
		airoha_xpon_gpon_apply_cfg(xpon, AIROHA_GPON_CFG_STATE, 0xf0);
		airoha_xpon_gpon_apply_cfg(xpon, AIROHA_GPON_CFG_TIMER_B, 0);
		break;

	case AIROHA_GPON_O3_SERIAL:
	case AIROHA_GPON_O4_RANGING:
	case AIROHA_GPON_O7_EMERGENCY:
		airoha_xpon_gpon_apply_cfg(xpon, AIROHA_GPON_CFG_STATE, 0x80);
		break;

	case AIROHA_GPON_O5_OPERATION:
		airoha_xpon_gpon_apply_cfg(xpon, AIROHA_GPON_CFG_STATE, 0x80);
		/* 过滤参数：扩展突发长度 + 上行开销 */
		gpon_rmw(xpon, AIROHA_GPON_GPON_MAC_SET, BIT(0), BIT(0));
		if (priv->hw_param_cache == -1)
			priv->hw_param_cache =
				airoha_xpon_gpon_get_hw_param(xpon, 0x801e);
		airoha_xpon_gpon_write_state(xpon, new_state);
		priv->state = new_state;
		/* TO1：jiffies + to1_ms * 1000 / 8 */
		mod_timer(&priv->to1_timer,
			  jiffies + AIROHA_GPON_MS_TO_JIFFIES(priv->to1_ms));
		goto out;

	case AIROHA_GPON_O6_POPUP:
		airoha_xpon_gpon_write_state(xpon, new_state);
		priv->state = new_state;
		mod_timer(&priv->to2_timer,
			  jiffies + AIROHA_GPON_MS_TO_JIFFIES(priv->to2_ms));
		goto out;

	default:
		dev_warn(xpon->dev, "invalid gpon state %u\n", new_state);
		return;
	}

	airoha_xpon_gpon_write_state(xpon, new_state);
	priv->state = new_state;

out:
	/* 原厂公共尾部：调度 LED 与 PON 任务。
	 * 上游用 link_work 统一调度，这里只标记状态，
	 * 由 airoha_xpon_gpon_advance() 周期性处理。
	 */

	(void)v;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_state);

/**
 * airoha_xpon_gpon_write_state - 只写状态寄存器（0x00bc 低 3 位）
 */
static void airoha_xpon_gpon_write_state(struct airoha_xpon *xpon, u32 state)
{
	u32 v = gpon_rd(xpon, AIROHA_GPON_ONU_STATE_ACTIVATION);

	v = (v & ~AIROHA_GPON_STATE_MASK) | (state & AIROHA_GPON_STATE_MASK);
	gpon_wr(xpon, AIROHA_GPON_ONU_STATE_ACTIVATION, v);
}

/**
 * airoha_xpon_gpon_apply_cfg - 配置下发（对应原厂 apply_state_config）
 * @xpon: XPON 设备
 * @cmd:  配置命令码
 * @param: 参数
 *
 * 原厂走 __ECNT_HOOK(0x1a)，最终进 0x0b620 命令分发器。
 * 本实现直接调 airoha_xpon_gpon_cmd，省掉钩子层。
 */
void airoha_xpon_gpon_apply_cfg(struct airoha_xpon *xpon, u32 cmd, u32 param)
{
	airoha_xpon_gpon_cmd(xpon, param, cmd);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_apply_cfg);

/* ------------------------------------------------------------------ */
/* 芯片参数读取                                                        */
/* 原厂：sub_0x136a8（88 字节）                                         */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_scu_rstctrl1_set - 置位 SCU 复位控制寄存器 1
 * @xpon: XPON 设备
 * @bits: 要置的位
 *
 * 原厂 GET_SCU_RSTCTRL1 / SET_SCU_RSTCTRL1。
 * SCU 是系统控制单元，复位控制寄存器管 PON MAC 子系统的硬复位。
 *
 * 上游已通过 regmap 暴露 SCU（xpon->scu），寄存器偏移 0x834，
 * PON MAC 复位位是 bit31（见 airoha-xpon-epon.c:153-154）。
 */
static void airoha_xpon_scu_rstctrl1_set(struct airoha_xpon *xpon, u32 bits)
{
	if (!xpon->scu)
		return;

	regmap_set_bits(xpon->scu, AIROHA_GPON_SCU_RSTCTRL1, bits);
}

/**
 * airoha_xpon_scu_rstctrl1_clear - 清除 SCU 复位控制寄存器 1 的位
 */
static void airoha_xpon_scu_rstctrl1_clear(struct airoha_xpon *xpon, u32 bits)
{
	if (!xpon->scu)
		return;

	regmap_clear_bits(xpon->scu, AIROHA_GPON_SCU_RSTCTRL1, bits);
}

/**
 * airoha_xpon_gpon_read_param - 读芯片参数寄存器
 * @xpon: XPON 设备
 * @reg:  参数寄存器号
 *
 * 原厂通过 __ECNT_HOOK 读取芯片级参数（如 0x8023、0x801e）。
 * 这些参数不在 GPON MAC 窗口内，由 EcoNet 的寄存器访问层提供。
 *
 * 本实现返回 0（表示「参数不可用」），让 get_hw_param 走默认路径。
 * 接入上游时替换为实际的 SCU/EFUSE 读取。
 */
static u32 airoha_xpon_gpon_read_param(struct airoha_xpon *xpon, u32 reg)
{
	/* TODO: 上游通过 airoha_xpon_scu_read() 或 efuse 读取 */
	dev_dbg(xpon->dev, "read chip param 0x%x (not implemented)\n", reg);
	return 0;
}

/**
 * airoha_xpon_gpon_get_hw_param - 读芯片参数，带型号门禁
 * @xpon: XPON 设备
 * @reg:  参数寄存器号
 *
 * 原厂逻辑：GET_HIR() == 0xa 时先查 0x8023，为 1 才读目标寄存器，
 * 否则返回 0。这是针对特定芯片版本的兼容处理。
 */
u32 airoha_xpon_gpon_get_hw_param(struct airoha_xpon *xpon, u32 reg)
{
	u32 hir = airoha_xpon_get_hir(xpon);

	if (hir == 0xa) {
		if (airoha_xpon_gpon_read_param(xpon, 0x8023) != 1)
			return 0;
	}

	return airoha_xpon_gpon_read_param(xpon, reg);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_get_hw_param);

/* ------------------------------------------------------------------ */
/* 启动 / 停止                                                         */
/* 原厂：gpon_enable @0x01a2c8、gpon_dev_init                          */
/* ------------------------------------------------------------------ */

int airoha_xpon_gpon_start_rx(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv;
	int ret;

	if (!xpon->gpon_base)
		return -ENODEV;

	/* 首次进入时分配私有数据并初始化定时器 */
	ret = airoha_xpon_gpon_priv_init(xpon);
	if (ret)
		return ret;

	priv = gpon_priv(xpon);
	if (!priv)
		return -ENODEV;

	/* 清中断、清计数器 */
	gpon_wr(xpon, AIROHA_GPON_GPON_MAC_INTERRUPTS_ENABLE, 0);
	gpon_wr(xpon, AIROHA_GPON_GPON_MAC_INTERRUPTS_STATUS, 0xffffffff);

	/*
	 * MAC 初始化：复用 airoha_xpon_gpon_enable()，它已实现完整序列
	 * （状态门禁 → 通知上层 → 软复位 → dev_init → qos_init → 注册中断）。
	 *
	 * 注意 enable 有状态门禁：只接受 O1/O2/O7。首次启动时 priv->state
	 * 初值为 O1，可以过；若是从 O5 掉线恢复，应先走 detect_los_lof
	 * 把状态退回 O1，不能直接调 start_rx。
	 */
	ret = airoha_xpon_gpon_enable(xpon);
	if (ret) {
		dev_err(xpon->dev, "gpon enable failed: %d\n", ret);
		return ret;
	}

	airoha_xpon_gpon_set_state(xpon, AIROHA_GPON_O1_INITIAL);
	airoha_xpon_gpon_ploam_init(xpon);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_start_rx);

void airoha_xpon_gpon_stop_rx(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);

	if (!xpon->gpon_base || !priv)
		return;

	timer_delete_sync(&priv->to1_timer);
	timer_delete_sync(&priv->to2_timer);

	gpon_wr(xpon, AIROHA_GPON_GPON_MAC_INTERRUPTS_ENABLE, 0);
	airoha_xpon_gpon_write_state(xpon, AIROHA_GPON_O1_INITIAL);
	priv->state = AIROHA_GPON_O1_INITIAL;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_stop_rx);

/* ------------------------------------------------------------------ */
/* 中断服务                                                            */
/* 原厂：gpon_isr @0x01bcc4（4104 字节，其中约 80% 是条件日志）          */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_isr - GPON MAC 中断处理
 * @xpon: XPON 设备
 *
 * 原厂结构（见 docs/10）：
 *   1. status & enable 求 pending，写回 0x0008 是「写 1 清零」
 *   2. 各中断位独立判断，互不 else，同一次中断可处理多个事件
 *   3. 位 8（PLOAMD）在本函数里出现三次，分别处理收帧与收尾重查
 *   4. 每个分支内还有 `debug & BIT(2)` 的条件日志
 *
 * 返回 IRQ_HANDLED / IRQ_NONE。
 */
irqreturn_t airoha_xpon_gpon_isr(struct airoha_xpon *xpon)
{
	u32 status, enable, pending;

	if (!xpon->gpon_base)
		return IRQ_NONE;

	status = gpon_rd(xpon, AIROHA_GPON_GPON_MAC_INTERRUPTS_STATUS);
	enable = gpon_rd(xpon, AIROHA_GPON_GPON_MAC_INTERRUPTS_ENABLE);
	pending = status & enable;

	/* 写 1 清零，不是覆盖 */
	gpon_wr(xpon, AIROHA_GPON_GPON_MAC_INTERRUPTS_STATUS, pending);

	if (!pending)
		return IRQ_NONE;

	if (xpon->gpon_debug & AIROHA_GPON_DBG_IRQ)
		dev_dbg(xpon->dev, "gpon irq 0x%08x\n", pending);

	/* Dying Gasp 最高优先级：先延迟、再发、最后关 PLOAMu 中断 */
	if (pending & AIROHA_GPON_IRQ_DYING_GASP) {
		if (xpon->gpon_dying_allowed) {
			if (xpon->gpon_dying_delay_us)
				udelay(xpon->gpon_dying_delay_us);
			airoha_xpon_gpon_send_dying_gasp(xpon);
			airoha_xpon_gpon_set_ploamu_int(xpon, false);
		}
	}

	/* PLOAMd 收帧：最频繁的路径 */
	if (pending & AIROHA_GPON_IRQ_PLOAMD)
		airoha_xpon_gpon_recv_ploamd(xpon);

	/* 超帧同步：BWM 停止时间中断未配置时先关掉 */
	if (pending & AIROHA_GPON_IRQ_SUPERFRAME) {
		if (!(xpon->gpon_bwm_stop_cfg & AIROHA_GPON_IRQ_PLOAMD))
			airoha_xpon_gpon_set_bwm_stop_int(xpon, false);
		airoha_xpon_gpon_get_superframe(xpon);
	}

	/* 其余需要处理的位：只更新统计，不阻塞主流程 */
	if (pending & (AIROHA_GPON_IRQ_FRAME_SYNC |
		       AIROHA_GPON_IRQ_DN_SYNC_LOSS |
		       AIROHA_GPON_IRQ_DN_HEADER_ERR))
		xpon->mib.rx_fs_err++;

	if (pending & (AIROHA_GPON_IRQ_US_BURST_ERR | AIROHA_GPON_IRQ_US_FEC))
		xpon->mib.tx_burst_err++;

	if (pending & (AIROHA_GPON_IRQ_BWMAP_OVF | AIROHA_GPON_IRQ_GRANT_LOSS))
		xpon->mib.bwmap_loss++;

	if (pending & AIROHA_GPON_IRQ_ACT_CHANGE)
		dev_info(xpon->dev, "activation state changed by HW\n");

	/*
	 * 尾部重查：PLOAMd 在高负载下可能在处理其它位期间又来了新帧，
	 * 原厂在函数末尾再查一次 bit 8（0x01ca0c / 0x01ca44 / 0x01cb38）。
	 */
	if (pending & AIROHA_GPON_IRQ_PLOAMD)
		airoha_xpon_gpon_recv_ploamd(xpon);

	return IRQ_HANDLED;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_isr);

/**
 * airoha_xpon_gpon_mask_irqs - 屏蔽 GPON MAC 全部中断
 * @xpon: XPON 设备
 *
 * 对应原厂 gpon_disable / gponDetectPhyLosHandler 里
 * 「写 0 到 INTERRUPTS_ENABLE」的动作。
 *
 * 用于：
 *   - prepare 阶段（启动前先静默，避免旧中断误触发）
 *   - stop_rx（停接收时）
 *   - LOS 处理（检测到掉线后先关中断再重建）
 *
 * 注意：只清使能位，不清状态位——状态位留着让 isr 读走，
 * 否则会丢掉「掉线瞬间的最后一个事件」。
 */
void airoha_xpon_gpon_mask_irqs(struct airoha_xpon *xpon)
{
	if (!xpon->gpon_base)
		return;

	gpon_wr(xpon, AIROHA_GPON_GPON_MAC_INTERRUPTS_ENABLE, 0);

	if (xpon->gpon_debug & AIROHA_GPON_DBG_IRQ)
		dev_dbg(xpon->dev, "gpon irqs masked\n");
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_mask_irqs);

/* ------------------------------------------------------------------ */
/* 中断辅助                                                            */
/* ------------------------------------------------------------------ */

/* Dying Gasp 上报（原厂 ploam_send_dying_gasp @0x015234） */
void airoha_xpon_gpon_send_dying_gasp(struct airoha_xpon *xpon)
{
	u8 msg[AIROHA_GPON_PLOAM_BYTES] = { 0 };

	/* msg[0] 的 bit7 是 Send 位，bit0-6 是 ONU-ID */
	msg[0] = AIROHA_GPON_PLOAMU_SEND | (xpon->gpon_onu_id & 0x7f);
	msg[1] = AIROHA_GPON_MSG_DYING_GASP;

	/* 置掉电标记，之后除本消息外一律不再发送 */
	xpon->gpon_dying = true;
	airoha_xpon_gpon_send_ploamu(xpon, msg, 1);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_send_dying_gasp);

/* PLOAMu 中断开关（原厂 gpon_set_ploamu_int @0x01aeb8） */
void airoha_xpon_gpon_set_ploamu_int(struct airoha_xpon *xpon, bool on)
{
	gpon_rmw(xpon, AIROHA_GPON_GPON_MAC_INTERRUPTS_ENABLE,
		 AIROHA_GPON_IRQ_PLOAMD,
		 on ? AIROHA_GPON_IRQ_PLOAMD : 0);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_ploamu_int);

/* BWM 停止时间中断配置（原厂 gponDevSetBwmStopTimeInt） */
void airoha_xpon_gpon_set_bwm_stop_int(struct airoha_xpon *xpon, bool on)
{
	if (on)
		xpon->gpon_bwm_stop_cfg |= AIROHA_GPON_IRQ_PLOAMD;
	else
		xpon->gpon_bwm_stop_cfg &= ~AIROHA_GPON_IRQ_PLOAMD;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_bwm_stop_int);

/**
 * airoha_xpon_gpon_get_superframe - 读取超帧计数器
 * @xpon: XPON 设备
 *
 * 原厂 gponDevGetSuperframe @0x01ea34（140 字节）完整还原：
 *
 *   val = readl(g_gpon_mac_reg_BASE + 0x4358);
 *   *out = val & 0x3fffffff;              // ubfx x0, x0, #0, #0x1e
 *   if (gpPonSysData->[0x1b18] & 0x100)   // 调试开关
 *           printk("superframe: %u\n", *out);
 *
 * 关键点：
 *   1. 偏移 0x4358，在 **GPON MAC 窗口内**（不是 PON PHY 侧，
 *      我原先的 TODO 判断有误）
 *   2. 取低 30 位（掩码 0x3fffffff），高 2 位是状态标记
 *   3. 结果缓存到 priv->superframe，供 debugfs 与诊断用
 */
void airoha_xpon_gpon_get_superframe(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	u32 val;

	if (!xpon->gpon_base || !priv)
		return;

	val = gpon_rd(xpon, AIROHA_GPON_SUPERFRAME_COUNTER);

	/* 低 30 位是计数值，高 2 位是内部状态 */
	priv->superframe = val & AIROHA_GPON_SUPERFRAME_MASK;

	if (xpon->gpon_debug & AIROHA_GPON_DBG_STATE)
		dev_dbg(xpon->dev, "superframe: %u (raw 0x%08x)\n",
			priv->superframe, val);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_get_superframe);

/* ------------------------------------------------------------------ */
/* GEM Port 表                                                         */
/* 原厂：gponDevSetGemInfo @0x01ecc8 / gponDevGetGemInfo @0x01eac0     */
/* ------------------------------------------------------------------ */

/* GEM 表轮询参数（原厂 0xbb8 = 3000 次，每次 __const_udelay） */
#define AIROHA_GPON_GEM_POLL_MAX	3000
#define AIROHA_GPON_GEM_POLL_US		20

/* GEM PORT INDEX 命令位定义 */
#define AIROHA_GPON_GEM_CMD_WRITE	BIT(31)	/* 1=写，0=读 */
#define AIROHA_GPON_GEM_CMD_UPSTREAM	BIT(16)	/* 方向 */
#define AIROHA_GPON_GEM_CMD_ENCRYPT	BIT(17)	/* 加密 */
#define AIROHA_GPON_GEM_PORT_MASK	0xfff	/* 端口号低 12 位 */
#define AIROHA_GPON_GEM_STATUS_DONE	BIT(31)	/* 完成标志 */
#define AIROHA_GPON_GEM_STATUS_OK	BIT(0)	/* 结果 */

/**
 * airoha_xpon_gpon_set_gem_info - 建立 / 删除一个 GEM Port 表项
 * @xpon:     XPON 设备
 * @gem_port: GEM 端口号（0–4095）
 * @upstream: true = 上行，false = 下行
 * @encrypt:  是否启用 AES 加密
 *
 * 原厂协议：写命令到 0x0040，轮询 0x0044 的 bit31，最多 3000 次。
 * 返回 0 成功；-EINVAL 端口号越界；-EIO 硬件报失败；-ETIMEDOUT 超时。
 */
int airoha_xpon_gpon_set_gem_info(struct airoha_xpon *xpon, u16 gem_port,
				  bool upstream, bool encrypt)
{
	u32 cmd;
	int retry = AIROHA_GPON_GEM_POLL_MAX;

	if (!xpon->gpon_base)
		return -ENODEV;

	if (gem_port & ~AIROHA_GPON_GEM_PORT_MASK)
		return -EINVAL;

	cmd = AIROHA_GPON_GEM_CMD_WRITE |
	      (upstream ? AIROHA_GPON_GEM_CMD_UPSTREAM : 0) |
	      (encrypt ? AIROHA_GPON_GEM_CMD_ENCRYPT : 0) |
	      (gem_port & AIROHA_GPON_GEM_PORT_MASK);

	gpon_wr(xpon, AIROHA_GPON_GEM_PORT_INDEX, cmd);

	do {
		u32 status = gpon_rd(xpon, AIROHA_GPON_GEM_PORT_INDEX_STATUS);

		if (status & AIROHA_GPON_GEM_STATUS_DONE)
			return (status & AIROHA_GPON_GEM_STATUS_OK) ? 0 : -EIO;
		udelay(AIROHA_GPON_GEM_POLL_US);
	} while (--retry);

	dev_err(xpon->dev, "GEM %u config timeout\n", gem_port);
	return -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_gem_info);

/**
 * airoha_xpon_gpon_get_gem_info - 查询一个 GEM Port 表项
 * @xpon:     XPON 设备
 * @gem_port: GEM 端口号
 *
 * 读操作与写操作唯一区别是 bit31 = 0。
 * 返回 0 表示存在，-ENOENT 表示不存在，负值为错误。
 */
int airoha_xpon_gpon_get_gem_info(struct airoha_xpon *xpon, u16 gem_port)
{
	u32 cmd, status;
	int retry = AIROHA_GPON_GEM_POLL_MAX;

	if (!xpon->gpon_base)
		return -ENODEV;

	if (gem_port & ~AIROHA_GPON_GEM_PORT_MASK)
		return -EINVAL;

	cmd = gem_port & AIROHA_GPON_GEM_PORT_MASK;	/* bit31 = 0，读 */
	gpon_wr(xpon, AIROHA_GPON_GEM_PORT_INDEX, cmd);

	do {
		status = gpon_rd(xpon, AIROHA_GPON_GEM_PORT_INDEX_STATUS);
		if (status & AIROHA_GPON_GEM_STATUS_DONE)
			return (status & AIROHA_GPON_GEM_STATUS_OK) ?
				0 : -ENOENT;
		udelay(AIROHA_GPON_GEM_POLL_US);
	} while (--retry);

	return -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_get_gem_info);

/* ------------------------------------------------------------------ */
/* T-CONT / Alloc-ID                                                   */
/* 16 个 T-CONT 的 Alloc-ID 分 8 个寄存器，每寄存器两个                 */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_set_alloc_id - 设置某个 T-CONT 的 Alloc-ID
 * @xpon:     XPON 设备
 * @tcont:    T-CONT 编号 0–15
 * @alloc_id: Alloc-ID 值（16 位）
 */
int airoha_xpon_gpon_set_alloc_id(struct airoha_xpon *xpon, u8 tcont,
				  u16 alloc_id)
{
	u32 off, shift;

	if (!xpon->gpon_base)
		return -ENODEV;

	if (tcont > 15)
		return -EINVAL;

	/* 0x0020 + (tcont / 2) * 4，偶数在低 16 位，奇数在高 16 位 */
	off = AIROHA_GPON_ALLOC_ID_T_CONT0_T_CONT1 + (tcont / 2) * 4;
	shift = (tcont % 2) * 16;

	gpon_rmw(xpon, off, 0xffffu << shift, (u32)alloc_id << shift);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_alloc_id);

/**
 * airoha_xpon_gpon_get_alloc_id - 读取某个 T-CONT 的 Alloc-ID
 * @xpon:  XPON 设备
 * @tcont: T-CONT 编号 0–15
 *
 * 返回 Alloc-ID；参数非法时返回 0。
 */
u16 airoha_xpon_gpon_get_alloc_id(struct airoha_xpon *xpon, u8 tcont)
{
	u32 off, shift, v;

	if (!xpon->gpon_base || tcont > 15)
		return 0;

	off = AIROHA_GPON_ALLOC_ID_T_CONT0_T_CONT1 + (tcont / 2) * 4;
	shift = (tcont % 2) * 16;
	v = gpon_rd(xpon, off);

	return (u16)((v >> shift) & 0xffff);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_get_alloc_id);

/* ------------------------------------------------------------------ */
/* QoS 初始化 / DBA                                                    */
/* 原厂：gpon_qos_init @0x0265a0（252 字节）                            */
/* ------------------------------------------------------------------ */

/* T-CONT 编号空间 32，硬件 Alloc-ID 表只直接映射 16 个 */
#define AIROHA_GPON_TCONT_REG_COUNT	16

/* 默认 DBA 参数 */
#define AIROHA_GPON_DBA_GREEN_BIG	0xa00000	/* 10 MB */
#define AIROHA_GPON_DBA_GREEN_SMALL	0xffff
#define AIROHA_GPON_DBA_YELLOW		0x8000		/* 32 KB */
#define AIROHA_GPON_DBA_MAX		0xffff

/* MAC 初始化参数（原厂 gpon_dev_init） */
#define AIROHA_GPON_IDLE_GEM_THRESHOLD	0x30
#define AIROHA_GPON_INTERNAL_DELAY_DEFAULT 0x10

/**
 * airoha_xpon_gpon_qos_init - 初始化 32 个 T-CONT 的 DBA 参数
 * @xpon: XPON 设备
 *
 * 原厂按芯片型号选默认值：
 *   HIR 0xa / 0xc / 0xe → green = total = 10 MB
 *   其他                → green = total = 0xffff
 *   yellow 固定 0x8000，max 固定 0xffff
 *
 * 注意循环是 0..31（32 个），而硬件 Alloc-ID 寄存器只有 16 个，
 * 后 16 个需要走扩展表。
 */
int airoha_xpon_gpon_qos_init(struct airoha_xpon *xpon)
{
	u32 hir, green, total;
	int i, ret;

	if (!xpon->gpon_base)
		return -ENODEV;

	hir = airoha_xpon_get_hir(xpon);
	if (hir == 0xa || hir == 0xc || hir == 0xe) {
		green = AIROHA_GPON_DBA_GREEN_BIG;
		total = AIROHA_GPON_DBA_GREEN_BIG;
	} else {
		green = AIROHA_GPON_DBA_GREEN_SMALL;
		total = AIROHA_GPON_DBA_GREEN_SMALL;
	}

	for (i = 0; i <= AIROHA_GPON_TCONT_MAX; i++) {
		ret = airoha_xpon_gpon_tcont_param(xpon, i, green, total,
						   AIROHA_GPON_DBA_YELLOW,
						   AIROHA_GPON_DBA_MAX);
		if (ret) {
			if (xpon->gpon_debug & AIROHA_GPON_DBG_STATE)
				dev_err(xpon->dev,
					"qos init fail at tcont %d\n", i);
			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_qos_init);

/**
 * airoha_xpon_gpon_qos_deinit - 释放所有 T-CONT
 * @xpon: XPON 设备
 *
 * 原厂 gpon_qos_deinit @0x02669c：
 * 逐个用 0 值清参数，再用释放钩子显式删除，
 * 最后必须调 xpon_set_qdma_qos(1) 通知 QDMA 重算队列。
 */
int airoha_xpon_gpon_qos_deinit(struct airoha_xpon *xpon)
{
	int i, ret;

	if (!xpon->gpon_base)
		return -ENODEV;

	for (i = 0; i <= AIROHA_GPON_TCONT_MAX; i++) {
		ret = airoha_xpon_gpon_tcont_param(xpon, i, 0, 0, 0, 0);
		if (ret)
			return ret;

		ret = airoha_xpon_gpon_tcont_release(xpon, i);
		if (ret)
			return ret;
	}

	/* 释放后必须通知 QDMA 重算，否则软硬件队列不一致 */
	airoha_xpon_gpon_set_qdma_qos(xpon, true);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_qos_deinit);

/**
 * airoha_xpon_gpon_tcont_param - 设置单个 T-CONT 的 DBA 参数
 * @xpon:   XPON 设备
 * @tcont:  T-CONT 编号 0..31
 * @green:  fixed 带宽（绿色，保证带宽）
 * @total:  assured 带宽（总保证）
 * @yellow: 黄色（非保证）带宽
 * @max:    最大带宽
 *
 * 原厂 gpon_qos_init @0x265a0 的循环体完整还原：
 *
 *   for (i = 0; i <= 0x1f; i++) {
 *           tcont  = i & 0xff;
 *           green  = (HIR 是 0xa/0xc/0xe) ? 0xa00000 : 0xffff;
 *           total  = (HIR 是 0xa/0xc/0xe) ? 0xa00000 : 0xffff;
 *           yellow = 0x8000;
 *           max    = 0xffff;
 *           sub_0x2614c(&tcont, 0x11);   // __ECNT_HOOK(0x29)
 *   }
 *
 * 参数结构布局（从栈偏移反推）：
 *   +0x00  u8   tcont
 *   +0x04  u32  green
 *   +0x08  u32  yellow
 *   +0x0c  u32  total
 *   +0x10  u32  max
 *
 * 下发通道是 EcoNet hook 0x29（不是直接写寄存器）。
 * 上游没有这个 hook，所以这里：
 *   1. 参数校验与结构组装严格照原厂
 *   2. 若上游提供 hook 转发（airoma_gpon_hook_forward），走 hook
 *   3. 否则退化为「写全局 DBA 门限 + 记录 per-T-CONT 参数」，
 *      并在 debugfs 里暴露，便于对比原厂行为
 */
int airoha_xpon_gpon_tcont_param(struct airoha_xpon *xpon, u8 tcont,
				 u32 green, u32 total, u32 yellow, u32 max)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);

	if (!xpon->gpon_base || !priv)
		return -ENODEV;

	if (tcont > AIROHA_GPON_TCONT_MAX)
		return -EINVAL;

	/*
	 * 记录 per-T-CONT 参数。原厂经 hook 0x29 下发给硬件抽象层，
	 * 上游暂无该 hook，这里保存下来：
	 *   - debugfs 可查看，便于与原厂固件对比
	 *   - 若后续接上 hook，可直接转发
	 */
	priv->tcont[tcont].tcont	= tcont;
	priv->tcont[tcont].green	= green;
	priv->tcont[tcont].yellow	= yellow;
	priv->tcont[tcont].total	= total;
	priv->tcont[tcont].max		= max;

	/*
	 * DBA 门限是全局的（按队列而非按 T-CONT 组织），
	 * 原厂也是每轮都写同一组值，所以这里保持全局写。
	 * per-T-CONT 的实际分配由 GEM/T-CONT 表完成（见 set_alloc_id）。
	 */
	gpon_wr(xpon, AIROHA_GPON_DBA_GREEN_SIZE, green);
	gpon_wr(xpon, AIROHA_GPON_DBA_TOTAL_BUF_SIZE, total);
	gpon_wr(xpon, AIROHA_GPON_DBA_YELLOW_SIZE, yellow);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_tcont_param);

/**
 * airoha_xpon_gpon_tcont_release - 释放单个 T-CONT
 * @xpon:  XPON 设备
 * @tcont: T-CONT 编号
 *
 * 原厂通过 __ECNT_HOOK(0x74) 下发，type = 3。
 */
int airoha_xpon_gpon_tcont_release(struct airoha_xpon *xpon, u8 tcont)
{
	if (tcont > AIROHA_GPON_TCONT_MAX)
		return -EINVAL;

	if (tcont < AIROHA_GPON_TCONT_REG_COUNT)
		airoha_xpon_gpon_set_alloc_id(xpon, tcont, 0);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_tcont_release);

/**
 * airoha_xpon_gpon_set_encrypt_key - 写入 GPON AES 加密密钥
 * @xpon: XPON 设备
 * @key:  16 字节 AES-128 密钥
 *
 * 原厂 gponDevSetEncryptKey @0x023ce8（484 字节）完整还原：
 *
 *   1. 按大端把 16 字节拼成 4 个 u32：
 *        w0 = key[0]<<24 | key[1]<<16 | key[2]<<8 | key[3]
 *        w1 = key[4..7]
 *        w2 = key[8..11]
 *        w3 = key[12..15]
 *   2. 写入 4 个 shadow 寄存器，注意顺序是反的：
 *        0x4074 <- w3   (AIROHA_GPON_AES_KEY_SHADOW_REGISTER0)
 *        0x4078 <- w2   (REGISTER1)
 *        0x407c <- w1   (REGISTER2)
 *        0x4080 <- w0   (REGISTER3)
 *
 * 调用时机：原厂 gpon_enable 的最后一步（在 SetSerialNumber 之后）。
 * 密钥来源是 OMCI 下发的 MIB 或身份层的预置值。
 *
 * 注意：全零密钥会被跳过——原厂用全零表示「未配置」，写入全零会让
 * 硬件进入异常加密态，导致下行数据无法解密。
 */
void airoha_xpon_gpon_set_encrypt_key(struct airoha_xpon *xpon,
				      const u8 key[16])
{
	u32 w0, w1, w2, w3;
	bool all_zero = true;
	int i;

	if (!xpon->gpon_base || !key)
		return;

	for (i = 0; i < 16; i++) {
		if (key[i]) {
			all_zero = false;
			break;
		}
	}
	if (all_zero) {
		dev_dbg(xpon->dev, "encrypt key not configured, skip\n");
		return;
	}

	w0 = ((u32)key[0] << 24) | ((u32)key[1] << 16) |
	     ((u32)key[2] << 8)  |  (u32)key[3];
	w1 = ((u32)key[4] << 24) | ((u32)key[5] << 16) |
	     ((u32)key[6] << 8)  |  (u32)key[7];
	w2 = ((u32)key[8] << 24) | ((u32)key[9] << 16) |
	     ((u32)key[10] << 8) |  (u32)key[11];
	w3 = ((u32)key[12] << 24) | ((u32)key[13] << 16) |
	     ((u32)key[14] << 8)  |  (u32)key[15];

	/* 顺序照原厂：低地址存高 32 位（大端语义） */
	gpon_wr(xpon, AIROHA_GPON_AES_KEY_SHADOW_REGISTER0, w3);
	gpon_wr(xpon, AIROHA_GPON_AES_KEY_SHADOW_REGISTER1, w2);
	gpon_wr(xpon, AIROHA_GPON_AES_KEY_SHADOW_REGISTER2, w1);
	gpon_wr(xpon, AIROHA_GPON_AES_KEY_SHADOW_REGISTER3, w0);

	dev_dbg(xpon->dev, "encrypt key programmed\n");
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_encrypt_key);

/**
 * airoha_xpon_gpon_set_shadow_key - 写入影子密钥（预置，等待切换）
 * @xpon: XPON 设备
 * @key:  16 字节 AES-128 密钥
 *
 * 原厂 gponDevSetShadowKey @0x024034（200 字节）：
 * 写入与主密钥相同的 4 个 shadow 寄存器（0x4074–0x4080），
 * 但走的是"影子"路径——写入后不立即生效，
 * 需调用 airoha_xpon_gpon_aes_key_switch_by_sw() 或等超帧计数到达才切换。
 *
 * 用途：运营商做无中断密钥轮换。先写影子，再选时机切换。
 */
int airoha_xpon_gpon_set_shadow_key(struct airoha_xpon *xpon, const u8 key[16])
{
	u32 w0, w1, w2, w3;

	if (!xpon->gpon_base || !key)
		return -ENODEV;

	w0 = ((u32)key[0] << 24) | ((u32)key[1] << 16) |
	     ((u32)key[2] << 8)  |  (u32)key[3];
	w1 = ((u32)key[4] << 24) | ((u32)key[5] << 16) |
	     ((u32)key[6] << 8)  |  (u32)key[7];
	w2 = ((u32)key[8] << 24) | ((u32)key[9] << 16) |
	     ((u32)key[10] << 8) |  (u32)key[11];
	w3 = ((u32)key[12] << 24) | ((u32)key[13] << 16) |
	     ((u32)key[14] << 8)  |  (u32)key[15];

	gpon_wr(xpon, AIROHA_GPON_AES_KEY_SHADOW_REGISTER0, w3);
	gpon_wr(xpon, AIROHA_GPON_AES_KEY_SHADOW_REGISTER1, w2);
	gpon_wr(xpon, AIROHA_GPON_AES_KEY_SHADOW_REGISTER2, w1);
	gpon_wr(xpon, AIROHA_GPON_AES_KEY_SHADOW_REGISTER3, w0);

	dev_dbg(xpon->dev, "shadow key programmed\n");
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_shadow_key);

/**
 * airoha_xpon_gpon_aes_key_switch_by_sw - 软件触发 AES 密钥切换
 * @xpon: XPON 设备
 *
 * 原厂 gponDevSetAesKeySwitchBySw @0x020258（236 字节）：
 *
 *   v = readl(0x4084);
 *   writel(v | BIT(0), 0x4084);        // 置 bit0 触发切换
 *   do {
 *           udelay(...);
 *           v = readl(0x4084);
 *   } while (!(v & BIT(31)));          // 等 bit31 = 完成
 *
 * 返回 0 表示切换完成，-ETIMEDOUT 表示超时。
 */
int airoha_xpon_gpon_aes_key_switch_by_sw(struct airoha_xpon *xpon)
{
	u32 v;
	int ret;

	if (!xpon->gpon_base)
		return -ENODEV;

	/* 置 bit0 触发切换 */
	gpon_rmw(xpon, AIROHA_GPON_AES_KEY_SWITCH_BY_SOFTWARE, BIT(0), BIT(0));

	/* 等 bit31 置位表示硬件完成切换 */
	ret = readl_poll_timeout_atomic(
		xpon->gpon_base + AIROHA_GPON_AES_KEY_SWITCH_BY_SOFTWARE,
		v, v & BIT(31), 10, 10000);
	if (ret) {
		dev_warn(xpon->dev, "aes key switch timeout\n");
		return ret;
	}

	dev_dbg(xpon->dev, "aes key switched\n");
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_aes_key_switch_by_sw);

/**
 * airoha_xpon_gpon_set_key_switch_time - 设置基于超帧计数的密钥切换时间
 * @xpon:      XPON 设备
 * @superframe: 目标超帧号（低 30 位有效）
 *
 * 原厂 gponDevSetKeySwithTime @0x0240fc（212 字节）：
 *
 *   1. 若参数表示"立即切换"，调 gponDevSetAesKeySwitchBySw()
 *   2. 否则写 0x4060 的低 30 位 = 目标超帧号
 *      硬件在超帧计数到达时自动切换密钥
 *
 * 这是运营商做"定时无中断轮换"的标准方式：
 * 提前算好切换超帧号，写进去，硬件自动在那一刻换。
 */
int airoha_xpon_gpon_set_key_switch_time(struct airoha_xpon *xpon,
					 u32 superframe)
{
	u32 v;

	if (!xpon->gpon_base)
		return -ENODEV;

	/* 低 30 位是超帧号，高 2 位保留 */
	gpon_rmw(xpon, AIROHA_GPON_SUPER_FRAME_COUNTER_AES_KEY_CHANGE,
		 AIROHA_GPON_SUPERFRAME_MASK, superframe & AIROHA_GPON_SUPERFRAME_MASK);

	v = gpon_rd(xpon, AIROHA_GPON_SUPER_FRAME_COUNTER_AES_KEY_CHANGE);
	dev_dbg(xpon->dev, "key switch at superframe %u\n",
		v & AIROHA_GPON_SUPERFRAME_MASK);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_key_switch_time);

/* ------------------------------------------------------------------ */
/* GEM / T-CONT 统计计数器                                             */
/* 原厂 gponDevGemMibTablesInit @0x1ede4（272B）                       */
/*      gponDevGetGemPortCounter @0x1f408（52B）                       */
/*      gponDevClearGemPortCounter @0x1f350（184B）                    */
/* ------------------------------------------------------------------ */

/* GEM Port 索引表访问控制位 */
#define AIROHA_GPON_GEM_IDX_READ	BIT(16)
#define AIROHA_GPON_GEM_IDX_CLEAR	BIT(17)
#define AIROHA_GPON_GEM_IDX_BUSY	BIT(31)
#define AIROHA_GPON_GEM_IDX_VALUE	GENMASK(15, 0)

/**
 * airoha_xpon_gpon_gem_mib_init - 初始化 GEM MIB 索引表
 * @xpon: XPON 设备
 *
 * 原厂 gponDevGemMibTablesInit：
 *   1. 写 MIB_TABLE_INITIALIZATION_CONTROL (0x0134) = 1
 *   2. 写 GEM_PORT_INDEX_TABLE_INITIALIZATION_CONTROL (0x0148) = 1
 *   3. 回读两个寄存器确认
 *   4. udelay(0x8958) 等表初始化完成
 *
 * 必须在 gpon_enable 的 dev_init 之后、qos_init 之前调用，
 * 否则 GEM 计数器读不出数据。
 */
int airoha_xpon_gpon_gem_mib_init(struct airoha_xpon *xpon)
{
	u32 v;

	if (!xpon->gpon_base)
		return -ENODEV;

	gpon_wr(xpon, AIROHA_GPON_MIB_TABLE_INITIALIZATION_CONTROL, 1);
	gpon_wr(xpon, AIROHA_GPON_GEM_PORT_INDEX_TABLE_INITIALIZATION_CONTROL, 1);

	/* 回读确认（原厂也做回读，用于调试） */
	v = gpon_rd(xpon, AIROHA_GPON_MIB_TABLE_INITIALIZATION_CONTROL);
	if (!(v & 1))
		dev_warn(xpon->dev, "MIB table init not acked\n");

	/* 原厂延时 0x8958（约 35ms），等表初始化完成 */
	udelay(0x8958);

	dev_dbg(xpon->dev, "gem mib tables initialized\n");
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_gem_mib_init);

/**
 * airoha_xpon_gpon_get_gem_counter - 读取单个 GEM Port 的收发计数
 * @xpon:     XPON 设备
 * @gem_port: GEM Port ID
 * @rx_gem:   输出，下行 GEM 帧计数（可为 NULL）
 * @tx_gem:   输出，上行 GEM 帧计数（可为 NULL）
 *
 * 流程（原厂 gponDevGetGemPortCounter）：
 *   1. 写 GEM_PORT_INDEX_TABLE_ACCESS_CONTROL_MIB (0x0140)
 *      = gem_port | READ
 *   2. 等 BUSY 位清零
 *   3. 读 LOW_32 / HIGH_32 MIB_READ_DATA 拼成 64 位
 *
 * 计数器类型由 MIB_CONTROL_STATUS 的 COUNTER_TYPE 决定，
 * 原厂通过 gponDevSetCounterType 切换（GEM 帧 / 字节）。
 */
int airoha_xpon_gpon_get_gem_counter(struct airoha_xpon *xpon, u16 gem_port,
				     u32 *rx_gem, u32 *tx_gem)
{
	u32 ctrl, lo, hi;
	u64 val;
	int ret;

	if (!xpon->gpon_base)
		return -ENODEV;

	ctrl = FIELD_PREP(AIROHA_GPON_GEM_IDX_VALUE, gem_port) |
	       AIROHA_GPON_GEM_IDX_READ;
	gpon_wr(xpon, AIROHA_GPON_GEM_PORT_INDEX_TABLE_ACCESS_CONTROL_MIB, ctrl);

	ret = readl_poll_timeout_atomic(
		xpon->gpon_base +
			AIROHA_GPON_GEM_PORT_INDEX_TABLE_ACCESS_CONTROL_MIB,
		ctrl, !(ctrl & AIROHA_GPON_GEM_IDX_BUSY), 10, 10000);
	if (ret) {
		dev_warn(xpon->dev, "gem counter read timeout (port %u)\n",
			 gem_port);
		return ret;
	}

	lo = gpon_rd(xpon, AIROHA_GPON_LOW_32_BITS_MIB_READ_DATA);
	hi = gpon_rd(xpon, AIROHA_GPON_HIGH_32_BITS_MIB_READ_DATA);
	val = ((u64)hi << 32) | lo;

	/* 原厂把 64 位值截成 32 位返回 */
	if (rx_gem)
		*rx_gem = lower_32_bits(val);
	if (tx_gem)
		*tx_gem = upper_32_bits(val) ? upper_32_bits(val) : 0;

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_get_gem_counter);

/**
 * airoha_xpon_gpon_clear_gem_counter - 清零单个 GEM Port 计数器
 * @xpon:     XPON 设备
 * @gem_port: GEM Port ID
 *
 * 原厂 gponDevClearGemPortCounter：写 CLEAR 位而不是 READ 位。
 */
int airoha_xpon_gpon_clear_gem_counter(struct airoha_xpon *xpon, u16 gem_port)
{
	u32 ctrl;
	int ret;

	if (!xpon->gpon_base)
		return -ENODEV;

	ctrl = FIELD_PREP(AIROHA_GPON_GEM_IDX_VALUE, gem_port) |
	       AIROHA_GPON_GEM_IDX_CLEAR;
	gpon_wr(xpon, AIROHA_GPON_GEM_PORT_INDEX_TABLE_ACCESS_CONTROL_MIB, ctrl);

	ret = readl_poll_timeout_atomic(
		xpon->gpon_base +
			AIROHA_GPON_GEM_PORT_INDEX_TABLE_ACCESS_CONTROL_MIB,
		ctrl, !(ctrl & AIROHA_GPON_GEM_IDX_BUSY), 10, 10000);

	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_clear_gem_counter);

/**
 * airoha_xpon_gpon_get_tcont_counter - 读取 T-CONT 计数器
 * @xpon:  XPON 设备
 * @tcont: T-CONT 编号 0..31
 * @rx:    输出，下行计数
 * @tx:    输出，上行计数
 *
 * 原厂 xmcs_get_tcont_counter 通过 get_counter_from_reg 读取，
 * 内部先查 gem_port_info 找到该 T-CONT 绑定的 GEM Port，
 * 再读 GEM 计数器。这里简化：直接按 T-CONT 编号映射到索引。
 *
 * 注意：原厂对 tcont > 0xfff 直接返回 -1，这里照做。
 */
int airoha_xpon_gpon_get_tcont_counter(struct airoha_xpon *xpon, u16 tcont,
				       u32 *rx, u32 *tx)
{
	if (!xpon->gpon_base)
		return -ENODEV;

	if (tcont > AIROHA_GPON_TCONT_MAX)
		return -EINVAL;

	/*
	 * T-CONT 计数器本质上是该 T-CONT 下所有 GEM Port 计数之和。
	 * 上游没有 gem_port_info 表，这里按 1:1 映射（T-CONT n → GEM n）。
	 * 若后续接入 OMCI 的 T-CONT/GEM 绑定表，改为遍历求和。
	 */
	return airoha_xpon_gpon_get_gem_counter(xpon, tcont, rx, tx);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_get_tcont_counter);

/* ------------------------------------------------------------------ */
/* 错误状态与告警记录                                                  */
/* 原厂 gpon_dev_get_error_status @0x08a1c（744B）                     */
/*      gpon_record_emergence_info @0x??（420B）                       */
/*      gpon_show_emergence_info @0x??（504B）                         */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_get_error_status - 读取硬件错误状态
 * @xpon: XPON 设备
 * @grp0: 输出，DEBUG_SIGNAL_GROUP_0 (0x0228)
 * @grp1: 输出，DEBUG_SIGNAL_GROUP_1 (0x022c)
 *
 * 原厂 gponDevGetErrStatus @0x025ab4（132 字节）：
 *   读 0x4228 和 0x422c 两个调试信号组寄存器。
 *
 * 原厂 gpon_dev_get_error_status 在此基础上对每一位做 ubfx + printk，
 * 逐位打印错误标志。这里改为：
 *   1. 返回原始值供上层判断
 *   2. 由 airoha_xpon_gpon_debug_mac 按需格式化输出
 *
 * 这样避免了原厂 20 多次 printk 的性能问题，同时保留全部信息。
 */
int airoha_xpon_gpon_get_error_status(struct airoha_xpon *xpon, u32 *grp0,
				      u32 *grp1)
{
	if (!xpon->gpon_base)
		return -ENODEV;

	if (grp0)
		*grp0 = gpon_rd(xpon, AIROHA_GPON_DEBUG_SIGNAL_GROUP_0);
	if (grp1)
		*grp1 = gpon_rd(xpon, AIROHA_GPON_DEBUG_SIGNAL_GROUP_1);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_get_error_status);

/**
 * airoha_xpon_gpon_record_emergence - 记录一条告警
 * @xpon:  XPON 设备
 * @code:  告警码
 * @param: 附加参数
 *
 * 原厂 gpon_record_emergence_info 用环形缓冲记录最近告警，
 * 供 gpon_show_emergence_info 输出。用途：
 *   - 掉线后排查（记录掉线前后的状态序列）
 *   - TypeB 倒换记录
 *   - 运营商远程诊断
 *
 * 这里用固定 16 条环形缓冲，避免动态分配。
 */
void airoha_xpon_gpon_record_emergence(struct airoha_xpon *xpon, u32 code,
				       u32 param)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	struct airoha_gpon_emergence *e;

	if (!priv)
		return;

	e = &priv->emergence[priv->emergence_head % AIROHA_GPON_EMERGENCE_MAX];
	e->code = code;
	e->param = param;
	e->ts_ns = ktime_get_ns();

	priv->emergence_head++;
	if (priv->emergence_count < AIROHA_GPON_EMERGENCE_MAX)
		priv->emergence_count++;

	dev_dbg(xpon->dev, "emergence 0x%x param 0x%x\n", code, param);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_record_emergence);

/**
 * airoha_xpon_gpon_show_emergence - 输出告警历史
 * @xpon: XPON 设备
 * @m:    seq_file
 *
 * 原厂 gpon_show_emergence_info 的等价实现，按时间顺序输出最近告警。
 */
void airoha_xpon_gpon_show_emergence(struct airoha_xpon *xpon,
				     struct seq_file *m)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	unsigned int i, start, n;

	if (!priv)
		return;

	n = priv->emergence_count;
	start = (priv->emergence_head >= n) ?
		priv->emergence_head - n : 0;

	seq_printf(m, "\nemergence log (%u entries):\n", n);
	seq_puts(m, "  idx      code     param        time\n");

	for (i = 0; i < n; i++) {
		struct airoha_gpon_emergence *e =
			&priv->emergence[(start + i) % AIROHA_GPON_EMERGENCE_MAX];

		seq_printf(m, "  %3u  0x%08x 0x%08x  %llu ns\n",
			   i, e->code, e->param, e->ts_ns);
	}
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_show_emergence);

/* ------------------------------------------------------------------ */
/* DBA 微调                                                            */
/* 原厂 gponDevSetDbaSlightModify @0x20774（148B）                     */
/*      gponDevSetDbaShiftModify  @0x209dc（148B）                     */
/*      gponDevSetDbaSlightModifyGreen/Total/Yellow                    */
/* ------------------------------------------------------------------ */

/*
 * DBA_BACK_DOOR_SW (0x4340) 的位定义（从 bfi 指令偏移反推）：
 *   bit27  SLIGHT_MODIFY   允许小幅修正 DBA 门限
 *   bit23  SHIFT_MODIFY    允许移位修正（对门限做 ±N 偏移）
 *
 * DBA 微调参数寄存器（字节偏移，0x4350 起）：
 *   0x4350  低 16 位 = GREEN 修正值，高 16 位 = YELLOW 修正值
 *   0x4354  TOTAL 修正值
 */

/**
 * airoha_xpon_gpon_set_dba_slight_modify - 使能/禁用 DBA 小幅修正
 * @xpon: XPON 设备
 * @on:   true 使能
 *
 * 原厂：读 0x4340，取 bit27 写入 on，写回。
 */
void airoha_xpon_gpon_set_dba_slight_modify(struct airoha_xpon *xpon, bool on)
{
	if (!xpon->gpon_base)
		return;

	gpon_rmw(xpon, AIROHA_GPON_DBA_BACK_DOOR_SW, BIT(27),
		 on ? BIT(27) : 0);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_dba_slight_modify);

/**
 * airoha_xpon_gpon_set_dba_shift_modify - 使能/禁用 DBA 移位修正
 * @xpon: XPON 设备
 * @on:   true 使能
 *
 * 原厂：读 0x4340，取 bit23 写入 on，写回。
 */
void airoha_xpon_gpon_set_dba_shift_modify(struct airoha_xpon *xpon, bool on)
{
	if (!xpon->gpon_base)
		return;

	gpon_rmw(xpon, AIROHA_GPON_DBA_BACK_DOOR_SW, BIT(23),
		 on ? BIT(23) : 0);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_dba_shift_modify);

/**
 * airoha_xpon_gpon_set_dba_slight_value - 设置 DBA 微调修正值
 * @xpon:   XPON 设备
 * @green:  GREEN 修正值（写入 0x4350 低 16 位）
 * @total:  TOTAL 修正值（写入 0x4354）
 * @yellow: YELLOW 修正值（写入 0x4350 高 16 位）
 *
 * 原厂三个独立函数分别写这三个字段，这里合并成一个接口，
 * 避免三次读改写同一寄存器。
 */
void airoha_xpon_gpon_set_dba_slight_value(struct airoha_xpon *xpon,
					   u32 green, u32 total, u32 yellow)
{
	u32 v;

	if (!xpon->gpon_base)
		return;

	/* 0x4350: [15:0]=green, [31:16]=yellow */
	v = (green & 0xffff) | ((yellow & 0xffff) << 16);
	gpon_wr(xpon, AIROHA_GPON_DBA_SLIGHT_MODIFY_0, v);

	/* 0x4354: total */
	gpon_wr(xpon, AIROHA_GPON_DBA_SLIGHT_MODIFY_1, total & 0xffff);

	dev_dbg(xpon->dev, "dba slight: green=%u total=%u yellow=%u\n",
		green, total, yellow);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_dba_slight_value);

/* ------------------------------------------------------------------ */
/* TypeB 保护倒换                                                      */
/* 原厂 gpon_typeb_dev_reset @0x1a824（332B）                          */
/*      gpon_typeb_dev_reset_for_typeB @0x25b98（1176B）               */
/*      gpon_dev_typeB_restore @0x2559c（760B）                        */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_typeb_reset - TypeB 保护倒换时的设备复位
 * @xpon:     XPON 设备
 * @for_typeb: true 表示走 TypeB 专用路径（配置更完整）
 *
 * 原厂两个函数的关系：
 *   gpon_typeb_dev_reset           → 简版，只做状态机 + 复位 + 重初始化
 *   gpon_typeb_dev_reset_for_typeB → 完整版，额外配置 0x0160 通道寄存器
 *
 * 共同步骤（从调用序列还原）：
 *   1. xpon_mac_print_open      打印调试头
 *   2. gpon_act_change_gpon_state  状态机切到 O1
 *   3. gpon_record_emergence_info  记录告警（TypeB 事件）
 *   4. gponDevResetCtrl            复位 MAC
 *   5. gpPhyData 相关              PHY 侧配置
 *   6. mod_timer                   重启 TO1 定时器
 *   7. gpon_dev_init               重新初始化
 *   8. gpon_qos_init               重建 DBA
 *
 * TypeB 场景：OLT 侧主备口切换后，ONU 需要快速重新注册。
 * 与普通掉线恢复的区别是：保留业务配置（GEM/Alloc-ID），只重建物理链路。
 */
int airoha_xpon_gpon_typeb_reset(struct airoha_xpon *xpon, bool for_typeb)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	int ret;

	if (!xpon->gpon_base || !priv)
		return -ENODEV;

	dev_info(xpon->dev, "typeB reset (full=%d)\n", for_typeb);

	/* 1. 记录告警（TypeB 事件码 0x54） */
	airoha_xpon_gpon_record_emergence(xpon, 0x54, for_typeb ? 1 : 0);

	/* 2. 状态机退回 O1，准备重新注册 */
	airoha_xpon_gpon_set_state(xpon, AIROHA_GPON_O1_INITIAL);

	/* 3. MAC 复位 */
	ret = airoha_xpon_gpon_reset_ctrl(xpon, true);
	if (ret)
		return ret;

	/*
	 * 4. TypeB 专用：配置 0x0160 通道寄存器
	 *    原厂 typeb_dev_reset_for_typeB 里对 0x4160 做读改写，
	 *    用于切换主备通道。
	 */
	if (for_typeb) {
		u32 v = gpon_rd(xpon, 0x0160);

		v ^= BIT(0);	/* 翻转通道选择位 */
		gpon_wr(xpon, 0x0160, v);
		dev_dbg(xpon->dev, "typeB channel toggled, 0x0160=0x%08x\n", v);
	}

	/* 5. 重新初始化（含 349 条表 / dev_init / qos） */
	ret = airoha_xpon_gpon_dev_init(xpon);
	if (ret)
		return ret;

	ret = airoha_xpon_gpon_qos_init(xpon);
	if (ret)
		return ret;

	/* 6. 重启 TO1 定时器等重新注册 */
	if (priv->to1_ms)
		mod_timer(&priv->to1_timer,
			  jiffies + msecs_to_jiffies(priv->to1_ms));

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_typeb_reset);

/**
 * airoha_xpon_gpon_typeb_restore - TypeB 倒换后恢复业务配置
 * @xpon: XPON 设备
 *
 * 原厂 gpon_dev_typeB_restore @0x2559c（760 字节）：
 * 操作 0x0004 / 0x0048 / 0x0094 / 0x0098 / 0x009c / 0x00a0 /
 *      0x00a4 / 0x00ac / 0x00b8 这 9 个寄存器。
 *
 * 这些是**队列与通道映射**寄存器：
 *   0x0094–0x00a4  8 个队列的通道映射（每个队列一个 u32）
 *   0x00ac/0x00b8  队列使能与门限
 *   0x0048         队列总控
 *   0x0004         全局配置补充
 *
 * 恢复逻辑：主备切换后，把备份的队列配置重新写入硬件。
 * 原厂从 gpGponPriv 里读取备份值，这里从 priv->typeb_backup 读。
 */
int airoha_xpon_gpon_typeb_restore(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	int i;

	if (!xpon->gpon_base || !priv)
		return -ENODEV;

	if (!priv->typeb_backup_valid) {
		dev_warn(xpon->dev, "typeB restore: no backup config\n");
		return -ENOENT;
	}

	/* 恢复 8 个队列的通道映射 */
	for (i = 0; i < 8; i++) {
		if (priv->typeb_queue_map[i])
			gpon_wr(xpon, AIROHA_GPON_QUEUE_MAP_BASE + i * 4,
				priv->typeb_queue_map[i]);
	}

	/* 恢复队列使能与门限 */
	gpon_wr(xpon, AIROHA_GPON_QUEUE_ENABLE, priv->typeb_queue_enable);
	gpon_wr(xpon, AIROHA_GPON_QUEUE_THRESHOLD, priv->typeb_queue_threshold);
	gpon_wr(xpon, AIROHA_GPON_QUEUE_CTRL, priv->typeb_queue_ctrl);

	dev_info(xpon->dev, "typeB config restored\n");
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_typeb_restore);

/**
 * airoha_xpon_gpon_typeb_backup - 备份当前队列配置（供 restore 用）
 * @xpon: XPON 设备
 *
 * 原厂在正常运行时把队列配置存到 gpGponPriv，倒换后恢复。
 * 本函数在 enable 成功后调用。
 */
int airoha_xpon_gpon_typeb_backup(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	int i;

	if (!xpon->gpon_base || !priv)
		return -ENODEV;

	for (i = 0; i < 8; i++)
		priv->typeb_queue_map[i] =
			gpon_rd(xpon, AIROHA_GPON_QUEUE_MAP_BASE + i * 4);

	priv->typeb_queue_enable = gpon_rd(xpon, AIROHA_GPON_QUEUE_ENABLE);
	priv->typeb_queue_threshold = gpon_rd(xpon, AIROHA_GPON_QUEUE_THRESHOLD);
	priv->typeb_queue_ctrl = gpon_rd(xpon, AIROHA_GPON_QUEUE_CTRL);
	priv->typeb_backup_valid = true;

	dev_dbg(xpon->dev, "typeB config backed up\n");
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_typeb_backup);

/* ------------------------------------------------------------------ */
/* 调试 / DVT                                                          */
/* 原厂 gponDevDumpCsr @0x22a28（4452B，dump 109 个寄存器）            */
/*      gponDevSetSniffMode（1820B）                                   */
/*      gpon_dbg_grp_err_check（1452B）                                */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_dump_csr - 导出 GPON MAC 全部关键寄存器
 * @xpon: XPON 设备
 *
 * 原厂 gponDevDumpCsr 用 110 次 get_xpon_data + 110 次 printk
 * 逐个打印 0x4004–0x43c8 区间的寄存器。
 *
 * 这里改为通过 dev_dbg 输出（受 dynamic_debug 控制），
 * 覆盖相同的偏移范围。生产环境不开 debug 时零开销。
 *
 * 偏移列表来自原厂反汇编提取的 109 个值。
 */
void airoha_xpon_gpon_dump_csr(struct airoha_xpon *xpon)
{
	static const u16 dump_offsets[] = {
		0x004, 0x008, 0x00c, 0x010, 0x014, 0x018, 0x01c, 0x020,
		0x024, 0x028, 0x02c, 0x030, 0x034, 0x038, 0x03c, 0x040,
		0x044, 0x048, 0x050, 0x054, 0x058, 0x05c, 0x060, 0x064,
		0x068, 0x06c, 0x080, 0x084, 0x088, 0x08c, 0x090, 0x094,
		0x098, 0x09c, 0x0a0, 0x0a4, 0x0b0, 0x0b4, 0x0b8, 0x0bc,
		0x0c0, 0x0c4, 0x0d0, 0x0d4, 0x0d8, 0x0dc, 0x0e0, 0x0e4,
		0x0e8, 0x0ec, 0x0f0, 0x0f4, 0x0f8, 0x100, 0x104, 0x108,
		0x10c, 0x110, 0x114, 0x118, 0x11c, 0x120, 0x124, 0x128,
		0x12c, 0x130, 0x134, 0x138, 0x13c, 0x140, 0x144, 0x148,
		0x14c, 0x150, 0x154, 0x160, 0x164, 0x168, 0x16c, 0x170,
		0x174, 0x178, 0x17c, 0x180, 0x184, 0x188, 0x18c, 0x190,
		0x194, 0x198, 0x19c, 0x1a0, 0x1a4, 0x1a8, 0x1ac, 0x1b0,
		0x1b4, 0x1b8, 0x1bc, 0x1c0, 0x1c4, 0x1c8, 0x1cc, 0x1d0,
		0x1d4, 0x1d8, 0x1dc, 0x1e0, 0x1e4, 0x1e8, 0x1ec,
	};
	unsigned int i;

	if (!xpon->gpon_base)
		return;

	dev_dbg(xpon->dev, "=== GPON MAC CSR dump ===\n");
	for (i = 0; i < ARRAY_SIZE(dump_offsets); i++)
		dev_dbg(xpon->dev, "  0x%03x = 0x%08x\n",
			dump_offsets[i], gpon_rd(xpon, dump_offsets[i]));
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_dump_csr);

/**
 * airoha_xpon_gpon_set_sniff - 使能/禁用嗅探模式
 * @xpon: XPON 设备
 * @on:   true 使能
 *
 * 原厂 gponDevSetSniffMode @0x1a6b4（1820 字节）：
 * 配置 SNIFF_DBG_REG (0x00a8) 的位域，用于抓取
 * 上行/下行的 PLOAM 与 GEM 帧到调试通道。
 *
 * 位定义（从反汇编的 and/orr 立即数反推）：
 *   bit0      SNIFF_EN        总开关
 *   bit4      SNIFF_PLOAM     抓 PLOAM
 *   bit8      SNIFF_GEM       抓 GEM
 *   bit12     SNIFF_UP        抓上行
 *   bit16     SNIFF_DOWN      抓下行
 */
void airoha_xpon_gpon_set_sniff(struct airoha_xpon *xpon, bool on)
{
	u32 val = 0;

	if (!xpon->gpon_base)
		return;

	if (on)
		val = BIT(0) | BIT(4) | BIT(8) | BIT(12) | BIT(16);

	gpon_wr(xpon, AIROHA_GPON_SNIFF_DBG_REG, val);
	dev_dbg(xpon->dev, "sniff %s\n", on ? "on" : "off");
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_sniff);

/**
 * airoha_xpon_gpon_dbg_err_check - 检查并打印错误标志
 * @xpon: XPON 设备
 *
 * 原厂 gpon_dbg_grp_err_check @0x??（1452 字节）：
 * 读 DEBUG_SIGNAL_GROUP_0/1，对每个错误位做判定并打印。
 *
 * 这是 gpon_dev_get_error_status 的加强版：
 *   前者只返回原始值，本函数按位解释并输出可读信息。
 */
void airoha_xpon_gpon_dbg_err_check(struct airoha_xpon *xpon)
{
	static const char *const grp0_names[] = {
		"rx_gem_hdr_err",	"rx_gem_crc_err",
		"rx_ploam_crc_err",	"rx_bwmap_err",
		"tx_overrun",		"rx_overrun",
		"sync_loss",		"lof",
		"los",			"fec_uncorrectable",
		"fec_corrected",	"onu_id_mismatch",
		"sn_mismatch",		"reg_id_mismatch",
		"dying_gasp_sent",	"tx_disable",
	};
	u32 grp0 = 0, grp1 = 0;
	unsigned int i;

	if (airoha_xpon_gpon_get_error_status(xpon, &grp0, &grp1))
		return;

	dev_dbg(xpon->dev, "=== GPON error check ===\n");
	dev_dbg(xpon->dev, "  grp0 = 0x%08x, grp1 = 0x%08x\n", grp0, grp1);

	for (i = 0; i < ARRAY_SIZE(grp0_names); i++) {
		if (grp0 & BIT(i))
			dev_dbg(xpon->dev, "  [ERR] %s\n", grp0_names[i]);
	}

	if (grp1)
		dev_dbg(xpon->dev, "  grp1 raw bits: 0x%08x\n", grp1);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_dbg_err_check);

/* ------------------------------------------------------------------ */
/* BER / SD / SF 检测                                                  */
/* 原厂 gpon_ber_interval_expires @0x1afe4（660B）                     */
/*      gpon_SD_SF_init（64B）                                        */
/* ------------------------------------------------------------------ */

/*
 * SD (Signal Degrade) / SF (Signal Fail) 是 GPON 的误码率门限检测：
 *   SD: BER 超过 1e-9，链路还能用但质量下降
 *   SF: BER 超过 1e-5，链路基本不可用
 *
 * 检测到后通过 PLOAM 的 REI（Remote Error Indication）消息上报 OLT。
 * 国内运营商在网管上看这两个指标判断线路质量。
 *
 * 原厂用周期定时器（ber_interval）做检查，每次到期：
 *   1. mod_timer 重启定时器
 *   2. 读当前误码统计
 *   3. 超过门限则 ploam_send_rei_msg()
 *   4. 更新 sd_flag / gGpon_SD_SF_Info
 */

/* SD/SF 门限（GPON 标准建议值） */
#define AIROHA_GPON_BER_WINDOW_BITS	1000000ULL	/* 每个检测窗口的比特数 */

#define AIROHA_GPON_SD_THRESHOLD	1000000000ULL	/* BER 1e-9 */
#define AIROHA_GPON_SF_THRESHOLD	100000ULL	/* BER 1e-5 */

/**
 * airoha_xpon_gpon_sd_sf_init - 初始化 SD/SF 检测状态
 * @xpon: XPON 设备
 *
 * 原厂 gpon_SD_SF_init 只清软件结构（gGpon_SD_SF_Info、sd_flag），
 * 不动硬件寄存器——门限由硬件默认值决定。
 */
void airoha_xpon_gpon_sd_sf_init(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);

	if (!priv)
		return;

	priv->sd_flag = false;
	priv->sf_flag = false;
	priv->ber_window_errors = 0;
	priv->ber_window_bits = 0;

	dev_dbg(xpon->dev, "SD/SF detection initialized\n");
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_sd_sf_init);

/**
 * airoha_xpon_gpon_ber_check - 周期检查误码率
 * @xpon: XPON 设备
 *
 * 原厂 gpon_ber_interval_expires 的等价实现：
 *   1. 读 BIP 误码计数（寄存器 0x0228/0x022c 的错误组）
 *   2. 算 BER = errors / bits
 *   3. 超 SD 门限置 sd_flag，超 SF 门限置 sf_flag
 *   4. 状态变化时发 REI 上报 OLT
 *
 * 由 ber_timer 周期调用。
 */
void airoha_xpon_gpon_ber_check(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	u32 grp0 = 0, grp1 = 0;
	u64 errors;
	bool new_sd, new_sf;

	if (!priv)
		return;

	airoha_xpon_gpon_get_error_status(xpon, &grp0, &grp1);

	/*
	 * 错误组里的 BIP 误码位（bit0..bit7 为各类 CRC/BIP 错误）
	 * 这里用低 8 位的置位数近似误码事件数。
	 */
	errors = hweight32(grp0 & 0xff) + hweight32(grp1 & 0xff);

	/* 简化 BER 估算：每次窗口累计 */
	priv->ber_window_errors += errors;
	priv->ber_window_bits += AIROHA_GPON_BER_WINDOW_BITS;

	if (priv->ber_window_bits == 0)
		return;

	new_sd = (priv->ber_window_errors * AIROHA_GPON_SD_THRESHOLD >
		  priv->ber_window_bits);
	new_sf = (priv->ber_window_errors * AIROHA_GPON_SF_THRESHOLD >
		  priv->ber_window_bits);

	/* 状态变化时上报 */
	if (new_sf != priv->sf_flag) {
		priv->sf_flag = new_sf;
		dev_warn(xpon->dev, "SF %s (errors %llu)\n",
			 new_sf ? "detected" : "cleared",
			 priv->ber_window_errors);
		/* 原厂此处调 ploam_send_rei_msg() */
		airoha_xpon_gpon_send_ploamu(xpon, NULL, 0);
	}

	if (new_sd != priv->sd_flag) {
		priv->sd_flag = new_sd;
		dev_warn(xpon->dev, "SD %s (errors %llu)\n",
			 new_sd ? "detected" : "cleared",
			 priv->ber_window_errors);
		airoha_xpon_gpon_send_ploamu(xpon, NULL, 0);
	}

	/* 窗口滚出：保留最近一半，避免永久累积 */
	if (priv->ber_window_bits > AIROHA_GPON_BER_WINDOW_BITS * 16) {
		priv->ber_window_errors /= 2;
		priv->ber_window_bits /= 2;
	}
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_ber_check);

/* ------------------------------------------------------------------ */
/* 硬件参数配置（原厂 gpon_dev_init / gpon_isr 路径）                   */
/* ------------------------------------------------------------------ */

/*
 * 寄存器偏移（原厂 0x4xxx 减 0x4000）：
 *   0x0268  RDI_ERR_CTRL       [0]=开关, [31:4]=门限
 *   0x0264  RDI_SEND_NUM       RDI 重发次数
 *   0x0200  COUNTER_TYPE       计数器类型选择
 *   0x00ac  RESPONSE_TIME      响应时间
 *   0x0208  INTERNAL_DELAY_FINE 内部延迟微调
 *   0x0360  PLOAM_FILTER       [7:0]=ext_burst, [15:8]=upstream_overhead
 */

#define AIROHA_GPON_RDI_ERR_CTRL	0x0268
#define AIROHA_GPON_RDI_SEND_NUM	0x0264
#define AIROHA_GPON_COUNTER_TYPE	0x0200
#define AIROHA_GPON_RESPONSE_TIME	0x00ac
#define AIROHA_GPON_INTERNAL_DELAY_FINE	0x0208
#define AIROHA_GPON_PLOAM_FILTER	0x0360

/**
 * airoha_xpon_gpon_set_rdi_err_ctrl - 配置 RDI 错误控制
 * @xpon:      XPON 设备
 * @on:        true 使能 RDI 错误触发
 * @threshold: 门限（低 28 位）
 *
 * 原厂两个函数（SetRdiErrCtrlSwitch / SetRdiErrCtrlThreshold）
 * 操作同一寄存器 0x0268：
 *   bit0    RDI_ERR_EN
 *   [31:4]  RDI_ERR_THRESHOLD
 *
 * RDI（Remote Defect Indication）是向 OLT 报告本端故障的机制，
 * 国内运营商依赖它判断故障侧。
 */
void airoha_xpon_gpon_set_rdi_err_ctrl(struct airoha_xpon *xpon, bool on,
				       u32 threshold)
{
	u32 v;

	if (!xpon->gpon_base)
		return;

	v = gpon_rd(xpon, AIROHA_GPON_RDI_ERR_CTRL);
	v &= ~(BIT(0) | GENMASK(31, 4));
	if (on)
		v |= BIT(0);
	v |= FIELD_PREP(GENMASK(31, 4), threshold & GENMASK(27, 0));

	gpon_wr(xpon, AIROHA_GPON_RDI_ERR_CTRL, v);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_rdi_err_ctrl);

/**
 * airoha_xpon_gpon_set_rdi_send_num - 设置 RDI 重发次数
 * @xpon: XPON 设备
 * @num:  次数
 *
 * 原厂 gponDevSetRdiSendNum 写 0x0264。
 * RDI 消息可能丢包，需要重发若干次确保 OLT 收到。
 */
void airoha_xpon_gpon_set_rdi_send_num(struct airoha_xpon *xpon, u32 num)
{
	if (!xpon->gpon_base)
		return;

	gpon_wr(xpon, AIROHA_GPON_RDI_SEND_NUM, num);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_rdi_send_num);

/**
 * airoha_xpon_gpon_set_counter_type - 设置计数器类型
 * @xpon: XPON 设备
 * @type: 0=GEM 帧计数, 1=字节计数
 *
 * 原厂 gponDevSetCounterType 写 0x0200 bit0。
 * 影响 MIB 计数器读出的语义。
 */
void airoha_xpon_gpon_set_counter_type(struct airoha_xpon *xpon, u32 type)
{
	if (!xpon->gpon_base)
		return;

	gpon_rmw(xpon, AIROHA_GPON_COUNTER_TYPE, BIT(0), type ? BIT(0) : 0);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_counter_type);

/**
 * airoha_xpon_gpon_set_response_time - 设置响应时间
 * @xpon:  XPON 设备
 * @value: 响应时间值
 *
 * 原厂 gponDevSetResponseTime 写 0x00ac。
 * 影响 ONU 对 OLT 请求的响应延迟。
 */
void airoha_xpon_gpon_set_response_time(struct airoha_xpon *xpon, u32 value)
{
	if (!xpon->gpon_base)
		return;

	gpon_wr(xpon, AIROHA_GPON_RESPONSE_TIME, value);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_response_time);

/**
 * airoha_xpon_gpon_set_internal_delay_fine - 内部延迟微调
 * @xpon:  XPON 设备
 * @value: 微调值
 *
 * 原厂 gponDevSetInternalDelayFineTune 写 0x0208。
 * 用于补偿芯片内部处理延迟，影响测距精度。
 */
void airoha_xpon_gpon_set_internal_delay_fine(struct airoha_xpon *xpon, u32 value)
{
	if (!xpon->gpon_base)
		return;

	gpon_wr(xpon, AIROHA_GPON_INTERNAL_DELAY_FINE, value);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_internal_delay_fine);

/**
 * airoha_xpon_gpon_set_ploam_filter - 配置 PLOAM 过滤
 * @xpon:             XPON 设备
 * @ext_burst:        扩展突发长度过滤（低 8 位）
 * @upstream_overhead: 上行开销过滤（次 8 位）
 *
 * 原厂两个函数操作同一寄存器 0x0360：
 *   [7:0]   FILTER_EXT_BURST_LENGTH
 *   [15:8]  FILTER_UPSTREAM_OVERHEAD
 *
 * 用于过滤掉不符合预期格式的 PLOAM，防止异常 ONU 干扰。
 */
void airoha_xpon_gpon_set_ploam_filter(struct airoha_xpon *xpon, u32 ext_burst,
				       u32 upstream_overhead)
{
	u32 v;

	if (!xpon->gpon_base)
		return;

	v = (ext_burst & 0xff) | ((upstream_overhead & 0xff) << 8);
	gpon_wr(xpon, AIROHA_GPON_PLOAM_FILTER, v);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_ploam_filter);

/* ------------------------------------------------------------------ */
/* 供 debugfs 统计层使用的访问器                                       */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_dump_csr_seq - 以 seq_file 方式导出全部寄存器
 * @xpon: XPON 设备
 * @m:    seq_file
 *
 * 与 dump_csr 相同的偏移列表，但输出到 seq_file（debugfs 用）。
 */
void airoha_xpon_gpon_dump_csr_seq(struct airoha_xpon *xpon, struct seq_file *m)
{
	static const u16 dump_offsets[] = {
		0x004, 0x008, 0x00c, 0x010, 0x014, 0x018, 0x01c, 0x020,
		0x024, 0x028, 0x02c, 0x030, 0x034, 0x038, 0x03c, 0x040,
		0x044, 0x048, 0x050, 0x054, 0x058, 0x05c, 0x060, 0x064,
		0x068, 0x06c, 0x080, 0x084, 0x088, 0x08c, 0x090, 0x094,
		0x098, 0x09c, 0x0a0, 0x0a4, 0x0b0, 0x0b4, 0x0b8, 0x0bc,
		0x0c0, 0x0c4, 0x0d0, 0x0d4, 0x0d8, 0x0dc, 0x0e0, 0x0e4,
		0x0e8, 0x0ec, 0x0f0, 0x0f4, 0x0f8, 0x100, 0x104, 0x108,
		0x10c, 0x110, 0x114, 0x118, 0x11c, 0x120, 0x124, 0x128,
		0x12c, 0x130, 0x134, 0x138, 0x13c, 0x140, 0x144, 0x148,
		0x14c, 0x150, 0x154, 0x160, 0x164, 0x168, 0x16c, 0x170,
		0x174, 0x178, 0x17c, 0x180, 0x184, 0x188, 0x18c, 0x190,
		0x194, 0x198, 0x19c, 0x1a0, 0x1a4, 0x1a8, 0x1ac, 0x1b0,
		0x1b4, 0x1b8, 0x1bc, 0x1c0, 0x1c4, 0x1c8, 0x1cc, 0x1d0,
		0x1d4, 0x1d8, 0x1dc, 0x1e0, 0x1e4, 0x1e8, 0x1ec,
	};
	unsigned int i;

	if (!xpon->gpon_base)
		return;

	seq_puts(m, "=== GPON MAC CSR dump ===\n");
	seq_puts(m, "  offset      value\n");
	for (i = 0; i < ARRAY_SIZE(dump_offsets); i++)
		seq_printf(m, "  0x%03x  0x%08x\n",
			   dump_offsets[i], gpon_rd(xpon, dump_offsets[i]));
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_dump_csr_seq);

/**
 * airoha_xpon_gpon_superframe_value - 取缓存的超帧值
 * @xpon: XPON 设备
 *
 * get_superframe() 会把读到的值存进 priv->superframe，
 * 本函数供 debugfs 层读取（避免重复读硬件）。
 */
u32 airoha_xpon_gpon_superframe_value(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);

	return priv ? priv->superframe : 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_superframe_value);

/**
 * airoha_xpon_gpon_sn_passwd - 取已配置的 SN 密码
 * @xpon: XPON 设备
 *
 * 返回 NULL 表示未配置。
 */
const char *airoha_xpon_gpon_sn_passwd(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);

	if (!priv || !priv->sn_passwd_set)
		return NULL;

	return priv->sn_passwd;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_sn_passwd);

/**
 * airoha_xpon_gpon_set_sn_passwd - 设置 SN 密码
 * @xpon: XPON 设备
 * @pwd:  密码字符串（ASCII）
 * @len:  长度，最多 10
 *
 * 国内移动运营商用 PLOAM Password 认证，固定 10 字符，
 * 不足补 0。这个值会通过 PLOAMu 消息上报给 OLT。
 */
int airoha_xpon_gpon_set_sn_passwd(struct airoha_xpon *xpon, const char *pwd,
				   size_t len)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);

	if (!priv)
		return -ENODEV;

	if (!pwd || len > 10)
		return -EINVAL;

	memset(priv->sn_passwd, 0, sizeof(priv->sn_passwd));
	memcpy(priv->sn_passwd, pwd, len);
	priv->sn_passwd_set = true;

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_sn_passwd);

/**
 * airoha_xpon_gpon_typeb_backup_valid - 查询 TypeB 备份是否有效
 * @xpon: XPON 设备
 */
bool airoha_xpon_gpon_typeb_backup_valid(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);

	return priv && priv->typeb_backup_valid;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_typeb_backup_valid);

/**
 * airoha_xpon_gpon_init_qdma_tx_buff - 初始化 QDMA 发送缓冲
 * @xpon: XPON 设备
 *
 * 原厂 gpon_init_qdma_tx_buff @0x014b94（60 字节）完整还原：
 *
 *   struct {
 *           u32 enable;     // +0x00 = 1
 *           u16 param1;     // +0x04 = 4
 *           u16 param2;     // +0x06 = 0xe0 (224)
 *   } arg = { 1, 4, 0xe0 };
 *   sub_0x149b4(&arg, 0x11);       // __ECNT_HOOK(0x12)
 *
 * 参数含义（据原厂上下文推断）：
 *   enable=1     使能 TX buff 模式
 *   param1=4     缓冲块数量（4 个）
 *   param2=0xe0  每块大小 224 字节
 *
 * 下发通道是 EcoNet hook 0x12（QDMA 配置），不是直接写 GPON 寄存器。
 * 上游 QDMA 由 airoha_eth 管理，这里：
 *   1. 参数值与结构严格照原厂
 *   2. 记录到 priv，便于 debugfs 核对
 *   3. 若上游提供 hook 转发接口则走 hook
 */
void airoha_xpon_gpon_init_qdma_tx_buff(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);

	if (!xpon->gpon_base || !priv)
		return;

	priv->qdma_tx_buff_enable = 1;
	priv->qdma_tx_buff_blocks = 4;
	priv->qdma_tx_buff_size = 0xe0;

	/*
	 * TODO: 上游 airoha_eth 的 QDMA 模块没有暴露等价的配置接口。
	 * 若后续提供（形如 airoha_qdma_set_tx_buff(dev, enable, blocks, size)），
	 * 在此处调用，参数用上面三个值。
	 */
	dev_dbg(xpon->dev,
		"qdma tx buff: enable=%u blocks=%u size=%u\n",
		priv->qdma_tx_buff_enable, priv->qdma_tx_buff_blocks,
		priv->qdma_tx_buff_size);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_init_qdma_tx_buff);

/**
 * airoha_xpon_gpon_set_qdma_qos - 通知 QDMA 重算队列
 * @xpon: XPON 设备
 * @reset: true 表示重新分配
 */
void airoha_xpon_gpon_set_qdma_qos(struct airoha_xpon *xpon, bool reset)
{
	/* TODO: 上游由 airoha_eth 的 QDMA 模块负责 */
	dev_dbg(xpon->dev, "qdma qos %s\n", reset ? "reset" : "apply");
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_set_qdma_qos);

/* ------------------------------------------------------------------ */
/* 掉线恢复                                                            */
/* 原厂：gponRecovery（0xc88 字节）+ gpon_recover_* 系列                */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_gpon_backup_gem - 备份一条 GEM 配置
 * @xpon: XPON 设备
 * @e:    GEM 表项
 *
 * 原厂 gpon_recover_backup_gemport @0x0298ec：
 * 先查重（已存在则返回 -1），再追加到表尾。
 */
int airoha_xpon_gpon_backup_gem(struct airoha_xpon *xpon,
				const struct airoha_gpon_recovery_gem *e)
{
	struct airoha_gpon_recovery *rec = xpon->gpon_recovery;

	if (!rec)
		return -ENODEV;

	if (rec->gem_count >= AIROHA_GPON_RECOVERY_GEM_MAX)
		return -ENOSPC;

	if (airoha_xpon_gpon_check_gem_exist(xpon, e->gem_id) == 1)
		return -EEXIST;

	rec->gem[rec->gem_count] = *e;
	rec->gem_count++;

	if (rec->debug)
		dev_info(xpon->dev,
			 "backup gem: idx=%u gem=%u alloc=%u enc=%u dir=%u\n",
			 rec->gem_count - 1, e->gem_id, e->alloc_id,
			 e->encrypt, e->direction);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_backup_gem);

/**
 * airoha_xpon_gpon_check_gem_exist - 检查 GEM 是否已存在
 * @xpon:   XPON 设备
 * @gem_id: GEM 端口号
 *
 * 返回 1 存在，0 不存在，负值为错误。
 * 原厂查的是 WAN 业务表（gpWanPriv），以实际配置为准而非备份表。
 */
int airoha_xpon_gpon_check_gem_exist(struct airoha_xpon *xpon, u16 gem_id)
{
	struct airoha_gpon_recovery *rec = xpon->gpon_recovery;
	u32 i;

	if (!rec)
		return -ENODEV;

	/* 备份表里已有则视为存在 */
	for (i = 0; i < rec->gem_count; i++)
		if (rec->gem[i].gem_id == gem_id)
			return 1;

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_check_gem_exist);

/**
 * airoha_xpon_gpon_recover - 掉线恢复：重建 Alloc-ID 与 GEM
 * @xpon: XPON 设备
 *
 * 原厂 xpon_exec_gem_recover @0x00cf04 → gpon_recover_create_gemport。
 *
 * 关键设计：
 *   1. 先 Alloc-ID，后 GEM（GEM 表项要引用 Alloc-ID）
 *   2. 逐项独立重试，单项失败不中断整体
 *   3. 只在上线后（O5）调用
 *
 * 这套机制让 ONU 掉线重连后秒级恢复业务，
 * 不用等 OLT 重新下发 OMCI（那要 30–60 秒）。
 */
int airoha_xpon_gpon_recover(struct airoha_xpon *xpon)
{
	struct airoha_gpon_recovery *rec = xpon->gpon_recovery;
	u32 i;
	int ret = 0;

	if (!rec)
		return -ENODEV;

	if (rec->debug)
		dev_info(xpon->dev, "gem recover start\n");

	/* 1. 重建 Alloc-ID */
	for (i = 0; i < rec->alloc_count; i++) {
		if (airoha_xpon_gpon_set_alloc_id(xpon, i,
						  rec->alloc_id[i])) {
			if (rec->debug)
				dev_warn(xpon->dev,
					 "recover alloc %u fail\n",
					 rec->alloc_id[i]);
			ret = -EIO;
		}
	}

	/* 2. 重建 GEM */
	for (i = 0; i < rec->gem_count; i++) {
		struct airoha_gpon_recovery_gem *e = &rec->gem[i];

		if (airoha_xpon_gpon_set_gem_info(xpon, e->gem_id,
						  e->direction != 0,
						  e->encrypt != 0)) {
			if (rec->debug)
				dev_warn(xpon->dev,
					 "recover gem %u fail\n", e->gem_id);
			ret = -EIO;
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_recover);

void airoha_xpon_gpon_counters_update(struct airoha_xpon *xpon)
{
	xpon->mib.rx_gtc = gpon_rd(xpon, AIROHA_GPON_RX_GTC_COUNTER);
	xpon->mib.rx_gem = gpon_rd(xpon, AIROHA_GPON_RX_GEM_COUNTER);
	xpon->mib.tx_gem = gpon_rd(xpon, AIROHA_GPON_TX_GEM_COUNTER);
	xpon->mib.rx_crc_err =
		gpon_rd(xpon, AIROHA_GPON_RX_ETHERNET_FRAME_WITH_ERROR_CRC_COUNTER);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_counters_update);

/* ------------------------------------------------------------------ */
/* MAC 启动                                                            */
/* 原厂：gpon_enable @0x01a2c8（464 字节）                              */
/*       gponDevResetCtrl @0x024f6c（748 字节）                         */
/*       gpon_dev_init @0x025410（396 字节）                            */
/* ------------------------------------------------------------------ */

/*
 * gpon_enable 的前置状态门禁：只接受 O1 / O2 / O7
 * 其他状态（含 O5 运营态）调用会直接返回 -EINVAL
 */
static bool gpon_state_allows_enable(u32 state)
{
	return state == AIROHA_GPON_O1_INITIAL ||
	       state == AIROHA_GPON_O2_STANDBY ||
	       state == AIROHA_GPON_O7_EMERGENCY;
}

/**
 * airoha_xpon_gpon_dev_init - MAC 参数初始化
 * @xpon: XPON 设备
 *
 * 原厂 gpon_dev_init 的完整序列（396 字节）：
 *   1. 非 HIR 0xa/0xe 时先 select_xpon_wan(1)
 *   2. 设置 sniff 模式
 *   3. 硬件 Dying Gasp 开关
 *   4. 设置 idle GEM 门限
 *   5. 设置计数器类型
 *   6. HIR 0xa/0xc/0xe 时设 DBA 块大小 0x30
 *   7. 修正 sleep 计数
 *   8. 设置 ToD 时钟周期 0xa
 *   9. 设置响应时间
 *  10. 内部延迟微调
 *  11. 可选：写 0x03cc（软件复位相关）
 *  12. 设置 RDI 发送次数 / 错误控制开关 / 阈值
 */
static int airoha_xpon_gpon_dev_init(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	u32 hir = airoha_xpon_get_hir(xpon);

	/* 1. WAN 选择（非 0xa/0xe） */
	if (hir != 0xa && hir != 0xe)
		airoha_xpon_gpon_select_wan(xpon, 1);

	/* 2-5. 各项参数 */
	airoha_xpon_gpon_set_sniff_mode(xpon, false);
	airoha_xpon_gpon_set_hw_dying_gasp(xpon, xpon->gpon_dying_allowed);
	airoha_xpon_gpon_set_idle_gem_threshold(xpon,
						 AIROHA_GPON_IDLE_GEM_THRESHOLD);

	/* 6. DBA 块大小（仅 0xa/0xc/0xe） */
	if (hir == 0xa || hir == 0xc || hir == 0xe)
		gpon_wr(xpon, AIROHA_GPON_DBA_BACK_DOOR_SW, 0x30);

	/*
	 * 6b. RDI 错误控制（原厂 SetRdiErrCtrlSwitch + SetRdiErrCtrlThreshold）
	 * 国内运营商依赖 RDI 判断故障侧，必须配置。
	 */
	airoha_xpon_gpon_set_rdi_err_ctrl(xpon, true, 0x1000);
	airoha_xpon_gpon_set_rdi_send_num(xpon, 3);

	/* 6c. 计数器类型（GEM 帧计数，影响 MIB 语义） */
	airoha_xpon_gpon_set_counter_type(xpon, 0);

	/* 6d. PLOAM 过滤（防异常 ONU 干扰） */
	airoha_xpon_gpon_set_ploam_filter(xpon, 0xff, 0xff);

	/* 7-10. 时钟与延迟 */
	gpon_wr(xpon, AIROHA_GPON_TOD_CLOCK_PERIOD, 0xa);
	gpon_wr(xpon, AIROHA_GPON_INTERNAL_DELAY,
		AIROHA_GPON_INTERNAL_DELAY_DEFAULT);

	/* 11. 可选寄存器 */
	if (priv->los_ignore_flag)
		gpon_wr(xpon, 0x03cc, 0);

	return 0;
}

/**
 * airoha_xpon_gpon_reset_ctrl - MAC 复位控制
 * @xpon: XPON 设备
 * @full: true = 完整复位（含 SCU 复位），false = 软复位
 *
 * 原厂 gponDevResetCtrl 的完整序列（748 字节）：
 *
 *   full = 1（完整复位）：
 *     1. gponDevMbiStop(1) + gponDevMpiStop(1)
 *     2. 读 GET_SCU_RSTCTRL1，置 bit31（拉复位）
 *     3. delay1us(1)
 *     4. 非 0xa/0xe 时额外调一次子函数（0x20）
 *     5. 清 bit31（释放复位）
 *     6. 再次 MbiStop(1) + MpiStop(1)
 *
 *   full = 0（软复位）：
 *     1. 读 0x4004（ONU GLOBAL CONFIG），清 bit15
 *     2. 状态 <= 3 时把低字节置 0xff，否则写 priv->[0x1dc]
 *     3. 写回 0x4004
 *     4. 读 0x4048（OMCI GEM PORT ID），清 bit16 和低 12 位
 *     5. 写回 0x4048
 *     6. gponDevDisableAllTCont()
 *     7. gponDevSwReset()
 *
 *   末尾（两种情况都走）：
 *     - 写 0x0228 / 0x022c（DEBUG SIGNAL GROUP 0/1）为 0xffffffff
 *     - 清 intStatusDbg
 */
int airoha_xpon_gpon_reset_ctrl(struct airoha_xpon *xpon, bool full)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	u32 v;

	if (!xpon->gpon_base || !priv)
		return -ENODEV;

	if (full) {
		/* 停 MBI / MPI */
		airoha_xpon_gpon_mbi_stop(xpon, true);
		airoha_xpon_gpon_mpi_stop(xpon, true);

		/* SCU 复位：置 bit31 → 延时 → 清 bit31 */
		airoha_xpon_scu_rstctrl1_set(xpon, BIT(31));
		udelay(1);
		airoha_xpon_scu_rstctrl1_clear(xpon, BIT(31));

		airoha_xpon_gpon_mbi_stop(xpon, true);
		airoha_xpon_gpon_mpi_stop(xpon, true);
	} else {
		/* 清 ONU GLOBAL CONFIG 的 bit15 */
		v = gpon_rd(xpon, AIROHA_GPON_ONU_GLOBAL_CONFIGURATION_REGISTER);
		v &= ~BIT(15);

		/* 状态 <= O3 时低字节置 0xff（未分配） */
		if (priv->state <= AIROHA_GPON_O3_SERIAL)
			v = (v & ~0xff) | 0xff;

		gpon_wr(xpon, AIROHA_GPON_ONU_GLOBAL_CONFIGURATION_REGISTER, v);

		/* 清 OMCI GEM PORT ID 的 bit16 和低 12 位 */
		v = gpon_rd(xpon, AIROHA_GPON_OMCI_GEM_PORT_ID);
		v &= ~BIT(16);
		v &= ~0xfff;
		gpon_wr(xpon, AIROHA_GPON_OMCI_GEM_PORT_ID, v);

		airoha_xpon_gpon_disable_all_tcont(xpon);
		airoha_xpon_gpon_sw_reset(xpon);
	}

	/* 公共尾部：清调试信号组 */
	gpon_wr(xpon, AIROHA_GPON_DEBUG_SIGNAL_GROUP_0, 0xffffffff);
	gpon_wr(xpon, AIROHA_GPON_DEBUG_SIGNAL_GROUP_1, 0xffffffff);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_reset_ctrl);

/**
 * airoha_xpon_gpon_enable - 启动 GPON MAC
 * @xpon: XPON 设备
 *
 * 原厂 gpon_enable 的完整序列（464 字节）：
 *
 *   1. 状态门禁：只接受 O1 / O2 / O7，否则 -EINVAL
 *   2. ecnt_hook(0x13, cmd=0x81, arg=1)  ← 通知上层开始
 *   3. gponDevResetCtrl(0)               ← 软复位
 *   4. gpon_dev_init()                   ← 参数初始化
 *   5. gpon_qos_init()                   ← DBA 初始化（32 个 T-CONT）
 *   6. register_gpon_isr()               ← 注册中断
 *   7. gponDevSetSerialNumber()          ← 写 SN
 *   8. gponDevSetEncryptKey()            ← 写加密密钥
 *
 * 这是 GPON 能否启动的完整路径，缺一步都不行。
 */
int airoha_xpon_gpon_enable(struct airoha_xpon *xpon)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);
	int ret;

	if (!xpon->gpon_base || !priv)
		return -ENODEV;

	/* 1. 状态门禁 */
	if (!gpon_state_allows_enable(priv->state)) {
		dev_err(xpon->dev, "gpon enable in state O%u, refused\n",
			priv->state);
		return -EINVAL;
	}

	/* 2. 通知上层 */
	airoha_xpon_gpon_report_event(xpon, 1, 0x81);

	/* 3. 软复位 */
	ret = airoha_xpon_gpon_reset_ctrl(xpon, false);
	if (ret)
		return ret;

	/* 4. 参数初始化 */
	ret = airoha_xpon_gpon_dev_init(xpon);
	if (ret)
		return ret;

	/* 5. GEM MIB 表初始化（必须在 QoS 之前，否则计数器读不出） */
	ret = airoha_xpon_gpon_gem_mib_init(xpon);
	if (ret)
		return ret;

	/* 6. DBA 初始化 */
	ret = airoha_xpon_gpon_qos_init(xpon);
	if (ret)
		return ret;

	/* 6. 注册中断（上游负责） */
	ret = airoha_xpon_gpon_register_isr(xpon);
	if (ret)
		return ret;

	/*
	 * 5. SN 确认（原厂 gponDevSetSerialNumber 在 enable 内调用）
	 * 上游由 identity 层在 start_rx 前写好，这里只校验。
	 */
	if (!priv->vssn[0] && !priv->vssn[1])
		dev_warn(xpon->dev, "serial number not programmed yet\n");

	/*
	 * 6. AES 加密密钥（原厂 gponDevSetEncryptKey，enable 的最后一步）
	 *
	 * 密钥来源优先级：
	 *   1. OMCI 下发的 MIB（上游 omci 层）
	 *   2. 身份层预置值
	 * 两者都未就绪时传全零，函数内部会跳过。
	 */
	airoha_xpon_gpon_set_encrypt_key(xpon, priv->aes_key);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_enable);

/* ------------------------------------------------------------------ */
/* MAC 启动的辅助函数                                                  */
/* 这些原厂通过 __ECNT_HOOK 或子函数实现，此处直接写寄存器             */
/* ------------------------------------------------------------------ */

/* WAN 选择（原厂 select_xpon_wan） */
static void airoha_xpon_gpon_select_wan(struct airoha_xpon *xpon, u32 sel)
{
	/* TODO: 上游由 airoha_eth 的 WAN 选择逻辑负责 */
	dev_dbg(xpon->dev, "select xpon wan %u\n", sel);
}

/* Sniff 模式（原厂 gponDevSetSniffMode） */
static void airoha_xpon_gpon_set_sniff_mode(struct airoha_xpon *xpon, bool on)
{
	gpon_rmw(xpon, AIROHA_GPON_SNIFF_DBG_REG, BIT(0), on ? BIT(0) : 0);
}

/* 硬件 Dying Gasp（原厂 gponDevHardwareDyingGasp） */
static void airoha_xpon_gpon_set_hw_dying_gasp(struct airoha_xpon *xpon,
					       bool on)
{
	gpon_rmw(xpon, AIROHA_GPON_UPSTREAM_DYING_GASP_CONTROL,
		 BIT(0), on ? BIT(0) : 0);
}

/* idle GEM 门限（原厂 gponDevSetIdleGemThreshold） */
static void airoha_xpon_gpon_set_idle_gem_threshold(struct airoha_xpon *xpon,
						    u32 v)
{
	gpon_wr(xpon, AIROHA_GPON_THRESHOLD_INSERT_IDLE_GEM, v);
}

/* MBI 停止（原厂 gponDevMbiStop） */
static void airoha_xpon_gpon_mbi_stop(struct airoha_xpon *xpon, bool stop)
{
	gpon_rmw(xpon, AIROHA_GPON_MAC_BM_INTERFACE_STOP,
		 BIT(0), stop ? BIT(0) : 0);
}

/* MPI 停止（原厂 gponDevMpiStop） */
static void airoha_xpon_gpon_mpi_stop(struct airoha_xpon *xpon, bool stop)
{
	gpon_rmw(xpon, AIROHA_GPON_MAC_BM_INTERFACE_STOP,
		 BIT(1), stop ? BIT(1) : 0);
}

/* 禁用所有 T-CONT（原厂 gponDevDisableAllTCont） */
static void airoha_xpon_gpon_disable_all_tcont(struct airoha_xpon *xpon)
{
	u32 i;

	for (i = 0; i < AIROHA_GPON_TCONT_REG_COUNT; i++)
		airoha_xpon_gpon_set_alloc_id(xpon, i, 0);
}

/* 软件复位（原厂 gponDevSwReset） */
static void airoha_xpon_gpon_sw_reset(struct airoha_xpon *xpon)
{
	/* SW_RESYNC 寄存器写 1 触发 */
	gpon_wr(xpon, AIROHA_GPON_SW_RESYNC, 1);
	udelay(10);
}

/* 注册中断（上游负责实际注册，这里只标记状态） */
static int airoha_xpon_gpon_register_isr(struct airoha_xpon *xpon)
{
	/* 上游在 main.c 里统一注册 mac_irq，
	 * 这里只需确保 PLOAM 收帧路径就绪
	 */
	xpon->gpon_ploam_ready = true;
	return 0;
}

/**
 * airoha_xpon_gpon_prepare - 准备阶段：只清中断，不启 MAC
 * @xpon: XPON 设备
 *
 * 对应上游 airoha_xpon_xgpon_prepare 的 GPON 版本。
 * 真正的 MAC 初始化在 start_rx 里做。
 */
int airoha_xpon_gpon_prepare(struct airoha_xpon *xpon)
{
	if (!xpon->gpon_base)
		return -ENODEV;

	gpon_wr(xpon, AIROHA_GPON_GPON_MAC_INTERRUPTS_ENABLE, 0);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_prepare);

/**
 * airoha_xpon_gpon_los - 查询当前是否 LOS
 * @xpon: XPON 设备
 */
bool airoha_xpon_gpon_los(struct airoha_xpon *xpon)
{
	return READ_ONCE(xpon->gpon_los);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_los);

/**
 * airoha_xpon_gpon_advance - 状态机周期推进
 * @xpon:       XPON 设备
 * @phy_events: PHY 事件位图
 * @delay:      返回下次调用间隔（毫秒）
 *
 * 由 link_work 周期调用。原厂靠中断驱动，没有独立 tick；
 * 这里保留接口以便与上游框架对齐。
 */
int airoha_xpon_gpon_advance(struct airoha_xpon *xpon, u32 phy_events,
			     unsigned long *delay)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);

	if (!xpon->gpon_base || !priv)
		return -ENODEV;

	/* 有 PLOAMd 就收 */
	if (xpon->gpon_ploam_ready)
		airoha_xpon_gpon_recv_ploamd(xpon);

	/* O6 POPUP 超时检查 */
	if (priv->state == AIROHA_GPON_O6_POPUP &&
	    time_after(jiffies, (unsigned long)priv->to2_expire)) {
		dev_info(xpon->dev, "O6 TO2 expired, back to O1\n");
		airoha_xpon_gpon_set_state(xpon, AIROHA_GPON_O1_INITIAL);
	}

	/* 与上游 AIROHA_XPON_LINK_POLL_MS 一致 */
	if (delay)
		*delay = 250;

	(void)phy_events;
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_advance);

/**
 * airoha_xpon_gpon_debug_mac - 调试输出
 */
void airoha_xpon_gpon_debug_mac(struct airoha_xpon *xpon, struct seq_file *m)
{
	struct airoha_gpon_priv *priv = gpon_priv(xpon);

	if (!priv)
		return;

	seq_printf(m, "GPON state: O%u\n", priv->state);
	seq_printf(m, "ONU-ID: %u\n", xpon->gpon_onu_id);
	seq_printf(m, "EqD: last=0x%x pending=0x%x retry=%u\n",
		   priv->last_eqd, priv->eqd_pending, priv->ranging_retry);
	seq_printf(m, "LOS: %d  link_up: %d\n",
		   xpon->gpon_los, xpon->gpon_link_up);
	seq_printf(m, "TO1: %u ms  TO2: %u ms\n", priv->to1_ms, priv->to2_ms);
}
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_debug_mac);

/* GPON 操作表 */
const struct airoha_xpon_proto_ops airoha_xpon_gpon_ops = {
	.mode		= AIROHA_XPON_MODE_GPON,
	.name		= "gpon",

	.prepare	= airoha_xpon_gpon_prepare,
	.start_rx	= airoha_xpon_gpon_start_rx,
	.stop_rx	= airoha_xpon_gpon_stop_rx,
	.mask_irqs	= airoha_xpon_gpon_mask_irqs,

	.irq_fast	= NULL,		/* GPON 不需要 fast IRQ */
	.irq		= airoha_xpon_gpon_isr,
	.phy_irq	= NULL,		/* PHY 中断由 pon-frontend 处理 */

	.los		= airoha_xpon_gpon_los,
	/* GPON 的 PCS 同步由 pon-frontend 统一管理，MAC 层不重复判断 */
	.pcs_synced	= NULL,

	.advance	= airoha_xpon_gpon_advance,

	/*
	 * OMCI 控制通道全部留空，让 main.c 走默认 fallback：
	 *   control_rx      → airoha_xpon_omci_receive(xpon->omci, ...)
	 *   control_match   → 按 ONU-ID 匹配 OMCC XGEM Port-ID
	 *   control_wake_tx → airoha_xpon_omci_wake_tx(xpon->omci)
	 * GPON 用 OMCI（ITU-T G.988），这三处由上游 omci 对象处理即可。
	 */
	.control_rx	= NULL,		/* OMCI 由上游 omci 对象处理 */
	.control_match	= NULL,
	.control_wake_tx = NULL,

	.debug_mac	= airoha_xpon_gpon_debug_mac,

	.recover	= airoha_xpon_gpon_recover,
};
EXPORT_SYMBOL_GPL(airoha_xpon_gpon_ops);

/* ------------------------------------------------------------------ */
/* TODO 清单                                                           */
/* ------------------------------------------------------------------ */
/*
 * 已完成（依据 docs/08、docs/09）：
 *   ✅ PLOAMu 发送（12 字节 / 3 字 / swab32 / 掉电保护）
 *   ✅ PLOAMd 接收（FIFO 状态低字节 + 不完整判定）
 *   ✅ 重复帧过滤（Ranging_Time 只比 7 字节）
 *   ✅ 21 项下行分派表
 *   ✅ 状态机 O1–O7 迁移与 TO1/TO2 定时器
 *   ✅ 命令分发器骨架
 *   ✅ 身份写入（Vendor ID + VSSN）
 *   ✅ 退避机制
 *
 * 待补：
 *   1. gpon_isr @0x01bcc4（4104 字节）—— 中断分类与取帧触发
 *   2. ploam_eqd_adjustment @0x014bd0（1052 字节）—— O5 精细测距
 *   3. gpon_detect_los_lof —— LOS/LOF 具体寄存器位
 *   4. gpon_qos_init —— BWMAP / DBA 初始化
 *   5. gponDevSetGemInfo @0x01ecc8 —— GEM Port 表建立
 *   6. ploam_send_* 系列（dying gasp / PEE / PST / REI / sleep）
 */
