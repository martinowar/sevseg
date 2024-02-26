#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>

#define INVALID_NUMBER 0xFFFFFFFF

#define SEVSEG_DRV_NAME      "sevseg"

#define SEG_ON  0
#define SEG_OFF 1

#define SEG_ARRAY_SIZE     8
#define SEG_MAP_SIZE      11
#define SELECT_ARRAY_SIZE  3
#define SELECT_MAP_SIZE    6

#define NUM_ELEMENTS_DEF   1
#define REFRESH_PERIOD_DEF 3

#define MS_TO_NS(x)        (x * 1000000L)

static struct gpio_descs * select_pins_array;

static unsigned char map_select_pins[SELECT_MAP_SIZE][SELECT_ARRAY_SIZE] = {
	  {0, 0, 0}
	, {0, 0, 1}
	, {0, 1, 0}
	, {0, 1, 1}
	, {1, 0, 0}
	, {1, 0, 1}
};

static struct gpio_descs * seg_pins_array;

/*
*  1_
*6 | | 2
* 7 -
*5 |_| 3
*   4
*/

static unsigned char map_num_to_seg_pins[SEG_MAP_SIZE][SEG_ARRAY_SIZE] = {
{SEG_ON,  SEG_ON,  SEG_ON,  SEG_ON,  SEG_ON,  SEG_ON,  SEG_OFF, SEG_OFF}, /*0*/
{SEG_OFF, SEG_ON,  SEG_ON,  SEG_OFF, SEG_OFF, SEG_OFF, SEG_OFF, SEG_OFF}, /*1*/
{SEG_ON,  SEG_ON,  SEG_OFF, SEG_ON,  SEG_ON,  SEG_OFF, SEG_ON,  SEG_OFF}, /*2*/
{SEG_ON,  SEG_ON,  SEG_ON,  SEG_ON,  SEG_OFF, SEG_OFF, SEG_ON,  SEG_OFF}, /*3*/
{SEG_OFF, SEG_ON,  SEG_ON,  SEG_OFF, SEG_OFF, SEG_ON,  SEG_ON,  SEG_OFF}, /*4*/
{SEG_ON,  SEG_OFF, SEG_ON,  SEG_ON,  SEG_OFF, SEG_ON,  SEG_ON,  SEG_OFF}, /*5*/
{SEG_ON,  SEG_OFF, SEG_ON,  SEG_ON,  SEG_ON,  SEG_ON,  SEG_ON,  SEG_OFF}, /*6*/
{SEG_ON,  SEG_ON,  SEG_ON,  SEG_OFF, SEG_OFF, SEG_OFF, SEG_OFF, SEG_OFF}, /*7*/
{SEG_ON,  SEG_ON,  SEG_ON,  SEG_ON,  SEG_ON,  SEG_ON,  SEG_ON,  SEG_OFF}, /*8*/
{SEG_ON,  SEG_ON,  SEG_ON,  SEG_ON,  SEG_OFF, SEG_ON,  SEG_ON,  SEG_OFF}, /*9*/
{SEG_ON,  SEG_OFF, SEG_OFF, SEG_ON,  SEG_ON,  SEG_ON,  SEG_ON,  SEG_OFF}  /*E*/
};

static struct hrtimer refresh_timer;
static ktime_t refresh_period;
static long nb_of_elements = NUM_ELEMENTS_DEF;
static long sevseg_number = 123456;
static unsigned char active_element; /* set to 0 by default */

static void set_sevseg_digit(unsigned char digit)
{
	int idx;

	for (idx = 0; idx <  SEG_ARRAY_SIZE; idx++)
	{
		gpiod_set_value(seg_pins_array->desc[idx], map_num_to_seg_pins[digit][idx]);
	}
}

static void activate_element(unsigned char active_element)
{
	int idx;

	for (idx = 0; idx < SELECT_ARRAY_SIZE; idx++)
	{
		gpiod_set_value(select_pins_array->desc[idx], map_select_pins[active_element][idx]);
	}
}

enum hrtimer_restart refresh_timer_handler(struct hrtimer *timer)
{
	ktime_t timer_time;
	unsigned char idx;
	long active_digit;

