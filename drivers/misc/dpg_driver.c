// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  A kernel module that supports the data buffer and spi master
 *  of the DPG project.
 */


/*
 *	Fragen:
 *	Funktionen hier sperren waerend anderer Treiber arbeitet?
 *	-> Keine Config setzen waerend dem Senden
 *	-> EInfachste Lsg alles in einen Treiber da das eine ohne das andere nicht existieren kann


 * Files können offen bleiben read write aufruf bei entsprechendem zugriff


 * 	Unterordner in /sys/devices fuer unser Geraet
 * -> Mehere root objects
 * 
 * 	Files anlegen ohne Race conditions = ohne create_files oder create_group
 * 	-> .groups bei device oder driver funktioniert nicht
 *	-> Sollte mit char device funktionnieren nicht extra root device anlegen
 -> aus misc device verwenden 
 * 
 * 	copy_from_user und copy_to_user bei sysfs devices notwendig?	-> Nein
 * 
 * 	Create group funktioniert nur wenn kobj von sysfs_device genommen wird,
 * 	nicht wenn pdev->dev.kobj genommen wird, wieso?
 * 	Genauso bei register device!
 * 	Wieso funktioniert pdev->dev nicht? -> Sollte eventuell dann mit char dev funktionieren
 * 	
 * 	TODO defines, etc. -> Make it beautiful!
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
#define INTERRUPT_MASK 2
#define INTERRUPT_ACK 3
#define IRQ_ON 0xF
#define IRQ_OFF 0x0

#define OK 0

// forward declaration
static ssize_t spi_master_open(struct inode *pinode, struct file *pfile);
static int spi_master_remove(struct platform_device *pdev);
static int spi_master_release(struct inode *inode, struct file *pfile);
static int spi_master_probe(struct platform_device *pdev);
static ssize_t spi_master_write(struct file *filp, const char __user *buf,
				size_t count, loff_t *offp);
static ssize_t spi_master_read(struct file *filp, char __user *buf, size_t count,
			   loff_t *offp);
static ssize_t spi_master_cpol_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t spi_master_cpol_store(struct device *dev, struct device_attribute *attr,
         const char *buf, size_t count);
static ssize_t spi_master_cpha_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t spi_master_cpha_store(struct device *dev, struct device_attribute *attr,
         const char *buf, size_t count);
static ssize_t spi_master_pre_delay_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t spi_master_pre_delay_store(struct device *dev, struct device_attribute *attr,
         const char *buf, size_t count);
static ssize_t spi_master_post_delay_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t spi_master_post_delay_store(struct device *dev, struct device_attribute *attr,
         const char *buf, size_t count);
static ssize_t spi_master_clk_per_half_bit_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t spi_master_clk_per_half_bit_store(struct device *dev, struct device_attribute *attr,
         const char *buf, size_t count);
static ssize_t spi_master_gitrev_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t spi_master_gitrev_store(struct device *dev, struct device_attribute *attr,
         const char *buf, size_t count);


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
	struct mutex spi_master_mutex;
	wait_queue_head_t wq;
};

static DEVICE_ATTR(cpol, 0664, spi_master_cpol_show, spi_master_cpol_store);
static DEVICE_ATTR(cpha, 0664, spi_master_cpha_show, spi_master_cpha_store);
static DEVICE_ATTR(pre_delay, 0664, spi_master_pre_delay_show, spi_master_pre_delay_store);
static DEVICE_ATTR(post_delay, 0664, spi_master_post_delay_show, spi_master_post_delay_store);
static DEVICE_ATTR(clk_per_half_bit, 0664, spi_master_clk_per_half_bit_show, spi_master_clk_per_half_bit_store);
static DEVICE_ATTR(git_rev, 0664, spi_master_gitrev_show, spi_master_gitrev_store);

static struct attribute *spi_attrs[] = {
        &dev_attr_cpol.attr,
		&dev_attr_cpha.attr,
		&dev_attr_pre_delay.attr,
		&dev_attr_post_delay.attr,
		&dev_attr_clk_per_half_bit.attr,
		&dev_attr_git_rev.attr,
        NULL
};

// create spi_group and spi_groups
ATTRIBUTE_GROUPS(spi);

// TODO sysfs device nötig??
// sysfs
//static struct device *sysfs_device;

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
			.groups = spi_groups	// TODO Hier richtig?? -> Files in /sys/bus/platform/drivers/dpg_spi_master
			 },
	.probe = spi_master_probe,
	.remove = spi_master_remove
};
module_platform_driver(spi_master_driver);

// TODO -> richitg machen
// IRQ Handler
/*
static irqreturn_t handler_spi_master(int irq, void *dev_id)
{
	struct spi_master *data = dev_id;
	u32 interrupt_status = 0;

	interrupt_status =
		ioread32(data->registers +
			 INTERRUPT_ACK); // Check if IRQ from this device
	if (!interrupt_status)
		return IRQ_NONE; // if not bye

	// acknowledge IRQ
	iowrite32(IRQ_ON, data->registers + INTERRUPT_ACK);
	// wake up wait queue
	wake_up_interruptible(&data->wq);

	return IRQ_HANDLED;
}
*/

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

