// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  A kernel module that supports the data buffer and spi master
 *  of the DPG project.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/wait.h>

#define DRIVER_NAME "dpg_spi_master"
#define DRIVER_NAME_LEN 15
#define START 0x1
#define IRQ_ON 0x1
#define IRQ_OFF 0x0
#define IRQ_ACK 0x1
#define KFIFO_DATA_SIZE 32768
#define KFIFO_DATA_LEN_SIZE 32

#define OK 0

// Offsets data buffer
#define OFFSET_DATA_BUFFER (0x0 / 4)

#define OFFSET_GIT_REV (OFFSET_DATA_BUFFER + (0x0 / 4))
#define GIT_REV_WORDS 6
#define OFFSET_TX_START (OFFSET_DATA_BUFFER + (0x20 / 4))
#define OFFSET_TX_TRANSFER (OFFSET_DATA_BUFFER + (0x24 / 4))
#define OFFSET_TX_DATA (OFFSET_DATA_BUFFER + (0x28 / 4))
#define OFFSET_TX_COUNT (OFFSET_DATA_BUFFER + (0x2C / 4))
#define OFFSET_RX_DATA (OFFSET_DATA_BUFFER + (0x30 / 4))
#define OFFSET_REMAINING_SPACE (OFFSET_DATA_BUFFER + (0x34 / 4))
#define OFFSET_IRQ_MASK (OFFSET_DATA_BUFFER + (0x38 / 4))
#define OFFSET_IRQ_STATUS (OFFSET_DATA_BUFFER + (0x3C / 4))

// Offsets spi master
#define OFFSET_MASTER (0x40 / 4)

#define OFFSET_CPOL (OFFSET_MASTER + (0x0 / 4))
#define OFFSET_CPHA (OFFSET_MASTER + (0x4 / 4))
#define OFFSET_PRE_DELAY (OFFSET_MASTER + (0x8 / 4))
#define OFFSET_POST_DELAY (OFFSET_MASTER + (0xC / 4))
#define OFFSET_CLK_PER_HALF_BIT (OFFSET_MASTER + (0x10 / 4))

// number of buffers in data buffer
#define NUM_BUFFERS 2
static atomic_t buffer_level = ATOMIC_INIT(0);

// forward declaration
static ssize_t spi_master_open(struct inode *pinode, struct file *pfile);
static int spi_master_remove(struct platform_device *pdev);
static int spi_master_release(struct inode *inode, struct file *pfile);
static int spi_master_probe(struct platform_device *pdev);
static ssize_t spi_master_write(struct file *filp, const char __user *buf,
				size_t count, loff_t *offp);
static ssize_t spi_master_read(struct file *filp, char __user *buf,
			       size_t count, loff_t *offp);
static ssize_t spi_master_cpol_show(struct device *dev,
				    struct device_attribute *attr, char *buf);
static ssize_t spi_master_cpol_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count);
static ssize_t spi_master_cpha_show(struct device *dev,
				    struct device_attribute *attr, char *buf);
static ssize_t spi_master_cpha_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count);
static ssize_t spi_master_pre_delay_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf);
static ssize_t spi_master_pre_delay_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count);
static ssize_t spi_master_post_delay_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf);
static ssize_t spi_master_post_delay_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count);
static ssize_t spi_master_clk_per_half_bit_show(struct device *dev,
						struct device_attribute *attr,
						char *buf);
static ssize_t spi_master_clk_per_half_bit_store(struct device *dev,
						 struct device_attribute *attr,
						 const char *buf, size_t count);
static ssize_t spi_master_gitrev_show(struct device *dev,
				      struct device_attribute *attr, char *buf);

static const struct file_operations spi_master_fops = {
	.open = spi_master_open,
	.read = spi_master_read,
	.write = spi_master_write,
	.release = spi_master_release,
};

// device data struct
struct spi_master {
	u32 *registers; // dummy
	struct miscdevice misc;
	struct platform_device *pdev; // for print in read and write
	struct kfifo fifo_data;
	struct kfifo fifo_data_len;
	struct mutex spi_master_mutex;
	wait_queue_head_t wq_read;
	wait_queue_head_t wq_write;
};

static DEVICE_ATTR(cpol, 0664, spi_master_cpol_show, spi_master_cpol_store);
static DEVICE_ATTR(cpha, 0664, spi_master_cpha_show, spi_master_cpha_store);
static DEVICE_ATTR(pre_delay, 0664, spi_master_pre_delay_show,
		   spi_master_pre_delay_store);
