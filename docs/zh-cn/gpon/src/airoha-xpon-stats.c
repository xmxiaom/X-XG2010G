// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha AN7581 xPON —— 统计查询接口（debugfs）
 *
 * 目标位置：openwrt-pon-drivers/airoha-xpon/src/airoha-xpon-stats.c
 *
 * ============================================================
 * 为什么用 debugfs 而不是原厂的 ioctl
 * ============================================================
 *
 * 原厂 xpon.ko 用 ioctl（type=0xd9，nr 1–110）暴露统计查询，
 * 由 gpon_cmd_proc（26756 字节）统一分发。反汇编确认它的
 * 8 个真实功能全是统计查询：
 *   xmcs_get_onu_info / get_gem_counter / get_tcont_counter
 *   xmcs_set_sn_passwd / gponDevGetSuperframe / clear 计数器
 *
 * 本实现选 debugfs，理由：
 *   1. ioctl 需要新增字符设备或 netdev 钩子，侵入性大
 *   2. 上游 netlink family 是 static，无法从外部追加 ops
 *   3. debugfs 与上游 debug_mac（seq_file）风格一致
 *   4. 用户态直接 cat 即可，无需额外工具
 *
 * ============================================================
 * 文件布局（/sys/kernel/debug/airoha-xpon/<ifname>/）
 * ============================================================
 *
 *   gem_stats    所有 GEM Port 收发计数
 *   tcont_stats  所有 T-CONT 计数
 *   onu_info     ONU 综合信息（超帧/错误组/告警历史）
 *   sn_passwd    读写 SN 密码（国内移动 PLOAM Password）
 *   dba_tuning   读写 DBA 微调参数
 *   typeb        TypeB 保护状态与手动触发
 *   dump_csr     导出全部关键寄存器
 */

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "airoha-xpon-private.h"

/* 对外接口（供 proto 层调用） */
int airoha_xpon_stats_create(struct airoha_xpon *xpon);
void airoha_xpon_stats_destroy(struct airoha_xpon *xpon);
#include "airoha-xpon-proto.h"
#include "airoha-xpon-gpon.h"

#define AIROHA_XPON_STATS_DIR	"airoha-xpon"
#define AIROHA_XPON_STATS_MAX	8

/*
 * 实例注册表：把 xpon 指针与它的 debugfs 目录关联起来。
 *
 * 不改上游 struct airoha_xpon（避免侵入），用固定大小数组。
 * 单板最多 1–2 个 PON 实例，8 个足够。
 */
struct airoha_xpon_stats_entry {
	struct airoha_xpon	*xpon;
	struct dentry		*dir;
};

static struct airoha_xpon_stats_entry airoha_xpon_stats_entries[AIROHA_XPON_STATS_MAX];
static DEFINE_MUTEX(airoha_xpon_stats_lock);
static struct dentry *airoha_xpon_stats_root;

/* ------------------------------------------------------------------ */
/* 辅助                                                                */
/* ------------------------------------------------------------------ */

static bool airoha_xpon_stats_is_gpon(struct airoha_xpon *xpon)
{
	const struct airoha_xpon_proto_ops *ops = airoha_xpon_proto(xpon);

	return ops && ops->mode == AIROHA_XPON_MODE_GPON;
}

/* ------------------------------------------------------------------ */
/* gem_stats                                                           */
/* ------------------------------------------------------------------ */

static int airoha_xpon_gem_stats_show(struct seq_file *m, void *v)
{
	struct airoha_xpon *xpon = m->private;
	u32 rx, tx, total_rx = 0, total_tx = 0;
	unsigned int port, active = 0;

	if (!airoha_xpon_stats_is_gpon(xpon)) {
		seq_puts(m, "not in GPON mode\n");
		return 0;
	}

	seq_puts(m, "gem_port      rx_frames      tx_frames\n");
	seq_puts(m, "-------------------------------------\n");

	/*
	 * GEM Port ID 是 12 位（0..4095）。逐个读代价高，
	 * 但 debugfs 是调试接口，可以接受；只输出非零项。
	 */
	for (port = 0; port < 4096; port++) {
		if (airoha_xpon_gpon_get_gem_counter(xpon, port, &rx, &tx))
			continue;
		if (!rx && !tx)
			continue;

		seq_printf(m, "%8u  %12u  %12u\n", port, rx, tx);
		total_rx += rx;
		total_tx += tx;
		active++;
	}

	seq_puts(m, "-------------------------------------\n");
	seq_printf(m, "%8s  %12u  %12u  (%u active)\n",
		   "total", total_rx, total_tx, active);

	return 0;
}

static int airoha_xpon_gem_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, airoha_xpon_gem_stats_show,
			   inode->i_private);
}