// TODO -> IRQ
// Probe function -> driver loading
static int spi_master_probe(struct platform_device *pdev)
{
	int status = 0;
	//int irq = 0;
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

	// get pushbutton irq TODO
	/*
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
	*/
	init_waitqueue_head(&spi_master->wq);

	mutex_init(&spi_master->spi_master_mutex);

	// switch on IRQ
	//iowrite32(IRQ_ON, spi_master->registers + INTERRUPT_MASK);

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
	//iowrite32(IRQ_OFF, spi_master->registers + INTERRUPT_MASK);
	// unregister
	misc_deregister(&spi_master->misc);
	// set device to null
	platform_set_drvdata(pdev, NULL);

	// TODO delete mutex -> hier richtig??
	mutex_destroy(&spi_master->spi_master_mutex);

	dev_info(&pdev->dev, "Leaving %s remove.\n", __func__);
	return OK;
}

// TODO -> richitg machen
static ssize_t spi_master_read(struct file *filp, char __user *buf, size_t count,
			   loff_t *offp)
{
	/*
	int led_intensity = 0;
	int missing_bytes = 0;
	struct spi_master *data =
		container_of(filp->private_data, struct spi_master, misc);
	dev_info(&data->pdev->dev, "In %s. count: %d, off: %lld\n",
		 __func__, count, *offp);

	// end of file
	if (*offp >= 1)
		return OK;

	// small buffers
	if (count < sizeof(led_intensity))
		return -ETOOSMALL;

	// Convert hex to percent
	led_intensity = ioread32(data->registers) * LED_INTENSITY_100 / LED_ON;

	missing_bytes = copy_to_user(buf, &led_intensity, 1);
	if (missing_bytes > 0)
		dev_info(&data->pdev->dev, "Error in %s\n", __func__);
	*offp += 1;
*/
	return 1; // Zeichen gelesen
}

// TODO -> richitg machen
static ssize_t spi_master_write(struct file *filp, const char __user *buf,
				size_t count, loff_t *offp)
{
	/*
	int led_intensity = 0;
	int missing_bytes = 0;
	int idx = 0;
	struct spi_master *data =
		container_of(filp->private_data, struct spi_master, misc);
	dev_info(&data->pdev->dev, "In %s. count: %d, off: %lld\n",
		 __func__, count, *offp);

	for (idx = 0; idx < count; idx++) {
		missing_bytes = copy_from_user(&led_intensity, buf + *offp, 1);

		// check if value in range -> ignore value when outside of range
		if (led_intensity <= 100 && led_intensity >= 0) {
			iowrite32(LED_ON * led_intensity / LED_INTENSITY_100,
				  data->registers);
			msleep(200);
		}
		*offp += 1;
	}
	return count - missing_bytes;
	*/
	return count;
}

static ssize_t spi_master_cpol_show(struct device *dev, struct device_attribute *attr, char *buf){

	struct spi_master *data = dev_get_drvdata(dev);
	int value = ioread32(data->registers);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);
}


static ssize_t spi_master_cpol_store(struct device *dev, struct device_attribute *attr,
         const char *buf, size_t count){

	int bit = 0;
	struct spi_master *data = dev_get_drvdata(dev);

	dev_info(&data->pdev->dev, "In %s. count: %d\n", __func__, count);

	// parse buf to bit and check format
	if (sscanf(buf, "%d", &bit) != 1){
		dev_err(&data->pdev->dev, "error in %s, invalid format: %s\n", __func__, buf);
		return -EINVAL;
	}

	// check value
	if (bit == 1 || bit == 0){
		// Print value of bit -> for testing
		dev_info(&data->pdev->dev, "In %s. Value read: %d\n", __func__, bit);
		iowrite32(bit ,data->registers);
	}
	else {
		dev_err(&data->pdev->dev, "error in %s, invalid value: %d\n", __func__, bit);
        return -EINVAL;
	}

    return count;
}


static ssize_t spi_master_cpha_show(struct device *dev, struct device_attribute *attr, char *buf){

	struct spi_master *data = dev_get_drvdata(dev);
	int value = ioread32(data->registers + 0x4);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);
}