static DEVICE_ATTR(post_delay, 0664, spi_master_post_delay_show,
		   spi_master_post_delay_store);
static DEVICE_ATTR(clk_per_half_bit, 0664, spi_master_clk_per_half_bit_show,
		   spi_master_clk_per_half_bit_store);
static DEVICE_ATTR(git_rev, 0444, spi_master_gitrev_show, NULL);

static struct attribute *spi_attrs[] = { &dev_attr_cpol.attr,
					 &dev_attr_cpha.attr,
					 &dev_attr_pre_delay.attr,
					 &dev_attr_post_delay.attr,
					 &dev_attr_clk_per_half_bit.attr,
					 &dev_attr_git_rev.attr,
					 NULL };

// create spi_group and spi_groups
ATTRIBUTE_GROUPS(spi);

// Supported devices
static const struct of_device_id spi_master_of_match[] = {
	{
		.compatible = "dpg,spi_master",
	},
	{},
};
MODULE_DEVICE_TABLE(of, spi_master_of_match);

// init driver
static struct platform_driver spi_master_driver = {
	.driver = { .name = DRIVER_NAME,
			.owner = THIS_MODULE,
			.of_match_table = of_match_ptr(spi_master_of_match),
			.groups = spi_groups	// sysfs files in /sys/bus/platform/drivers/dpg_spi_master
			 },
	.probe = spi_master_probe,
	.remove = spi_master_remove
};
module_platform_driver(spi_master_driver);

// IRQ Handler
static irqreturn_t handler_spi_master(int irq, void *dev_id)
{
	struct spi_master *data = dev_id;
	u32 interrupt_status = 0;
	int data_register = 0;
	int data_len = 0;
	int idx = 0;

	interrupt_status =
		ioread32(data->registers +
			 OFFSET_IRQ_STATUS); // Check if IRQ from this device
	if (!interrupt_status)
		return IRQ_NONE; // if not bye

	// get number of registers to read
	if (kfifo_out(&data->fifo_data_len, &data_len, 4) != 4)
		return -EFAULT;

	for (idx = 0; idx < data_len; idx++) {
		// read data from data buffer
		data_register = ioread32(data->registers + OFFSET_RX_DATA);

		if (idx < data_len - 1) {
			// save data in fifo
			kfifo_in(&data->fifo_data, &data_register, 4);
		} else {
			if ((data_len % 4) == 0) {
				kfifo_in(&data->fifo_data, &data_register, 4);
			} else {
				kfifo_in(&data->fifo_data, &data_register,
					 (data_len % 4));
			}
		}
	}

	// decrease buffer level
	if (atomic_read(&buffer_level) > 0) {
		atomic_dec(&buffer_level);
	}

	// acknowledge IRQ
	iowrite32(IRQ_ACK, data->registers + OFFSET_IRQ_STATUS);

	// wake up wait queues
	wake_up_interruptible(&data->wq_write);
	wake_up_interruptible(&data->wq_read);

	return IRQ_HANDLED;
}

// file open -> lock file
static ssize_t spi_master_open(struct inode *pinode, struct file *pfile)
{
	struct spi_master *spi_master =
		container_of(pfile->private_data, struct spi_master, misc);

	if (mutex_lock_interruptible(&spi_master->spi_master_mutex))
		return -ERESTARTSYS;

	return OK;
}

// file close -> release lock
static int spi_master_release(struct inode *inode, struct file *pfile)
{
	struct spi_master *spi_master =
		container_of(pfile->private_data, struct spi_master, misc);
	mutex_unlock(&spi_master->spi_master_mutex);

	return OK;
}