static const struct file_operations airoha_xpon_gem_stats_fops = {
	.owner		= THIS_MODULE,
	.open		= airoha_xpon_gem_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ------------------------------------------------------------------ */
/* tcont_stats                                                         */
/* ------------------------------------------------------------------ */

static int airoha_xpon_tcont_stats_show(struct seq_file *m, void *v)
{
	struct airoha_xpon *xpon = m->private;
	u32 rx, tx;
	unsigned int i;

	if (!airoha_xpon_stats_is_gpon(xpon)) {
		seq_puts(m, "not in GPON mode\n");
		return 0;
	}

	seq_puts(m, "tcont         rx_count       tx_count\n");
	seq_puts(m, "-------------------------------------\n");

	for (i = 0; i <= AIROHA_GPON_TCONT_MAX; i++) {
		if (airoha_xpon_gpon_get_tcont_counter(xpon, i, &rx, &tx))
			continue;
		seq_printf(m, "%5u  %12u  %12u\n", i, rx, tx);
	}

	return 0;
}

static int airoha_xpon_tcont_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, airoha_xpon_tcont_stats_show,
			   inode->i_private);
}

static const struct file_operations airoha_xpon_tcont_stats_fops = {
	.owner		= THIS_MODULE,
	.open		= airoha_xpon_tcont_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ------------------------------------------------------------------ */
/* onu_info                                                            */
/* ------------------------------------------------------------------ */

static int airoha_xpon_onu_info_show(struct seq_file *m, void *v)
{
	struct airoha_xpon *xpon = m->private;
	u32 grp0 = 0, grp1 = 0;

	if (!airoha_xpon_stats_is_gpon(xpon)) {
		seq_puts(m, "not in GPON mode\n");
		return 0;
	}

	seq_puts(m, "=== ONU info ===\n");

	/* 上游已有的 MAC 状态输出 */
	airoha_xpon_gpon_debug_mac(xpon, m);

	airoha_xpon_gpon_get_superframe(xpon);
	seq_printf(m, "\nsuperframe  : %u\n",
		   airoha_xpon_gpon_superframe_value(xpon));

	airoha_xpon_gpon_get_error_status(xpon, &grp0, &grp1);
	seq_printf(m, "error grp0  : 0x%08x\n", grp0);
	seq_printf(m, "error grp1  : 0x%08x\n", grp1);

	/* 告警历史（原厂 gpon_show_emergence_info） */
	airoha_xpon_gpon_show_emergence(xpon, m);

	return 0;
}

static int airoha_xpon_onu_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, airoha_xpon_onu_info_show, inode->i_private);
}

