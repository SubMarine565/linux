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
#define WORD 4

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
	struct mutex device_mutex;
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
	struct spi_master *device = dev_id;
	u32 interrupt_status = 0;
	int data_buffer = 0;
	int data_len = 0;
	int idx = 0;

	interrupt_status =
		ioread32(device->registers +
			 OFFSET_IRQ_STATUS); // Check if IRQ from this device
	if (!interrupt_status)
		return IRQ_NONE; // if not bye

	// get number of registers to read
	if (kfifo_out(&device->fifo_data_len, &data_len, WORD) != WORD)
		return -EFAULT;

	for (idx = 0; idx < data_len; idx++) {
		// read data from data buffer
		data_buffer = ioread32(device->registers + OFFSET_RX_DATA);

		if (idx < data_len - 1) {
			// save data in fifo
			kfifo_in(&device->fifo_data, &data_buffer, WORD);
		} else {
			if ((data_len % WORD) == 0) {
				kfifo_in(&device->fifo_data, &data_buffer,
					 WORD);
			} else {
				kfifo_in(&device->fifo_data, &data_buffer,
					 (data_len % WORD));
			}
		}
	}

	// decrease buffer level
	if (atomic_read(&buffer_level) > 0) {
		atomic_dec(&buffer_level);
	}

	// acknowledge IRQ
	iowrite32(IRQ_ACK, device->registers + OFFSET_IRQ_STATUS);

	// wake up wait queues
	wake_up_interruptible(&device->wq_write);
	wake_up_interruptible(&device->wq_read);

	return IRQ_HANDLED;
}

// file open -> lock file
static ssize_t spi_master_open(struct inode *pinode, struct file *pfile)
{
	struct spi_master *device =
		container_of(pfile->private_data, struct spi_master, misc);

	if (mutex_lock_interruptible(&device->device_mutex))
		return -ERESTARTSYS;

	return OK;
}

// file close -> release lock
static int spi_master_release(struct inode *inode, struct file *pfile)
{
	struct spi_master *device =
		container_of(pfile->private_data, struct spi_master, misc);
	mutex_unlock(&device->device_mutex);

	return OK;
}

// Probe function -> get ressources on module load
static int spi_master_probe(struct platform_device *pdev)
{
	int status = 0;
	int irq = 0;
	void *kfifo_buffer = NULL;
	struct spi_master *device = NULL;
	struct resource *io = NULL;
	char device_name[DRIVER_NAME_LEN];
	static atomic_t idx = ATOMIC_INIT(-1);
	int device_number = atomic_inc_return(&idx);
	// generate device names
	snprintf(device_name, DRIVER_NAME_LEN, "dpg_spi%d", device_number);

	dev_info(&pdev->dev, "Function %s entered.\n", __func__);

	// alloc mem
	device = devm_kzalloc(&pdev->dev, sizeof(*device), GFP_KERNEL);
	if (IS_ERR(device)) {
		dev_err(&pdev->dev, "Error in kzalloc.\n");
		return -EFAULT;
	}

	// get resources
	io = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (IS_ERR(io)) {
		dev_err(&pdev->dev, "Error in get resource.\n");
		return -EFAULT;
	}

	device->registers = devm_ioremap_resource(&pdev->dev, io);
	if (IS_ERR(device->registers)) {
		dev_err(&pdev->dev, "Error in ioremap.\n");
		return -EFAULT;
	}

	// set data
	platform_set_drvdata(pdev, device);
	device->pdev = pdev;

	// set information
	device->misc.name = device_name;
	device->misc.minor = MISC_DYNAMIC_MINOR;
	device->misc.fops = &spi_master_fops;
	device->misc.parent = &pdev->dev;

	// get read ready irq
	irq = platform_get_irq(pdev, 0);
	if (!irq) {
		dev_err(&pdev->dev, "No IRQ resource\n");
		return -ENODEV;
	}

	if (devm_request_irq(&pdev->dev, irq, handler_spi_master, IRQF_SHARED,
			     dev_name(&pdev->dev), device)) {
		dev_err(&pdev->dev, "failed to request IRQ\n");
		return -ENOENT;
	}

	// kfifo for data
	// buffer greater than a page for kfifo
	kfifo_buffer = vzalloc(KFIFO_DATA_SIZE);
	if (kfifo_buffer == NULL)
		return -ENOMEM;

	if (kfifo_init(&device->fifo_data, kfifo_buffer, KFIFO_DATA_SIZE))
		return -ENOMEM;

	// kfifo for data length
	if (kfifo_alloc(&device->fifo_data_len, KFIFO_DATA_LEN_SIZE,
			GFP_KERNEL))
		return -ENOMEM;

	init_waitqueue_head(&device->wq_read);
	init_waitqueue_head(&device->wq_write);

	mutex_init(&device->device_mutex);

	// switch on IRQ
	iowrite32(IRQ_ON, device->registers + OFFSET_IRQ_MASK);

	// register device
	status = misc_register(&device->misc);
	if (status != OK) {
		dev_err(&pdev->dev, "Error during device registration.\n");
		return -EFAULT;
	}

	dev_info(&pdev->dev, "Leaving %s probe.\n", __func__);
	return OK;
}