// Probe function -> driver loading
static int spi_master_probe(struct platform_device *pdev)
{
	int status = 0;
	int irq = 0;
	void *kfifo_buff = NULL;
	char deviceName[DRIVER_NAME_LEN];
	struct spi_master *spi_master = NULL;
	struct resource *io = NULL;
	static atomic_t idx = ATOMIC_INIT(-1);
	int deviceNumber = atomic_inc_return(&idx);
	// generate device names
	snprintf(deviceName, DRIVER_NAME_LEN, "dpg_spi%d", deviceNumber);

	dev_info(&pdev->dev, "Function %s entered.\n", __func__);

	// alloc mem
	spi_master = devm_kzalloc(&pdev->dev, sizeof(*spi_master), GFP_KERNEL);
	if (IS_ERR(spi_master)) {
		dev_err(&pdev->dev, "Error in kzalloc.\n");
		return -EFAULT;
	}

	// get resources
	io = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (IS_ERR(io)) {
		dev_err(&pdev->dev, "Error in get resource.\n");
		return -EFAULT;
	}

	spi_master->registers = devm_ioremap_resource(&pdev->dev, io);
	if (IS_ERR(spi_master->registers)) {
		dev_err(&pdev->dev, "Error in ioremap.\n");
		return -EFAULT;
	}

	// set data
	platform_set_drvdata(pdev, spi_master);
	spi_master->pdev = pdev;

	// set information
	spi_master->misc.name = deviceName;
	spi_master->misc.minor = MISC_DYNAMIC_MINOR;
	spi_master->misc.fops = &spi_master_fops;
	spi_master->misc.parent = &pdev->dev;

	// get read ready irq TODO
	irq = platform_get_irq(pdev, 0);
	if (!irq) {
		dev_err(&pdev->dev, "No IRQ resource\n");
		return -ENODEV;
	}

	if (devm_request_irq(&pdev->dev, irq, handler_spi_master, IRQF_SHARED,
			     dev_name(&pdev->dev), spi_master)) {
		dev_err(&pdev->dev, "failed to request IRQ\n");
		return -ENOENT;
	}

	// kfifo for data
	// buffer greater than a page for kfifo
	kfifo_buff = vzalloc(KFIFO_DATA_SIZE);
	if (kfifo_buff == NULL)
		return -ENOMEM;

	if (kfifo_init(&spi_master->fifo_data, kfifo_buff, KFIFO_DATA_SIZE))
		return -ENOMEM;

	// kfifo for data length
	if (kfifo_alloc(&spi_master->fifo_data_len, KFIFO_DATA_LEN_SIZE,
			GFP_KERNEL))
		return -ENOMEM;

	init_waitqueue_head(&spi_master->wq_read);
	init_waitqueue_head(&spi_master->wq_write);

	mutex_init(&spi_master->spi_master_mutex);

	// switch on IRQ
	iowrite32(IRQ_ON, spi_master->registers + OFFSET_IRQ_MASK);

	// register device
	status = misc_register(&spi_master->misc);
	if (status != OK) {
		dev_err(&pdev->dev, "Error during device registration.\n");
		return -EFAULT;
	}

	dev_info(&pdev->dev, "Leaving %s probe.\n", __func__);
	return OK;
}

// Remove function -> unload driver
static int spi_master_remove(struct platform_device *pdev)
{
	struct spi_master *spi_master = NULL;

	dev_info(&pdev->dev, "Function %s entered.\n", __func__);

	spi_master = platform_get_drvdata(pdev);
	// unmask IRQ TODO
	iowrite32(IRQ_OFF, spi_master->registers + OFFSET_IRQ_MASK);
	// unregister
	misc_deregister(&spi_master->misc);
	// set device to null
	platform_set_drvdata(pdev, NULL);

	// free vzalloc
	kvfree(spi_master->fifo_data.kfifo.data);

	kfifo_free(&spi_master->fifo_data_len);

	mutex_destroy(&spi_master->spi_master_mutex);

	dev_info(&pdev->dev, "Leaving %s remove.\n", __func__);
	return OK;
}

static ssize_t spi_master_read(struct file *filp, char __user *buf,
			       size_t count, loff_t *offp)
{
	int copiedBytes = 0;
	struct spi_master *data =
		container_of(filp->private_data, struct spi_master, misc);

	dev_info(&data->pdev->dev, "In %s. count: %d, off: %lld\n", __func__,
		 count, *offp);

	// end of file
	//if (*offp >= 1)
	//	return OK;

	// wait when fifo empty
	if (wait_event_interruptible(data->wq_read,
				     !kfifo_is_empty(&data->fifo_data)))
		return -ERESTARTSYS;

	// write data when not
	if (kfifo_to_user(&data->fifo_data, buf, count, &copiedBytes)) {
		dev_err(&data->pdev->dev, "Error in %s.\n", __func__);
		return -EFAULT;
	}

	*offp += copiedBytes;
	return copiedBytes; // bytes read
}