	if (sevseg_number != INVALID_NUMBER)  {
		active_digit = sevseg_number;
		for (idx = 0; idx < nb_of_elements - active_element - 1; idx++)
			active_digit = active_digit / 10;
		active_digit = active_digit % 10;
	} else
		active_digit = SEG_MAP_SIZE - 1;

	activate_element(active_element);
	set_sevseg_digit(active_digit);
	if (active_element > 0)
		active_element--;
	else
		active_element = nb_of_elements - 1;

	timer_time = hrtimer_cb_get_time(&refresh_timer);
	hrtimer_forward(&refresh_timer, timer_time, refresh_period);

	return HRTIMER_RESTART;
}

static ssize_t set_number_callback(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf,
				   size_t count)
{
	long max_number = 1;
	unsigned char idx;

	if (kstrtol(buf, 10, &sevseg_number) < 0)
		return -EINVAL;

	for (idx = 0; idx < nb_of_elements; idx++)
		max_number = max_number * 10;

	if (sevseg_number > max_number)
		sevseg_number = INVALID_NUMBER;

	printk("set_number_callback: The sevseg number is %ld", sevseg_number);

	return count;
}

static ssize_t set_period_callback(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf,
				   size_t count)
{
	ktime_t timer_time;
	long period;

	if (kstrtol(buf, 10, &period) < 0)
		return -EINVAL;

	if (period > 0)
		refresh_period = ktime_set(0, MS_TO_NS(period));
	else
	{
		refresh_period = ktime_set(0, MS_TO_NS(REFRESH_PERIOD_DEF));
		printk("set_period_callback: Invalid timer period: %ld. Setting the default value.", period);
	}

	timer_time = hrtimer_cb_get_time(&refresh_timer);
	hrtimer_forward(&refresh_timer, timer_time, refresh_period);
	printk("set_period_callback: The timer period is %ld", period);

	return count;
}

static ssize_t set_elements_callback(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf,
				     size_t count)
{
	long elements;

	if (kstrtol(buf, 10, &elements) < 0)
		return -EINVAL;

	if ((elements > 0) && (elements <= SELECT_MAP_SIZE))
		nb_of_elements = elements;
	else
		nb_of_elements = NUM_ELEMENTS_DEF;

	active_element = nb_of_elements - 1;
	printk("set_elements_callback: Number of elements %ld", nb_of_elements);

	return count;
}

/****************************/
/* Initialization / Cleanup */
/****************************/

static int sevseg_probe(struct platform_device *pdev);
static int sevseg_remove(struct platform_device *pdev);

static struct of_device_id sevseg_of_match[] = {
	{
		.compatible = SEVSEG_DRV_NAME,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sevseg_of_match);

static struct platform_driver sevseg_driver = {
	.probe = sevseg_probe,
	.remove = sevseg_remove,
	.driver = {
		.name = SEVSEG_DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = sevseg_of_match,
	},
};

static DEVICE_ATTR(period, 0660, NULL, set_period_callback);
static DEVICE_ATTR(elements, 0660, NULL, set_elements_callback);
static DEVICE_ATTR(number, 0660, NULL, set_number_callback);

static struct attribute *sevseg_attr[] = {
		&dev_attr_period.attr,
		&dev_attr_elements.attr,
		&dev_attr_number.attr,
		NULL
	};

static struct attribute_group sevseg_attr_group = {
	.attrs = sevseg_attr
};

static struct class *s_pDeviceClass;
static struct device *s_pDevObject;

static int sevseg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	unsigned int tmp_refresh_period;

	printk("sevseg_probe: BEGIN\n");
	
	if (!device_property_present(dev, "segment-pins-gpios")) {
		printk("sevseg_probe - Error! Device property 'segment-pins-gpios' not found!\n");
		return -1;
	}

	if (!device_property_present(dev, "select-pins-gpios")) {
		printk("sevseg_probe - Error! Device property 'select-pins-gpios' not found!\n");
		return -1;
	}