// Remove function -> clean up on module unload
static int spi_master_remove(struct platform_device *pdev)
{
	struct spi_master *device = NULL;

	dev_info(&pdev->dev, "Function %s entered.\n", __func__);

	device = platform_get_drvdata(pdev);
	// unmask IRQ
	iowrite32(IRQ_OFF, device->registers + OFFSET_IRQ_MASK);
	// unregister
	misc_deregister(&device->misc);
	// set device to null
	platform_set_drvdata(pdev, NULL);

	// free vzalloc
	kvfree(device->fifo_data.kfifo.data);

	kfifo_free(&device->fifo_data_len);

	mutex_destroy(&device->device_mutex);

	dev_info(&pdev->dev, "Leaving %s remove.\n", __func__);
	return OK;
}

// on device read
static ssize_t spi_master_read(struct file *filp, char __user *buf,
			       size_t count, loff_t *offp)
{
	int copied_bytes = 0;
	struct spi_master *device =
		container_of(filp->private_data, struct spi_master, misc);

	dev_info(&device->pdev->dev, "In %s. count: %d, off: %lld\n", __func__,
		 count, *offp);

	// end of file
	//if (*offp >= 1)
	//	return OK;

	// wait when fifo empty
	if (wait_event_interruptible(device->wq_read,
				     !kfifo_is_empty(&device->fifo_data)))
		return -ERESTARTSYS;

	// write data when not
	if (kfifo_to_user(&device->fifo_data, buf, count, &copied_bytes)) {
		dev_err(&device->pdev->dev, "Error in %s.\n", __func__);
		return -EFAULT;
	}

	*offp += copied_bytes;
	return copied_bytes; // bytes read
}