static ssize_t spi_master_write(struct file *filp, const char __user *buf,
				size_t count, loff_t *offp)
{
	int word = 0;
	int idx = 0;
	int bytes_to_copy = 0;
	struct spi_master *data =
		container_of(filp->private_data, struct spi_master, misc);
	dev_info(&data->pdev->dev, "In %s. count: %d, off: %lld\n", __func__,
		 count, *offp);

	// wait when buffer level at two
	if (wait_event_interruptible(
		    data->wq_write, (atomic_read(&buffer_level) < NUM_BUFFERS)))
		return -ERESTARTSYS;

	bytes_to_copy =
		(ioread32(data->registers + OFFSET_REMAINING_SPACE) * 4);

	if (bytes_to_copy > count)
		bytes_to_copy = count;

	kfifo_in(&data->fifo_data_len, &bytes_to_copy, 4);

	// fill the data buffer
	for (idx = 0; idx < bytes_to_copy; idx++) {
		if (idx < bytes_to_copy - 1) {
			if (copy_from_user(&word, buf + *offp, 4) != 0)
				return -EFAULT;

			iowrite32(word, data->registers + OFFSET_TX_DATA);
			*offp += 4; // read word
		} else {
			switch (bytes_to_copy % 4) {
			case 1:
				if (copy_from_user(&word, buf + *offp, 1) != 0)
					return -EFAULT;
				iowrite32((word & 0x000000FF),
					  data->registers + OFFSET_TX_DATA);
				*offp += 1; // read 1 byte
				break;
			case 2:
				if (copy_from_user(&word, buf + *offp, 2) != 0)
					return -EFAULT;
				iowrite32((word & 0x0000FFFF),
					  data->registers + OFFSET_TX_DATA);
				*offp += 2; // read 2 byte
				break;
			case 3:
				if (copy_from_user(&word, buf + *offp, 3) != 0)
					return -EFAULT;
				iowrite32((word & 0x00FFFFFF),
					  data->registers + OFFSET_TX_DATA);
				*offp += 3; // read 3 byte
				break;
			default:
				if (copy_from_user(&word, buf + *offp, 4) != 0)
					return -EFAULT;
				iowrite32(word,
					  data->registers + OFFSET_TX_DATA);
				*offp += 4; // read word
				break;
			}
		}
	}
	atomic_inc(&buffer_level);

	// start signal for data buffer
	iowrite32(START, data->registers + OFFSET_TX_START);

	return bytes_to_copy;
}

static ssize_t spi_master_cpol_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct spi_master *data = dev_get_drvdata(dev);
	int value = ioread32(data->registers + OFFSET_CPOL);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);
}

static ssize_t spi_master_cpol_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	int bit = 0;
	struct spi_master *data = dev_get_drvdata(dev);

	dev_info(&data->pdev->dev, "In %s. count: %d\n", __func__, count);

	// parse buf to bit and check format
	if (sscanf(buf, "%d", &bit) != 1) {
		dev_err(&data->pdev->dev, "error in %s, invalid format: %s\n",
			__func__, buf);
		return -EINVAL;
	}

	// check value
	if (bit == 1 || bit == 0) {
		// Print value of bit -> for testing
		dev_info(&data->pdev->dev, "In %s. Value read: %d\n", __func__,
			 bit);
		iowrite32(bit, data->registers + OFFSET_CPOL);
	} else {
		dev_err(&data->pdev->dev, "error in %s, invalid value: %d\n",
			__func__, bit);
		return -EINVAL;
	}

	return count;
}

static ssize_t spi_master_cpha_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct spi_master *data = dev_get_drvdata(dev);
	int value = ioread32(data->registers + OFFSET_CPHA);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);
}

static ssize_t spi_master_cpha_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	int bit = 0;
	struct spi_master *data = dev_get_drvdata(dev);

	dev_info(&data->pdev->dev, "In %s. count: %d\n", __func__, count);

	// parse buf to bit and check format
	if (sscanf(buf, "%d", &bit) != 1) {
		dev_err(&data->pdev->dev, "error in %s, invalid format: %s\n",
			__func__, buf);
		return -EINVAL;
	}

	// check value
	if (bit == 1 || bit == 0) {
		// Print value of bit -> for testing
		dev_info(&data->pdev->dev, "In %s. Value read: %d\n", __func__,
			 bit);
		iowrite32(bit, data->registers + OFFSET_CPHA);
	} else {
		dev_err(&data->pdev->dev, "error in %s, invalid value: %d\n",
			__func__, bit);
		return -EINVAL;
	}

	return count;
}