	if (!device_property_present(dev, "sevseg-number")) {
		printk("sevseg_probe - Error! Device property 'sevseg-number' not found!\n");
		return -1;
	}

	if (!device_property_present(dev, "refresh-period")) {
		printk("sevseg_probe - Error! Device property 'refresh-period' not found!\n");
		return -1;
	}

	if (!device_property_present(dev, "sevseg-elements")) {
		printk("sevseg_probe - Error! Device property 'sevseg-elements' not found!\n");
		return -1;
	}

	seg_pins_array = gpiod_get_array(dev, "segment-pins", GPIOD_OUT_HIGH);
	if (IS_ERR(seg_pins_array)) {
		printk("sevseg_probe - Error! Could not setup the GPIO\n");
		return -1 * IS_ERR(seg_pins_array);
	}

	select_pins_array = gpiod_get_array(dev, "select-pins", GPIOD_OUT_HIGH);
	if(IS_ERR(select_pins_array)) {
		printk("sevseg_probe - Error! Could not setup the GPIO\n");
		return -1 * IS_ERR(select_pins_array);
	}

	// TODO fix the sevseg_number type (U64 or U32)
	if (device_property_read_u32(dev, "sevseg-number", (unsigned int *)&sevseg_number)) {
		printk("sevseg_probe - Error! Could not read 'sevseg-number'\n");
		return -1;
	}

	if (device_property_read_u32(dev, "refresh-period", &tmp_refresh_period)) {
		printk("sevseg_probe - Error! Could not read 'refresh-period'\n");
		return -1;
	}

	// TODO fix the nb_of_elements type (U8)
	if (device_property_read_u32(dev, "sevseg-elements", (unsigned int *)&nb_of_elements)) {
		printk("sevseg_probe - Error! Could not read 'sevseg-elements'\n");
		return -1;
	}

	active_element = nb_of_elements - 1;

	s_pDeviceClass = class_create(THIS_MODULE, SEVSEG_DRV_NAME);
	if (s_pDeviceClass == NULL) {
		printk("sevseg_probe: cannot create class");
		return -1;;
	}

	s_pDevObject = device_create(s_pDeviceClass, NULL, 0, NULL,
					SEVSEG_DRV_NAME);
	if (s_pDevObject == NULL) {
		printk("sevseg_probe: cannot create device");
		goto err_class_create;
	}

	if (sysfs_create_group(&s_pDevObject->kobj, &sevseg_attr_group)) {
		printk("sevseg_probe: cannot create sysfs files");
		goto err_device_create;
	}

	refresh_period = ktime_set(0, MS_TO_NS(tmp_refresh_period));
	hrtimer_init(&refresh_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	refresh_timer.function = &refresh_timer_handler;
	hrtimer_start(&refresh_timer, refresh_period, HRTIMER_MODE_REL);

	printk("sevseg_probe: END\n");

	return 0;

err_device_create:
	device_destroy(s_pDeviceClass, 0);

err_class_create:
	class_destroy(s_pDeviceClass);

	printk("sevseg_probe: END with errors\n");

	return -1;
}

static int sevseg_remove(struct platform_device *pdev) {
	printk("sevseg_remove: BEGIN\n");

	gpiod_put_array(seg_pins_array);
	gpiod_put_array(select_pins_array);

	hrtimer_cancel(&refresh_timer);
	sysfs_remove_group(&s_pDevObject->kobj, &sevseg_attr_group);
	device_destroy(s_pDeviceClass, 0);
	class_destroy(s_pDeviceClass);

	printk("sevseg_remove: END\n");

	return 0;
}

int sevseg_init(void)
{
	int result;

	printk("sevseg_init: driver initialization...");

	result = platform_driver_register(&sevseg_driver);
	if (result) {
		printk("sevseg_init: driver register error %d\n", result);
		return result;
	}

	return 0;
}

void sevseg_cleanup(void)
{
	printk("sevseg_cleanup: driver removed!");

	platform_driver_unregister(&sevseg_driver);
}

MODULE_LICENSE("GPL");
module_init(sevseg_init);
module_exit(sevseg_cleanup);