// on device write
static ssize_t spi_master_write(struct file *filp, const char __user *buf,
				size_t count, loff_t *offp)
{
	int data_user = 0;
	int idx = 0;
	int bytes_to_copy = 0;
	struct spi_master *device =
		container_of(filp->private_data, struct spi_master, misc);
	dev_info(&device->pdev->dev, "In %s. count: %d, off: %lld\n", __func__,
		 count, *offp);

	// wait when buffer level at two
	if (wait_event_interruptible(device->wq_write,
				     (atomic_read(&buffer_level) < NUM_BUFFERS)))
		return -ERESTARTSYS;

	bytes_to_copy =
		(ioread32(device->registers + OFFSET_REMAINING_SPACE) * WORD);

	if (bytes_to_copy > count)
		bytes_to_copy = count;

	kfifo_in(&device->fifo_data_len, &bytes_to_copy, WORD);

	// fill the data buffer
	for (idx = 0; idx < bytes_to_copy; idx++) {
		if (idx < bytes_to_copy - 1) {
			if (copy_from_user(&data_user, buf + *offp, WORD) != 0)
				return -EFAULT;

			iowrite32(data_user,
				  device->registers + OFFSET_TX_DATA);
			*offp += WORD; // read word
		} else {
			switch (bytes_to_copy % WORD) {
			case 1:
				if (copy_from_user(&data_user, buf + *offp,
						   1) != 0)
					return -EFAULT;
				iowrite32((data_user & 0x000000FF),
					  device->registers + OFFSET_TX_DATA);
				*offp += 1; // read 1 byte
				break;
			case 2:
				if (copy_from_user(&data_user, buf + *offp,
						   2) != 0)
					return -EFAULT;
				iowrite32((data_user & 0x0000FFFF),
					  device->registers + OFFSET_TX_DATA);
				*offp += 2; // read 2 byte
				break;
			case 3:
				if (copy_from_user(&data_user, buf + *offp,
						   3) != 0)
					return -EFAULT;
				iowrite32((data_user & 0x00FFFFFF),
					  device->registers + OFFSET_TX_DATA);
				*offp += 3; // read 3 byte
				break;
			default:
				if (copy_from_user(&data_user, buf + *offp,
						   WORD) != 0)
					return -EFAULT;
				iowrite32(data_user,
					  device->registers + OFFSET_TX_DATA);
				*offp += WORD; // read word
				break;
			}
		}
	}
	atomic_inc(&buffer_level);

	// start signal for data buffer
	iowrite32(START, device->registers + OFFSET_TX_START);

	return bytes_to_copy;
}

// Get CPOL
static ssize_t spi_master_cpol_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct spi_master *device = dev_get_drvdata(dev);
	int value = ioread32(device->registers + OFFSET_CPOL);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);
}

// Set CPOL
static ssize_t spi_master_cpol_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	int bit = 0;
	struct spi_master *device = dev_get_drvdata(dev);

	dev_info(&device->pdev->dev, "In %s. count: %d\n", __func__, count);

	// parse buf to bit and check format
	if (sscanf(buf, "%d", &bit) != 1) {
		dev_err(&device->pdev->dev, "error in %s, invalid format: %s\n",
			__func__, buf);
		return -EINVAL;
	}

	// check value
	if (bit == 1 || bit == 0) {
		// Print value of bit -> for testing
		dev_info(&device->pdev->dev, "In %s. Value read: %d\n",
			 __func__, bit);
		iowrite32(bit, device->registers + OFFSET_CPOL);
	} else {
		dev_err(&device->pdev->dev, "error in %s, invalid value: %d\n",
			__func__, bit);
		return -EINVAL;
	}

	return count;
}

// Get CPHA
static ssize_t spi_master_cpha_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct spi_master *device = dev_get_drvdata(dev);
	int value = ioread32(device->registers + OFFSET_CPHA);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);
}

// Read CPHA
static ssize_t spi_master_cpha_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	int bit = 0;
	struct spi_master *device = dev_get_drvdata(dev);

	dev_info(&device->pdev->dev, "In %s. count: %d\n", __func__, count);

	// parse buf to bit and check format
	if (sscanf(buf, "%d", &bit) != 1) {
		dev_err(&device->pdev->dev, "error in %s, invalid format: %s\n",
			__func__, buf);
		return -EINVAL;
	}

	// check value
	if (bit == 1 || bit == 0) {
		// Print value of bit -> for testing
		dev_info(&device->pdev->dev, "In %s. Value read: %d\n",
			 __func__, bit);
		iowrite32(bit, device->registers + OFFSET_CPHA);
	} else {
		dev_err(&device->pdev->dev, "error in %s, invalid value: %d\n",
			__func__, bit);
		return -EINVAL;
	}

	return count;
}

// Get pre delay
static ssize_t spi_master_pre_delay_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct spi_master *device = dev_get_drvdata(dev);
	int value = ioread32(device->registers + OFFSET_PRE_DELAY);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);
}

