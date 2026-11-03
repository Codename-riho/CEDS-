// SPDX-License-Identifier: GPL-2.0
/*
 * ceds_lkm - Cryo Enhance Deep Sleep sebagai Loadable Kernel Module.
 *
 * Menerapkan dua tuning VM hemat daya lewat antarmuka procfs:
 *
 *   /proc/sys/vm/stat_interval              (detik)
 *   /proc/sys/vm/dirty_writeback_centisecs  (centisecond)
 *
 * Kenapa lewat procfs, bukan menulis variabel kernel langsung?
 * sysctl_stat_interval dan dirty_writeback_interval tidak di-EXPORT, jadi
 * LKM tidak bisa me-link-nya. Jalur procfs sama persis dengan yang dipakai
 * `sysctl -w`, tidak bergantung pada kallsyms, dan lintas versi kernel
 * (butuh kernel_read/kernel_write dengan signature 4.14+).
 *
 * Alur: saat dimuat, modul menjadwalkan `passes` kali penegasan dengan jeda
 * `delay_sec` yang digandakan di tiap pass. Tidak ada daemon dan tidak ada
 * loop tak berujung. Nilai asli dicatat sebelum ditimpa pertama kali dan
 * dikembalikan saat `rmmod` (kecuali restore=0).
 *
 * Parameter (isi saat insmod):
 *   delay_sec  jeda pass pertama, 0..3600      (bawaan 10)
 *   passes     jumlah pass, 1..8               (bawaan 2)
 *   stat_sec   vm.stat_interval, 1..600        (bawaan 5)
 *   wb_cs      dirty_writeback_centisecs, 1..60000 (bawaan 1500)
 * Parameter runtime (/sys/module/ceds_lkm/parameters/):
 *   enable     1/0  hidupkan/matikan penegasan
 *   restore    1/0  kembalikan nilai asli saat rmmod
 *   apply      tulis 1 untuk menegaskan sekarang
 *
 * Log: dmesg | grep ceds_lkm. Baris "A -> B" pada pass >= 2 atau pada
 * pemicu manual berarti nilainya DITIMPA pihak lain sejak penegasan sebelumnya.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>

#ifndef CEDS_PROC_DIR
#define CEDS_PROC_DIR "/proc/sys/vm"
#endif

static unsigned int delay_sec = 10;
module_param(delay_sec, uint, 0444);
MODULE_PARM_DESC(delay_sec, "jeda pass pertama dalam detik (0..3600)");

static unsigned int passes = 2;
module_param(passes, uint, 0444);
MODULE_PARM_DESC(passes, "jumlah pass penegasan (1..8)");

static unsigned int stat_sec = 5;
module_param(stat_sec, uint, 0444);
MODULE_PARM_DESC(stat_sec, "target vm.stat_interval dalam detik (1..600)");

static unsigned int wb_cs = 1500;
module_param(wb_cs, uint, 0444);
MODULE_PARM_DESC(wb_cs, "target vm.dirty_writeback_centisecs (1..60000)");

static bool enable = true;
module_param(enable, bool, 0644);
MODULE_PARM_DESC(enable, "hidupkan penegasan");

static bool restore = true;
module_param(restore, bool, 0644);
MODULE_PARM_DESC(restore, "kembalikan nilai asli saat rmmod");

struct ceds_target {
	const char *path;
	const char *label;
	unsigned long want;
	unsigned long orig;
	bool have_orig;		/* nilai asli sudah dicatat */
	bool touched;		/* kita pernah menulis ke sini */
};

static struct ceds_target targets[] = {
	{ .path = CEDS_PROC_DIR "/stat_interval",
	  .label = "vm.stat_interval" },
	{ .path = CEDS_PROC_DIR "/dirty_writeback_centisecs",
	  .label = "vm.dirty_writeback_centisecs" },
};

static DEFINE_MUTEX(ceds_lock);
static unsigned int ceds_pass;

static int ceds_read(const char *path, unsigned long *val)
{
	struct file *f;
	char buf[32];
	loff_t pos = 0;
	ssize_t n;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	n = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);
	if (n < 0)
		return (int)n;
	if (n == 0)
		return -EIO;
	buf[n] = '\0';
	return kstrtoul(strim(buf), 10, val);
}