static const struct file_operations airoha_xpon_onu_info_fops = {
	.owner		= THIS_MODULE,
	.open		= airoha_xpon_onu_info_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ------------------------------------------------------------------ */
/* sn_passwd                                                           */
/* ------------------------------------------------------------------ */

static int airoha_xpon_sn_passwd_show(struct seq_file *m, void *v)
{
	struct airoha_xpon *xpon = m->private;
	const char *pwd = airoha_xpon_gpon_sn_passwd(xpon);

	if (!airoha_xpon_stats_is_gpon(xpon)) {
		seq_puts(m, "not in GPON mode\n");
		return 0;
	}

	if (pwd)
		seq_printf(m, "%s\n", pwd);
	else
		seq_puts(m, "(not set)\n");

	return 0;
}

static int airoha_xpon_sn_passwd_open(struct inode *inode, struct file *file)
{
	return single_open(file, airoha_xpon_sn_passwd_show, inode->i_private);
}

static ssize_t airoha_xpon_sn_passwd_write(struct file *file,
					   const char __user *ubuf,
					   size_t len, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct airoha_xpon *xpon = m->private;
	char buf[16];
	size_t n;
	int ret;

	if (!airoha_xpon_stats_is_gpon(xpon))
		return -EOPNOTSUPP;

	if (len >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;

	buf[len] = '\0';

	/* 去掉尾部换行 */
	n = strnlen(buf, sizeof(buf));
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';

	/*
	 * 国内 PLOAM Password 固定 10 字符（ASCII），不足补 0。
	 * 移动运营商要求严格格式，超长直接拒绝。
	 */
	if (n > 10)
		return -EINVAL;

	ret = airoha_xpon_gpon_set_sn_passwd(xpon, buf, n);
	if (ret)
		return ret;

	dev_info(xpon->dev, "SN password set (%zu chars)\n", n);
	return len;
}

static const struct file_operations airoha_xpon_sn_passwd_fops = {
	.owner		= THIS_MODULE,
	.open		= airoha_xpon_sn_passwd_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= airoha_xpon_sn_passwd_write,
};

/* ------------------------------------------------------------------ */
/* dba_tuning                                                          */
/* ------------------------------------------------------------------ */

static int airoha_xpon_dba_tuning_show(struct seq_file *m, void *v)
{
	struct airoha_xpon *xpon = m->private;

	if (!airoha_xpon_stats_is_gpon(xpon)) {
		seq_puts(m, "not in GPON mode\n");
		return 0;
	}

	seq_puts(m, "=== DBA tuning ===\n");
	seq_puts(m, "commands:\n");
	seq_puts(m, "  echo slight 0|1   > dba_tuning\n");
	seq_puts(m, "  echo shift  0|1   > dba_tuning\n");
	seq_puts(m, "  echo value <green> <total> <yellow> > dba_tuning\n");

	return 0;
}

static int airoha_xpon_dba_tuning_open(struct inode *inode, struct file *file)
{
	return single_open(file, airoha_xpon_dba_tuning_show, inode->i_private);
}

static ssize_t airoha_xpon_dba_tuning_write(struct file *file,
					    const char __user *ubuf,
					    size_t len, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct airoha_xpon *xpon = m->private;
	char buf[64];
	char cmd[16];
	unsigned int a = 0, b = 0, c = 0;
	size_t n;

	if (!airoha_xpon_stats_is_gpon(xpon))
		return -EOPNOTSUPP;

	if (len >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;

	buf[len] = '\0';
	n = strnlen(buf, sizeof(buf));
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';

	if (sscanf(buf, "%15s %u %u %u", cmd, &a, &b, &c) < 2)
		return -EINVAL;

	if (!strcmp(cmd, "slight")) {
		airoha_xpon_gpon_set_dba_slight_modify(xpon, a != 0);
	} else if (!strcmp(cmd, "shift")) {
		airoha_xpon_gpon_set_dba_shift_modify(xpon, a != 0);
	} else if (!strcmp(cmd, "value")) {
		airoha_xpon_gpon_set_dba_slight_value(xpon, a, b, c);
	} else {
		return -EINVAL;
	}

	return len;
}

static const struct file_operations airoha_xpon_dba_tuning_fops = {
	.owner		= THIS_MODULE,
	.open		= airoha_xpon_dba_tuning_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= airoha_xpon_dba_tuning_write,
};

/* ------------------------------------------------------------------ */
/* typeb                                                               */
/* ------------------------------------------------------------------ */

static int airoha_xpon_typeb_show(struct seq_file *m, void *v)
{
	struct airoha_xpon *xpon = m->private;

	if (!airoha_xpon_stats_is_gpon(xpon)) {
		seq_puts(m, "not in GPON mode\n");
		return 0;
	}

	seq_puts(m, "=== TypeB protection ===\n");
	seq_printf(m, "backup_valid: %d\n",
		   airoha_xpon_gpon_typeb_backup_valid(xpon));

	seq_puts(m, "\ncommands:\n");
	seq_puts(m, "  echo backup      > typeb   备份当前队列配置\n");
	seq_puts(m, "  echo restore     > typeb   倒换后恢复配置\n");
	seq_puts(m, "  echo reset       > typeb   简版复位\n");
	seq_puts(m, "  echo reset_full  > typeb   完整复位（TypeB 专用）\n");

	return 0;
}

static int airoha_xpon_typeb_open(struct inode *inode, struct file *file)
{
	return single_open(file, airoha_xpon_typeb_show, inode->i_private);
}

static ssize_t airoha_xpon_typeb_write(struct file *file,
				       const char __user *ubuf,
				       size_t len, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct airoha_xpon *xpon = m->private;
	char buf[24];
	size_t n;

	if (!airoha_xpon_stats_is_gpon(xpon))
		return -EOPNOTSUPP;

	if (len >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;

	buf[len] = '\0';
	n = strnlen(buf, sizeof(buf));
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';

	if (!strcmp(buf, "backup"))
		airoha_xpon_gpon_typeb_backup(xpon);
	else if (!strcmp(buf, "restore"))
		airoha_xpon_gpon_typeb_restore(xpon);
	else if (!strcmp(buf, "reset"))
		airoha_xpon_gpon_typeb_reset(xpon, false);
	else if (!strcmp(buf, "reset_full"))
		airoha_xpon_gpon_typeb_reset(xpon, true);
	else
		return -EINVAL;

	return len;
}

static const struct file_operations airoha_xpon_typeb_fops = {
	.owner		= THIS_MODULE,
	.open		= airoha_xpon_typeb_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= airoha_xpon_typeb_write,
};

/* ------------------------------------------------------------------ */
/* dump_csr                                                            */
/* ------------------------------------------------------------------ */

static int airoha_xpon_dump_csr_show(struct seq_file *m, void *v)
{
	struct airoha_xpon *xpon = m->private;

	if (!airoha_xpon_stats_is_gpon(xpon)) {
		seq_puts(m, "not in GPON mode\n");
		return 0;
	}

	/* 原厂 gponDevDumpCsr 的 109 个寄存器，走 seq_file 输出 */
	airoha_xpon_gpon_dump_csr_seq(xpon, m);

	return 0;
}

static int airoha_xpon_dump_csr_open(struct inode *inode, struct file *file)
{
	return single_open(file, airoha_xpon_dump_csr_show, inode->i_private);
}

static const struct file_operations airoha_xpon_dump_csr_fops = {
	.owner		= THIS_MODULE,
	.open		= airoha_xpon_dump_csr_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ------------------------------------------------------------------ */
/* 目录创建 / 销毁                                                     */
/* ------------------------------------------------------------------ */

/**
 * airoha_xpon_stats_create - 为单个 XPON 实例创建 debugfs 目录
 * @xpon: XPON 设备
 *
 * 由 probe 成功后调用。目录名用 netdev 名（如 pon0）区分多实例。
 */
int airoha_xpon_stats_create(struct airoha_xpon *xpon)
{
	struct dentry *dir;
	int slot = -1;
	int i, ret = 0;

	if (!xpon || !xpon->dev)
		return -EINVAL;

	mutex_lock(&airoha_xpon_stats_lock);

	/* 找一个空槽 */
	for (i = 0; i < AIROHA_XPON_STATS_MAX; i++) {
		if (!airoha_xpon_stats_entries[i].xpon) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		mutex_unlock(&airoha_xpon_stats_lock);
		return -ENOSPC;
	}

	if (!airoha_xpon_stats_root) {
		airoha_xpon_stats_root =
			debugfs_create_dir(AIROHA_XPON_STATS_DIR, NULL);
		if (IS_ERR(airoha_xpon_stats_root)) {
			ret = PTR_ERR(airoha_xpon_stats_root);
			airoha_xpon_stats_root = NULL;
			mutex_unlock(&airoha_xpon_stats_lock);
			return ret;
		}
	}

	dir = debugfs_create_dir(xpon->pon_netdev ?
				 netdev_name(xpon->pon_netdev) : "xpon",
				 airoha_xpon_stats_root);
	if (IS_ERR(dir)) {
		ret = PTR_ERR(dir);
		mutex_unlock(&airoha_xpon_stats_lock);
		return ret;
	}

	airoha_xpon_stats_entries[slot].xpon = xpon;
	airoha_xpon_stats_entries[slot].dir = dir;

	debugfs_create_file("gem_stats", 0444, dir, xpon,
			    &airoha_xpon_gem_stats_fops);
	debugfs_create_file("tcont_stats", 0444, dir, xpon,
			    &airoha_xpon_tcont_stats_fops);
	debugfs_create_file("onu_info", 0444, dir, xpon,
			    &airoha_xpon_onu_info_fops);
	debugfs_create_file("sn_passwd", 0644, dir, xpon,
			    &airoha_xpon_sn_passwd_fops);
	debugfs_create_file("dba_tuning", 0644, dir, xpon,
			    &airoha_xpon_dba_tuning_fops);
	debugfs_create_file("typeb", 0644, dir, xpon,
			    &airoha_xpon_typeb_fops);
	debugfs_create_file("dump_csr", 0444, dir, xpon,
			    &airoha_xpon_dump_csr_fops);

	mutex_unlock(&airoha_xpon_stats_lock);

	dev_info(xpon->dev, "debugfs stats created\n");
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_stats_create);

/**
 * airoha_xpon_stats_destroy - 销毁 debugfs 目录
 * @xpon: XPON 设备
 */
void airoha_xpon_stats_destroy(struct airoha_xpon *xpon)
{
	int i;

	if (!xpon)
		return;

	mutex_lock(&airoha_xpon_stats_lock);
	for (i = 0; i < AIROHA_XPON_STATS_MAX; i++) {
		if (airoha_xpon_stats_entries[i].xpon == xpon) {
			debugfs_remove_recursive(airoha_xpon_stats_entries[i].dir);
			airoha_xpon_stats_entries[i].xpon = NULL;
			airoha_xpon_stats_entries[i].dir = NULL;
			break;
		}
	}
	mutex_unlock(&airoha_xpon_stats_lock);
}
EXPORT_SYMBOL_GPL(airoha_xpon_stats_destroy);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Airoha xPON statistics via debugfs");