// Set pre delay
static ssize_t spi_master_pre_delay_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	int bit = 0;
	struct spi_master *device = dev_get_drvdata(dev);

	dev_info(&device->pdev->dev, "In %s. count: %d\n", __func__, count);

	// parse buf to bit and check format
	if (sscanf(buf, "%d", &bit) != 1) {
		dev_err(&device->pdev->dev, "error in %s, invalid format: %s\n",
			__func__, buf);
		return -EINVAL;
	}

	// check value
	if (bit < (1 << 8) || bit >= 0) {
		// Print value of bit -> for testing
		dev_info(&device->pdev->dev, "In %s. Value read: %d\n",
			 __func__, bit);
		iowrite32(bit, device->registers + OFFSET_PRE_DELAY);
	} else {
		dev_err(&device->pdev->dev, "error in %s, invalid value: %d\n",
			__func__, bit);
		return -EINVAL;
	}

	return count;
}

// Get post delay
static ssize_t spi_master_post_delay_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct spi_master *device = dev_get_drvdata(dev);
	int value = ioread32(device->registers + OFFSET_POST_DELAY);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);

	value = ioread32(device->registers + 0xC);
}

// Set post delay
static ssize_t spi_master_post_delay_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	int bit = 0;
	struct spi_master *device = dev_get_drvdata(dev);

	dev_info(&device->pdev->dev, "In %s. count: %d\n", __func__, count);

	// parse buf to bit and check format
	if (sscanf(buf, "%d", &bit) != 1) {
		dev_err(&device->pdev->dev, "error in %s, invalid format: %s\n",
			__func__, buf);
		return -EINVAL;
	}

	// check value
	if (bit < (1 << 8) || bit >= 0) {
		// Print value of bit -> for testing
		dev_info(&device->pdev->dev, "In %s. Value read: %d\n",
			 __func__, bit);
		iowrite32(bit, device->registers + OFFSET_POST_DELAY);
	} else {
		dev_err(&device->pdev->dev, "error in %s, invalid value: %d\n",
			__func__, bit);
		return -EINVAL;
	}

	return count;
}

// Get clock per half bit
static ssize_t spi_master_clk_per_half_bit_show(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	struct spi_master *device = dev_get_drvdata(dev);
	int value = ioread32(device->registers + OFFSET_CLK_PER_HALF_BIT);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);
}

// Set clock per half bit
static ssize_t spi_master_clk_per_half_bit_store(struct device *dev,
						 struct device_attribute *attr,
						 const char *buf, size_t count)
{
	int bit = 0;
	struct spi_master *device = dev_get_drvdata(dev);

	dev_info(&device->pdev->dev, "In %s. count: %d\n", __func__, count);

	// parse buf to bit and check format
	if (sscanf(buf, "%d", &bit) != 1) {
		dev_err(&device->pdev->dev, "error in %s, invalid format: %s\n",
			__func__, buf);
		return -EINVAL;
	}

	// check value
	if (bit < (1 << 16) || bit > 0) {
		// Print value of bit -> for testing
		dev_info(&device->pdev->dev, "In %s. Value read: %d\n",
			 __func__, bit);
		iowrite32(bit, device->registers + OFFSET_CLK_PER_HALF_BIT);
	} else {
		dev_err(&device->pdev->dev, "error in %s, invalid value: %d\n",
			__func__, bit);
		return -EINVAL;
	}

	return count;
}

// Get git revision
static ssize_t spi_master_gitrev_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct spi_master *device = dev_get_drvdata(dev);
	u32 gitrev[GIT_REV_WORDS] = { 0 };
	int idx = 0;

	for (idx = 0; idx < GIT_REV_WORDS; idx++) {
		gitrev[idx] =
			ioread32(device->registers + OFFSET_GIT_REV + idx);
	}

	return scnprintf(buf, PAGE_SIZE, "%8x%8x%8x%8x%8x%8x\n", gitrev[5],
			 gitrev[4], gitrev[3], gitrev[2], gitrev[1], gitrev[0]);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver for the DPG project spi master");
MODULE_AUTHOR("Paul Braher");