static int ceds_write(const char *path, unsigned long val)
{
	struct file *f;
	char buf[32];
	loff_t pos = 0;
	ssize_t n;
	int len;

	len = scnprintf(buf, sizeof(buf), "%lu\n", val);
	f = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	n = kernel_write(f, buf, len, &pos);
	filp_close(f, NULL);
	return n < 0 ? (int)n : 0;
}

/* n == 0: pemicu manual; n >= 1: pass otomatis ke-n */
static void ceds_apply(unsigned int n)
{
	size_t i;

	mutex_lock(&ceds_lock);
	if (!READ_ONCE(enable)) {
		pr_info("#%u: dinonaktifkan, dilewati\n", n);
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(targets); i++) {
		struct ceds_target *t = &targets[i];
		unsigned long cur;
		int ret;

		ret = ceds_read(t->path, &cur);
		if (ret) {
			pr_warn("#%u: gagal baca %s (%d)\n", n, t->path, ret);
			continue;
		}
		if (!t->have_orig) {
			t->orig = cur;
			t->have_orig = true;
		}
		if (cur == t->want) {
			pr_info("#%u: %s ok (%lu)\n", n, t->label, cur);
			continue;
		}
		ret = ceds_write(t->path, t->want);
		if (ret) {
			pr_warn("#%u: gagal tulis %s (%d)\n", n, t->path, ret);
			continue;
		}
		t->touched = true;
		pr_info("#%u: %s %lu -> %lu\n", n, t->label, cur, t->want);
	}
out:
	mutex_unlock(&ceds_lock);
}

static void ceds_work_fn(struct work_struct *work)
{
	unsigned long base = delay_sec ? delay_sec : 1;

	ceds_apply(++ceds_pass);
	if (ceds_pass < passes)
		queue_delayed_work(system_power_efficient_wq,
				   to_delayed_work(work),
				   (base * HZ) << ceds_pass);
}
static DECLARE_DELAYED_WORK(ceds_work, ceds_work_fn);

static int ceds_apply_set(const char *val, const struct kernel_param *kp)
{
	ceds_apply(0);
	return 0;
}
static const struct kernel_param_ops ceds_apply_ops = {
	.set = ceds_apply_set,
};
module_param_cb(apply, &ceds_apply_ops, NULL, 0200);

static int __init ceds_init(void)
{
	if (delay_sec > 3600 || !passes || passes > 8 ||
	    !stat_sec || stat_sec > 600 || !wb_cs || wb_cs > 60000) {
		pr_err("parameter di luar rentang (delay_sec=%u passes=%u stat_sec=%u wb_cs=%u)\n",
		       delay_sec, passes, stat_sec, wb_cs);
		return -EINVAL;
	}
	targets[0].want = stat_sec;
	targets[1].want = wb_cs;

	queue_delayed_work(system_power_efficient_wq, &ceds_work,
			   (unsigned long)delay_sec * HZ);
	pr_info("dimuat: stat_interval=%us dirty_writeback=%ucs, %u pass mulai +%us\n",
		stat_sec, wb_cs, passes, delay_sec);
	return 0;
}

static void __exit ceds_exit(void)
{
	size_t i;

	cancel_delayed_work_sync(&ceds_work);
	if (!READ_ONCE(restore)) {
		pr_info("dilepas tanpa restore\n");
		return;
	}
	mutex_lock(&ceds_lock);
	for (i = 0; i < ARRAY_SIZE(targets); i++) {
		struct ceds_target *t = &targets[i];
		int ret;

		if (!t->touched || !t->have_orig)
			continue;
		ret = ceds_write(t->path, t->orig);
		if (ret)
			pr_warn("gagal restore %s (%d)\n", t->path, ret);
		else
			pr_info("restore %s -> %lu\n", t->label, t->orig);
	}
	mutex_unlock(&ceds_lock);
}

module_init(ceds_init);
module_exit(ceds_exit);

MODULE_DESCRIPTION("CEDS - Cryo Enhance Deep Sleep (VM tuning hemat daya)");
MODULE_AUTHOR("CEDS contributors");
MODULE_LICENSE("GPL");
