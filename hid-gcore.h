#ifndef HID_GCORE_H_INCLUDED
#define HID_GCORE_H_INCLUDED		1

/* See hid-gfb.h */
struct gfb_data;

/* Private driver data that is common for G-series drivers
 *
 * The model of the hid-gXX driver is an unique driver for all
 * devices contained within the specific keyboard (framebuffer, extra keys
 * and leds). Factoring common functionalities between drivers lead to
 * separate modules needing access to common shared data.
 *
 * All functions along different modules should be able to access their
 * specific data structures starting from this structure, attached to
 * the root hid device, by downcasting the data field to the appropriate
 * gXX_data structure.
 */
struct gcore_data {
	char *name;		       /* name of the device */

	struct hid_device *hdev;       /* hid device */
	struct input_dev *input_dev;   /* input device */
	struct gfb_data *gfb_data;     /* framebuffer (may be NULL) */
	int led_count;		       /* number of leds */
	struct led_classdev **led_cdev; /* led devices */

	spinlock_t lock;	       /* global device lock */

	void *data;		       /* specific driver data */
};


/* get the common private driver data from a hid_device */
#define hid_get_gdata(hdev) \
	((struct gcore_data *)(hid_get_drvdata(hdev)))

/* get the common private driver data from a generic device */
#define dev_get_gdata(dev) \
	((struct gcore_data *)(dev_get_drvdata(dev)))


/** Exported functions. */


/** Initialization helpers. */
struct gcore_data *gcore_alloc_data(const char *name, struct hid_device *hdev);
void gcore_free_data(struct gcore_data *gdata);

int gcore_hid_open(struct gcore_data *gdata);
void gcore_hid_close(struct gcore_data *gdata);

int gcore_input_probe(struct gcore_data *gdata,
		      const unsigned int default_keymap[], int keymap_size);
void gcore_input_remove(struct gcore_data *gdata);

int gcore_leds_probe(struct gcore_data *gdata,
		     const struct led_classdev led_templates[], int led_count);
void gcore_leds_remove(struct gcore_data *gdata);

struct hid_device *gcore_led_classdev_to_hdev(struct led_classdev *led_cdev);

/** Input helpers. */
void gcore_input_report_key(struct gcore_data *gdata, int scancode, int value);

/** Common sysfs attributes. */
ssize_t gcore_name_show(struct device *dev, struct device_attribute *attr,
			char *buf);
ssize_t gcore_name_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count);
ssize_t gcore_minor_show(struct device *dev, struct device_attribute *attr,
			 char *buf);

#endif