static ssize_t spi_master_pre_delay_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct spi_master *data = dev_get_drvdata(dev);
	int value = ioread32(data->registers + OFFSET_PRE_DELAY);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);
}

static ssize_t spi_master_pre_delay_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	int bit = 0;
	struct spi_master *data = dev_get_drvdata(dev);

	dev_info(&data->pdev->dev, "In %s. count: %d\n", __func__, count);

	// parse buf to bit and check format
	if (sscanf(buf, "%d", &bit) != 1) {
		dev_err(&data->pdev->dev, "error in %s, invalid format: %s\n",
			__func__, buf);
		return -EINVAL;
	}

	// check value
	if (bit < (1 << 8) || bit >= 0) {
		// Print value of bit -> for testing
		dev_info(&data->pdev->dev, "In %s. Value read: %d\n", __func__,
			 bit);
		iowrite32(bit, data->registers + OFFSET_PRE_DELAY);
	} else {
		dev_err(&data->pdev->dev, "error in %s, invalid value: %d\n",
			__func__, bit);
		return -EINVAL;
	}

	return count;
}

static ssize_t spi_master_post_delay_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct spi_master *data = dev_get_drvdata(dev);
	int value = ioread32(data->registers + OFFSET_POST_DELAY);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);

	value = ioread32(data->registers + 0xC);
}

static ssize_t spi_master_post_delay_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	int bit = 0;
	struct spi_master *data = dev_get_drvdata(dev);

	dev_info(&data->pdev->dev, "In %s. count: %d\n", __func__, count);

	// parse buf to bit and check format
	if (sscanf(buf, "%d", &bit) != 1) {
		dev_err(&data->pdev->dev, "error in %s, invalid format: %s\n",
			__func__, buf);
		return -EINVAL;
	}

	// check value
	if (bit < (1 << 8) || bit >= 0) {
		// Print value of bit -> for testing
		dev_info(&data->pdev->dev, "In %s. Value read: %d\n", __func__,
			 bit);
		iowrite32(bit, data->registers + OFFSET_POST_DELAY);
	} else {
		dev_err(&data->pdev->dev, "error in %s, invalid value: %d\n",
			__func__, bit);
		return -EINVAL;
	}

	return count;
}

static ssize_t spi_master_clk_per_half_bit_show(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	struct spi_master *data = dev_get_drvdata(dev);
	int value = ioread32(data->registers + OFFSET_CLK_PER_HALF_BIT);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);
}

static ssize_t spi_master_clk_per_half_bit_store(struct device *dev,
						 struct device_attribute *attr,
						 const char *buf, size_t count)
{
	int bit = 0;
	struct spi_master *data = dev_get_drvdata(dev);

	dev_info(&data->pdev->dev, "In %s. count: %d\n", __func__, count);

	// parse buf to bit and check format
	if (sscanf(buf, "%d", &bit) != 1) {
		dev_err(&data->pdev->dev, "error in %s, invalid format: %s\n",
			__func__, buf);
		return -EINVAL;
	}

	// check value
	if (bit < (1 << 16) || bit > 0) {
		// Print value of bit -> for testing
		dev_info(&data->pdev->dev, "In %s. Value read: %d\n", __func__,
			 bit);
		iowrite32(bit, data->registers + OFFSET_CLK_PER_HALF_BIT);
	} else {
		dev_err(&data->pdev->dev, "error in %s, invalid value: %d\n",
			__func__, bit);
		return -EINVAL;
	}

	return count;
}

// TODO -> MSB reg 7, LSB reg0
static ssize_t spi_master_gitrev_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct spi_master *data = dev_get_drvdata(dev);
	u32 word[GIT_REV_WORDS] = { 0 };
	int idx = 0;

	for (idx = 0; idx < GIT_REV_WORDS; idx++) {
		word[idx] = ioread32(data->registers + OFFSET_GIT_REV + idx);
	}

	return scnprintf(buf, PAGE_SIZE, "%8x%8x%8x%8x%8x%8x\n", word[5],
			 word[4], word[3], word[2], word[1], word[0]);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver for the DPG project spi master");
MODULE_AUTHOR("Paul Braher");
