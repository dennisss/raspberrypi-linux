// SPDX-License-Identifier: GPL-2.0
/*
 * PTP GPIO Pulse driver
 * 
 * This driver allows userspace to trigger a sequence of reading a PTP clock,
 * toggling a GPIO, and reading the PTP clock again, with minimal latency.
 */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/file.h>
#include <linux/posix-clock.h>

struct ptp_gpio_pulse_args {
	int ptp_fd;
	unsigned int gpio_num;
	unsigned int wait_us;
	struct {
		long long tv_sec;
		long long tv_nsec;
	} ts1, ts2;
};

#define PTP_GPIO_PULSE_IOC_MAGIC 'P'
#define PTP_GPIO_PULSE_IOCTL _IOWR(PTP_GPIO_PULSE_IOC_MAGIC, 1, struct ptp_gpio_pulse_args)

struct ptp_gpio_pulse_ctx {
	bool gpio_requested;
	unsigned int gpio_num;
	struct mutex lock;
};

static int ptp_gpio_pulse_open(struct inode *inode, struct file *file)
{
	struct ptp_gpio_pulse_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->lock);
	ctx->gpio_requested = false;
	file->private_data = ctx;

	return 0;
}

static int ptp_gpio_pulse_release(struct inode *inode, struct file *file)
{
	struct ptp_gpio_pulse_ctx *ctx = file->private_data;

	if (ctx->gpio_requested)
		gpio_free(ctx->gpio_num);

	kfree(ctx);
	return 0;
}

static long ptp_gpio_pulse_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ptp_gpio_pulse_ctx *ctx = file->private_data;
	struct ptp_gpio_pulse_args args;
	struct fd f;
	struct posix_clock_context *pccontext;
	struct posix_clock *clk;
	struct timespec64 ts1 = {0}, ts2 = {0};
	unsigned long flags;
	int ret = 0;

	if (cmd != PTP_GPIO_PULSE_IOCTL)
		return -ENOTTY;

	if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
		return -EFAULT;

	mutex_lock(&ctx->lock);

	if (ctx->gpio_requested && ctx->gpio_num != args.gpio_num) {
		gpio_free(ctx->gpio_num);
		ctx->gpio_requested = false;
	}

	if (!ctx->gpio_requested) {
		ret = gpio_request(args.gpio_num, "ptp_gpio_pulse");
		if (ret) {
			mutex_unlock(&ctx->lock);
			return ret;
		}
		ctx->gpio_num = args.gpio_num;
		ctx->gpio_requested = true;
		gpio_direction_output(args.gpio_num, 0);
		
		// Ensure 0 is stabilized.
		usleep_range(500, 1000);
	}

	mutex_unlock(&ctx->lock);

	f = fdget(args.ptp_fd);
	if (fd_empty(f))
		return -EBADF;

	if (!fd_file(f)->f_path.dentry || !fd_file(f)->f_path.dentry->d_name.name ||
	    strncmp(fd_file(f)->f_path.dentry->d_name.name, "ptp", 3) != 0) {
		fdput(f);
		return -EINVAL;
	}

	pccontext = fd_file(f)->private_data;
	if (!pccontext || !pccontext->clk) {
		fdput(f);
		return -EINVAL;
	}

	clk = pccontext->clk;
	if (!clk->ops.clock_gettime) {
		fdput(f);
		return -EOPNOTSUPP;
	}

	down_read(&clk->rwsem);
	if (clk->zombie) {
		up_read(&clk->rwsem);
		fdput(f);
		return -ENODEV;
	}

	local_irq_save(flags);
	clk->ops.clock_gettime(clk, &ts1);
	gpio_set_value(args.gpio_num, 1);
	clk->ops.clock_gettime(clk, &ts2);
	local_irq_restore(flags);

	up_read(&clk->rwsem);
	fdput(f);

	if (args.wait_us > 0)
		usleep_range(args.wait_us, args.wait_us + 500);

	gpio_set_value(args.gpio_num, 0);

	args.ts1.tv_sec = ts1.tv_sec;
	args.ts1.tv_nsec = ts1.tv_nsec;
	args.ts2.tv_sec = ts2.tv_sec;
	args.ts2.tv_nsec = ts2.tv_nsec;

	if (copy_to_user((void __user *)arg, &args, sizeof(args)))
		return -EFAULT;

	return 0;
}

static const struct file_operations ptp_gpio_pulse_fops = {
	.owner = THIS_MODULE,
	.open = ptp_gpio_pulse_open,
	.release = ptp_gpio_pulse_release,
	.unlocked_ioctl = ptp_gpio_pulse_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ptp_gpio_pulse_ioctl,
#endif
};

static struct miscdevice ptp_gpio_pulse_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ptp_gpio_pulse",
	.fops = &ptp_gpio_pulse_fops,
};

static int __init ptp_gpio_pulse_init(void)
{
	return misc_register(&ptp_gpio_pulse_misc);
}

static void __exit ptp_gpio_pulse_exit(void)
{
	misc_deregister(&ptp_gpio_pulse_misc);
}

module_init(ptp_gpio_pulse_init);
module_exit(ptp_gpio_pulse_exit);

MODULE_AUTHOR("Dennis");
MODULE_DESCRIPTION("PTP GPIO Pulse trigger driver");
MODULE_LICENSE("GPL");