static ssize_t spi_master_cpha_store(struct device *dev, struct device_attribute *attr,
         const char *buf, size_t count){

	int bit = 0;
	struct spi_master *data = dev_get_drvdata(dev);

	dev_info(&data->pdev->dev, "In %s. count: %d\n", __func__, count);

	// parse buf to bit and check format
	if (sscanf(buf, "%d", &bit) != 1){
		dev_err(&data->pdev->dev, "error in %s, invalid format: %s\n", __func__, buf);
		return -EINVAL;
	}

	// check value
	if (bit == 1 || bit == 0){
		// Print value of bit -> for testing
		dev_info(&data->pdev->dev, "In %s. Value read: %d\n", __func__, bit);
		iowrite32(bit ,data->registers + 0x4);
	}
	else {
		dev_err(&data->pdev->dev, "error in %s, invalid value: %d\n", __func__, bit);
        return -EINVAL;
	}

    return count;
}


static ssize_t spi_master_pre_delay_show(struct device *dev, struct device_attribute *attr, char *buf){

	struct spi_master *data = dev_get_drvdata(dev);
	int value = ioread32(data->registers + 0x8);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);
}


static ssize_t spi_master_pre_delay_store(struct device *dev, struct device_attribute *attr,
         const char *buf, size_t count){

	int bit = 0;
	struct spi_master *data = dev_get_drvdata(dev);

	dev_info(&data->pdev->dev, "In %s. count: %d\n", __func__, count);

	// parse buf to bit and check format
	if (sscanf(buf, "%d", &bit) != 1){
		dev_err(&data->pdev->dev, "error in %s, invalid format: %s\n", __func__, buf);
		return -EINVAL;
	}

	// check value
	if (bit < (1<<8) || bit >= 0){
		// Print value of bit -> for testing
		dev_info(&data->pdev->dev, "In %s. Value read: %d\n", __func__, bit);
		iowrite32(bit ,data->registers + 0x8);
	}
	else {
		dev_err(&data->pdev->dev, "error in %s, invalid value: %d\n", __func__, bit);
        return -EINVAL;
	}

    return count;
}


static ssize_t spi_master_post_delay_show(struct device *dev, struct device_attribute *attr, char *buf){

	struct spi_master *data = dev_get_drvdata(dev);
	int value = ioread32(data->registers + 0xC);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);


	value = ioread32(data->registers + 0xC);
}


static ssize_t spi_master_post_delay_store(struct device *dev, struct device_attribute *attr,
         const char *buf, size_t count){

	int bit = 0;
	struct spi_master *data = dev_get_drvdata(dev);

	dev_info(&data->pdev->dev, "In %s. count: %d\n", __func__, count);

	// parse buf to bit and check format
	if (sscanf(buf, "%d", &bit) != 1){
		dev_err(&data->pdev->dev, "error in %s, invalid format: %s\n", __func__, buf);
		return -EINVAL;
	}

	// check value
	if (bit < (1<<8) || bit >= 0){
		// Print value of bit -> for testing
		dev_info(&data->pdev->dev, "In %s. Value read: %d\n", __func__, bit);
		iowrite32(bit ,data->registers + 0xC);
	}
	else {
		dev_err(&data->pdev->dev, "error in %s, invalid value: %d\n", __func__, bit);
        return -EINVAL;
	}

    return count;
}


static ssize_t spi_master_clk_per_half_bit_show(struct device *dev, struct device_attribute *attr, char *buf){

	struct spi_master *data = dev_get_drvdata(dev);
	int value = ioread32(data->registers + 0x10);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);
}


static ssize_t spi_master_clk_per_half_bit_store(struct device *dev, struct device_attribute *attr,
         const char *buf, size_t count){

	int bit = 0;
	struct spi_master *data = dev_get_drvdata(dev);

	dev_info(&data->pdev->dev, "In %s. count: %d\n", __func__, count);

	// parse buf to bit and check format
	if (sscanf(buf, "%d", &bit) != 1){
		dev_err(&data->pdev->dev, "error in %s, invalid format: %s\n", __func__, buf);
		return -EINVAL;
	}

	// check value
	if (bit < (1<<16) || bit > 0){
		// Print value of bit -> for testing
		dev_info(&data->pdev->dev, "In %s. Value read: %d\n", __func__, bit);
		iowrite32(bit ,data->registers + 0x10);
	}
	else {
		dev_err(&data->pdev->dev, "error in %s, invalid value: %d\n", __func__, bit);
        return -EINVAL;
	}

    return count;
}

// TODO -> richitg machen
static ssize_t spi_master_gitrev_show(struct device *dev, struct device_attribute *attr, char *buf){
	
	struct spi_master *data = dev_get_drvdata(dev);
	int value = ioread32(data->registers + 0x10);

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);

}

// TODO -> richitg machen
static ssize_t spi_master_gitrev_store(struct device *dev, struct device_attribute *attr,
         const char *buf, size_t count){

	return count;
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver for the DPG project spi master");
MODULE_AUTHOR("Paul Braher");
