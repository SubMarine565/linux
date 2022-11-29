// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   A kernel module thats generates a running light
 */

/*
 *	Fragen:
 *	Funktionen hier sperren waerend anderer Treiber arbeitet?
 *	-> Keine Config setzen waerend dem Senden
 *
 * 	Unterordner in /sys/devices fuer unser Geraet
 * 
 * 	Files anlegen ohne Race conditions = ohne create_files oder create_group
 * 	-> .groups bei device oder driver funktioniert nicht
 * 
 * 	copy_from_user und copy_to_user bei sysfs devices notwendig?
 * 
 * 	Create group funktioniert nur wenn kobj von sysfs_device genommen wird,
 * 	nicht wenn pdev->dev.kobj genommen wird, wieso?
 * 	Genauso bei register device!
 * 	Wieso funktioniert pdev->dev nicht?
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
#include <linux/printk.h>

#define DRIVER_NAME "dpg_spi_master"
#define OK 0

// forward declaration
static int spi_master_probe(struct platform_device *pdev);
static int spi_master_remove(struct platform_device *pdev);
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


// device data struct
struct spi_master {
	u32 *registers; // dummy
	struct platform_device *pdev; // for print in read and write
};

static DEVICE_ATTR(cpol, 0664, spi_master_cpol_show, spi_master_cpol_store);
static DEVICE_ATTR(cpha, 0664, spi_master_cpha_show, spi_master_cpha_store);
static DEVICE_ATTR(pre_delay, 0664, spi_master_pre_delay_show, spi_master_pre_delay_store);
static DEVICE_ATTR(post_delay, 0664, spi_master_post_delay_show, spi_master_post_delay_store);
static DEVICE_ATTR(clk_per_half_bit, 0664, spi_master_clk_per_half_bit_show, spi_master_clk_per_half_bit_store);


static struct attribute *spi_attrs[] = {
        &dev_attr_cpol.attr,
		&dev_attr_cpha.attr,
		&dev_attr_pre_delay.attr,
		&dev_attr_post_delay.attr,
		&dev_attr_clk_per_half_bit.attr,
        NULL
};

// static struct attribute_group spi_group = {
//         //.name = "dpg",
//         .attrs = spi_attrs
// };

// static const struct attribute_group *spi_groups[] = {
//     &spi_group,
//     NULL
// };

// Makro creates spi_group and spi_groups
ATTRIBUTE_GROUPS(spi);

static struct device *sysfs_device;

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
			.groups = spi_groups	// TODO Hier richtig bzw. funktioniert nicht
			 },
	.probe = spi_master_probe,
	.remove = spi_master_remove
};
module_platform_driver(spi_master_driver);


// Probe function -> driver loading
static int spi_master_probe(struct platform_device *pdev)
{
	int status = 0;
	char deviceName[10];
	struct spi_master *spi_m = NULL;
	struct resource *io = NULL;
	static atomic_t idx = ATOMIC_INIT(-1);
	int deviceNumber = atomic_inc_return(&idx);
	// generate device names
	snprintf(deviceName, 10, "dpg_spi%d", deviceNumber);

	dev_info(&pdev->dev, "Function %s entered.\n", __func__);

	// alloc mem
	spi_m = devm_kzalloc(&pdev->dev, sizeof(*spi_m), GFP_KERNEL);
	if (IS_ERR(spi_m)) {
		dev_err(&pdev->dev, "Error in kzalloc.\n");
		return -EFAULT;
	}

	// set data
	platform_set_drvdata(pdev, spi_m);

	// get resources
	io = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (IS_ERR(io)) {
		dev_err(&pdev->dev, "Error in get resource.\n");
		return -EFAULT;
	}

	spi_m->registers = devm_ioremap_resource(&pdev->dev, io);
	if (IS_ERR(spi_m->registers)) {
		dev_err(&pdev->dev, "Error in ioremap.\n");
		return -EFAULT;
	}

	// TODO .groups oben beim driver dazugegeben -> Hat hier auch nicht funktioniert
	//pdev->dev.groups = spi_groups;


	// sysfs device
	sysfs_device = root_device_register(deviceName);
	if (IS_ERR(sysfs_device)) {
		printk(KERN_INFO "Unable to register device\n");
		status = -EEXIST;
		return -EFAULT;
		//goto remove_device;
	}
	dev_set_drvdata(sysfs_device, &spi_m);

	// TODO Unterordner in devices fuer spi device
	// https://www.codetd.com/en/article/7166817
	// Am Schluss Ordnerstrukut -> erzeugte Klasse als unterordner fuer device

	// Gruppe anlegen
 	status = sysfs_create_group(&sysfs_device->kobj, &spi_group);
    if (status) {
        dev_err(&pdev->dev, "sysfs creation failed\n");
        return status;
    }

	dev_info(&pdev->dev, "Leaving %s probe.\n", __func__);
	return OK;
}

// Remove function -> unload driver
static int spi_master_remove(struct platform_device *pdev)
{
	struct spi_master *spi_m = NULL;

	dev_info(&pdev->dev, "Function %s entered.\n", __func__);

	spi_m = platform_get_drvdata(pdev);


	// TODO -> Funktion noetig? -> files verschwinden sowieso wenn device zerstoert wird
	sysfs_remove_group(&sysfs_device->kobj, &spi_group);

	// unregister
	root_device_unregister(sysfs_device);

	// TODO Wie platfrom_... Auch 0 setzen?
	//dev_set_drvdata(sysfs_device, NULL);

	// set device to null
	platform_set_drvdata(pdev, NULL);


	dev_info(&pdev->dev, "Leaving %s remove.\n", __func__);
	return OK;
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

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver for setting values of the DPG project spi master");
MODULE_AUTHOR("Paul Braher");
