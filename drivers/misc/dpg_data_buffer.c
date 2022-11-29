// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   A kernel module thats supports the dpg data buffer
 */


/*  TODO:
 *  Anpassen der Adressen auf die geschrieben wird
 *  
 *  Anpassen ob bit, bytes, u32 geschrieben wird. -> Also wieviel auf einmal
 * 
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
#include <linux/printk.h>

#define DRIVER_NAME "dpg_data_buffer"
#define OK 0


#define LED_ON 0x7FF
#define LED_INTENSITY_100 100
#define LED_OFF 0x0
#define LED_INTENSITY_0 0



// forward declaration
static int data_buffer_probe(struct platform_device *pdev);
static int data_buffer_remove(struct platform_device *pdev);
static ssize_t data_buffer_write(struct file *filp, const char __user *buf,
				size_t count, loff_t *offp);
static ssize_t data_buffer_read(struct file *filp, char __user *buf, size_t count,
			   loff_t *offp);

static const struct file_operations data_buffer_fops = {
	.read = data_buffer_read,
	.write = data_buffer_write,
};

// device data struct
struct data_buffer {
	u32 *registers; // dummy
	struct miscdevice misc;
	struct platform_device *pdev; // for print in read and write
};

// Supported devices
static const struct of_device_id data_buffer_of_match[] = {
	{
		.compatible = "dpg,spi_data_buffer",
	},
	{},
};
MODULE_DEVICE_TABLE(of, data_buffer_of_match);

// init driver
static struct platform_driver data_buffer_driver = {
	.driver = { .name = DRIVER_NAME,
			.owner = THIS_MODULE,
			.of_match_table = of_match_ptr(data_buffer_of_match) },
	.probe = data_buffer_probe,
	.remove = data_buffer_remove
};
module_platform_driver(data_buffer_driver);

// Probe function -> driver loading
static int data_buffer_probe(struct platform_device *pdev)
{
	int status = 0;
	char deviceName[25];
	struct data_buffer *data_buffer = NULL;
	struct resource *io = NULL;
	static atomic_t idx = ATOMIC_INIT(-1);
	int deviceNumber = atomic_inc_return(&idx);
	// generate device names
	snprintf(deviceName, 25, "dpg_data_buffer_spi%d", deviceNumber);

	dev_info(&pdev->dev, "Function %s entered.\n", __func__);

	// alloc mem
	data_buffer = devm_kzalloc(&pdev->dev, sizeof(*data_buffer), GFP_KERNEL);
	if (IS_ERR(data_buffer)) {
		dev_err(&pdev->dev, "Error in kzalloc.\n");
		return -EFAULT;
	}

	// set data
	platform_set_drvdata(pdev, data_buffer);

	// get resources
	io = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (IS_ERR(io)) {
		dev_err(&pdev->dev, "Error in get resource.\n");
		return -EFAULT;
	}

	data_buffer->registers = devm_ioremap_resource(&pdev->dev, io);
	if (IS_ERR(data_buffer->registers)) {
		dev_err(&pdev->dev, "Error in ioremap.\n");
		return -EFAULT;
	}
	data_buffer->pdev = pdev;

	// set information
	data_buffer->misc.name = deviceName;
	data_buffer->misc.minor = MISC_DYNAMIC_MINOR;
	data_buffer->misc.fops = &data_buffer_fops;
	data_buffer->misc.parent = &pdev->dev;

	// register device
	status = misc_register(&data_buffer->misc);
	if (status != OK) {
		dev_err(&pdev->dev, "Error during device registration.\n");
		return -EFAULT;
	}

	dev_info(&pdev->dev, "Leaving %s probe.\n", __func__);
	return OK;
}

// Remove function -> unload driver
static int data_buffer_remove(struct platform_device *pdev)
{
	struct data_buffer *data_buffer = NULL;

	dev_info(&pdev->dev, "Function %s entered.\n", __func__);

	data_buffer = platform_get_drvdata(pdev);
	// unregister
	misc_deregister(&data_buffer->misc);
	// set device to null
	platform_set_drvdata(pdev, NULL);
	// turn off leds
	// iowrite32(LED_OFF, data_buffer->registers);

	dev_info(&pdev->dev, "Leaving %s remove.\n", __func__);
	return OK;
}

static ssize_t data_buffer_read(struct file *filp, char __user *buf, size_t count,
			   loff_t *offp)
{
	int data = 0;
	int missing_bytes = 0;
	struct data_buffer *data_buff = container_of(filp->private_data, struct data_buffer, misc);
	
    dev_info(&data_buff->pdev->dev, "In %s. count: %d, off: %lld\n", __func__, count, *offp);

	// end of file
	if (*offp >= 1)
		return OK;

	// small buffers
	if (count < sizeof(data))
		return -ETOOSMALL;

	data = ioread32(data_buff->registers);

	missing_bytes = copy_to_user(buf, &data, 1);
	if (missing_bytes > 0)
		dev_info(&data_buff->pdev->dev, "Error in %s\n", __func__);
	*offp += 1;

	return 1; // Zeichen gelesen
}

static ssize_t data_buffer_write(struct file *filp, const char __user *buf,
				size_t count, loff_t *offp)
{
	int data = 0;
	int missing_bytes = 0;
	int idx = 0;
	struct data_buffer *data_buff = container_of(filp->private_data, struct data_buffer, misc);
	
    dev_info(&data_buff->pdev->dev, "In %s. count: %d, off: %lld\n", __func__, count, *offp);

	for (idx = 0; idx < count; idx++) {
		missing_bytes = copy_from_user(&data, buf + *offp, 1);

		iowrite32(data,data_buff->registers);
		msleep(200);

		*offp += 1;
	}
	return count - missing_bytes;
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver for sending and receiving data for the DPG project spi master");
MODULE_AUTHOR("Paul Braher");
