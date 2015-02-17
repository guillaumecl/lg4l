/***************************************************************************
 *   Copyright (C) 2010 by Alistair Buxton				   *
 *   a.j.buxton@gmail.com						   *
 *   based on hid-g13.c							   *
 *									   *
 *   This program is free software: you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation, either version 2 of the License, or	   *
 *   (at your option) any later version.				   *
 *									   *
 *   This driver is distributed in the hope that it will be useful, but	   *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of		   *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU	   *
 *   General Public License for more details.				   *
 *									   *
 *   You should have received a copy of the GNU General Public License	   *
 *   along with this software. If not see <http://www.gnu.org/licenses/>.  *
 ***************************************************************************/
#include <linux/hid.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>
#include <linux/leds.h>
#include <linux/completion.h>
#include <linux/version.h>

#include "hid-ids.h"
#include "hid-gcore.h"

#define G110_NAME "Logitech G110"

/* Key defines */
#define G110_KEYS 17

/* Backlight defaults */
#define G110_DEFAULT_RED (0)
#define G110_DEFAULT_BLUE (255)

/* LED array indices */
#define G110_LED_M1 0
#define G110_LED_M2 1
#define G110_LED_M3 2
#define G110_LED_MR 3
#define G110_LED_BL_R 4
#define G110_LED_BL_B 5

#define G110_REPORT_4_INIT	0x00
#define G110_REPORT_4_FINALIZE	0x01

#define G110_READY_SUBSTAGE_1 0x01
#define G110_READY_SUBSTAGE_2 0x02
#define G110_READY_SUBSTAGE_3 0x04
#define G110_READY_STAGE_1    0x07
#define G110_READY_SUBSTAGE_4 0x08
#define G110_READY_SUBSTAGE_5 0x10
#define G110_READY_STAGE_2    0x1F
#define G110_READY_SUBSTAGE_6 0x20
#define G110_READY_SUBSTAGE_7 0x40
#define G110_READY_STAGE_3    0x7F

#define G110_RESET_POST 0x01
#define G110_RESET_MESSAGE_1 0x02
#define G110_RESET_READY 0x03

/* G110-specific device data structure */
struct g110_data {
	/* HID reports */
	struct hid_report *backlight_report;
	struct hid_report *start_input_report;
	struct hid_report *feature_report_4;
	struct hid_report *led_report;
	struct hid_report *output_report_3;

	/* led state */
	u8 backlight_rb[2];	/* keyboard illumination */
	u8 led_mbtns;		/* m1, m2, m3 and mr */

	/* non-standard buttons */
	u8 ep1keys[2];
	struct urb *ep1_urb;
	spinlock_t ep1_urb_lock;

	/* initialization stages */
	struct completion ready;
	int ready_stages;
};

/* Convenience macros */
#define hid_get_g110data(hdev) \
	((struct g110_data *)(hid_get_gdata(hdev)->data))
#define dev_get_g110data(dev) \
	((struct g110_data *)(dev_get_gdata(dev)->data))

/*
 * Keymap array indices (used as scancodes)
 *
 * Key	      Index
 * ---------  ------
 * G1-G12     0-11
 * M1	      12
 * M2	      13
 * M3	      14
 * MR	      15
 * LIGHT      16
 */
static const unsigned int g110_default_keymap[G110_KEYS] = {
	KEY_F1, KEY_F2, KEY_F3, KEY_F4,
	KEY_F5, KEY_F6, KEY_F7, KEY_F8,
	KEY_F9, KEY_F10, KEY_F11, KEY_F12,
	/* M1, M2, M3, MR */
	KEY_PROG1, KEY_PROG2, KEY_PROG3, KEY_RECORD,
	KEY_KBDILLUMTOGGLE
};

static void g110_led_mbtns_send(struct hid_device *hdev)
{
	struct g110_data *g110data = hid_get_g110data(hdev);

	g110data->led_report->field[0]->value[0] = g110data->led_mbtns & 0xFF;

	hid_hw_request(hdev, g110data->led_report, HID_REQ_SET_REPORT);
}

static void g110_led_mbtns_brightness_set(struct led_classdev *led_cdev,
					  enum led_brightness value)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g110_data *g110data = gdata->data;
	u8 mask = 0;

	if (led_cdev == gdata->led_cdev[G110_LED_M1])
		mask = 0x80;
	else if (led_cdev == gdata->led_cdev[G110_LED_M2])
		mask = 0x40;
	else if (led_cdev == gdata->led_cdev[G110_LED_M3])
		mask = 0x20;
	else if (led_cdev == gdata->led_cdev[G110_LED_MR])
		mask = 0x10;

	if (mask && value)
		g110data->led_mbtns |= mask;
	else
		g110data->led_mbtns &= ~mask;

	g110_led_mbtns_send(hdev);
}

static enum led_brightness
g110_led_mbtns_brightness_get(struct led_classdev *led_cdev)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g110_data *g110data = gdata->data;
	int value = 0;

	if (led_cdev == gdata->led_cdev[G110_LED_M1])
		value = g110data->led_mbtns & 0x80;
	else if (led_cdev == gdata->led_cdev[G110_LED_M2])
		value = g110data->led_mbtns & 0x40;
	else if (led_cdev == gdata->led_cdev[G110_LED_M3])
		value = g110data->led_mbtns & 0x20;
	else if (led_cdev == gdata->led_cdev[G110_LED_MR])
		value = g110data->led_mbtns & 0x10;
	else
		dev_err(&hdev->dev,
			G110_NAME " error retrieving LED brightness\n");

	if (value)
		return LED_FULL;
	return LED_OFF;
}

static void g110_led_bl_send(struct hid_device *hdev)
{
	struct g110_data *g110data = hid_get_g110data(hdev);

	struct hid_field *field0 = g110data->backlight_report->field[0];
	struct hid_field *field1 = g110data->backlight_report->field[1];

	/*
	 * Unlike the other keyboards, the G110 only has 2 LED backlights (red
	 * and blue). Rather than just setting intensity on each, the keyboard
	 * instead has a single intensity value, and a second value to specify
	 * how red/blue the backlight should be. This weird logic converts the
	 * two intensity values from the user into an intensity/colour value
	 * suitable for the keyboard.
	 *
	 * Additionally, the intensity is only valid from 0x00 - 0x0f (rather
	 * than 0x00 - 0xff). I decided to keep accepting 0x00 - 0xff as input,
	 * and I just >>4 to make it fit.
	 */

	/* These are just always zero from what I can tell */
	field0->value[1] = 0x00;
	field0->value[2] = 0x00;

	/* If the intensities are the same, "colour" is 0x80 */
	if (g110data->backlight_rb[0] == g110data->backlight_rb[1]) {
		field0->value[0] = 0x80;
		field1->value[0] = g110data->backlight_rb[0]>>4;
	}
	/* If the blue value is higher */
	else if (g110data->backlight_rb[1] > g110data->backlight_rb[0]) {
		field0->value[0] = 0xff - (0x80 * g110data->backlight_rb[0]) /
			g110data->backlight_rb[1];
		field1->value[0] = g110data->backlight_rb[1]>>4;
	}
	/* If the red value is higher */
	else {
		field0->value[0] = (0x80 * g110data->backlight_rb[1]) /
			g110data->backlight_rb[0];
		field1->value[0] = g110data->backlight_rb[0]>>4;
	}

	hid_hw_request(hdev, g110data->backlight_report, HID_REQ_SET_REPORT);
}

static void g110_led_bl_brightness_set(struct led_classdev *led_cdev,
				       enum led_brightness value)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g110_data *g110data = gdata->data;

	if (led_cdev == gdata->led_cdev[G110_LED_BL_R])
		g110data->backlight_rb[0] = value;
	else if (led_cdev == gdata->led_cdev[G110_LED_BL_B])
		g110data->backlight_rb[1] = value;

	g110_led_bl_send(hdev);
}

static enum led_brightness
g110_led_bl_brightness_get(struct led_classdev *led_cdev)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g110_data *g110data = gdata->data;
	int value = 0;

	if (led_cdev == gdata->led_cdev[G110_LED_BL_R])
		value = g110data->backlight_rb[0];
	else if (led_cdev == gdata->led_cdev[G110_LED_BL_B])
		value = g110data->backlight_rb[1];
	else
		dev_err(&hdev->dev, G110_NAME " error retrieving LED brightness\n");

	if (value)
		return LED_FULL;
	return LED_OFF;
}


static const struct led_classdev g110_led_cdevs[] = {
	{
		.name			= "g110_%d:orange:m1",
		.brightness_set		= g110_led_mbtns_brightness_set,
		.brightness_get		= g110_led_mbtns_brightness_get,
	},
	{
		.name			= "g110_%d:orange:m2",
		.brightness_set		= g110_led_mbtns_brightness_set,
		.brightness_get		= g110_led_mbtns_brightness_get,
	},
	{
		.name			= "g110_%d:orange:m3",
		.brightness_set		= g110_led_mbtns_brightness_set,
		.brightness_get		= g110_led_mbtns_brightness_get,
	},
	{
		.name			= "g110_%d:red:mr",
		.brightness_set		= g110_led_mbtns_brightness_set,
		.brightness_get		= g110_led_mbtns_brightness_get,
	},
	{
		.name			= "g110_%d:red:bl",
		.brightness_set		= g110_led_bl_brightness_set,
		.brightness_get		= g110_led_bl_brightness_get,
	},
	{
		.name			= "g110_%d:blue:bl",
		.brightness_set		= g110_led_bl_brightness_set,
		.brightness_get		= g110_led_bl_brightness_get,
	},
};

static DEVICE_ATTR(name, 0664, gcore_name_show, gcore_name_store);
static DEVICE_ATTR(minor, 0444, gcore_minor_show, NULL);

static struct attribute *g110_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_minor.attr,
	NULL,	 /* need to NULL terminate the list of attributes */
};

static struct attribute_group g110_attr_group = {
	.attrs = g110_attrs,
};


static void g110_raw_event_process_input(struct hid_device *hdev,
					 struct gcore_data *gdata,
					 u8 *raw_data)
{
	struct input_dev *idev = gdata->input_dev;
	int scancode;
	int value;
	int i;
	int mask;

	raw_data[3] &= 0xBF; /* bit 6 is always on */

	for (i = 0, mask = 0x01; i < 8; i++, mask <<= 1) {
		/* Keys G1 through G8 */
		scancode = i;
		value = raw_data[1] & mask;
		gcore_input_report_key(gdata, scancode, value);

		/* Keys G9 through MR */
		scancode = i + 8;
		value = raw_data[2] & mask;
		gcore_input_report_key(gdata, scancode, value);

		/* Key Light Only */
		if (i == 0) {
			scancode = i + 16;
			value = raw_data[3] & mask;
			gcore_input_report_key(gdata, scancode, value);
		}

	}

	input_sync(idev);
}

static int g110_raw_event(struct hid_device *hdev,
			  struct hid_report *report,
			  u8 *raw_data, int size)
{
	struct gcore_data *gdata = dev_get_gdata(&hdev->dev);
	struct g110_data *g110data = gdata->data;
	unsigned long irq_flags;

	/*
	* On initialization receive a 258 byte message with
	* data = 6 0 255 255 255 255 255 255 255 255 ...
	*/

	spin_lock_irqsave(&gdata->lock, irq_flags);

	if (unlikely(g110data->ready_stages != G110_READY_STAGE_3)) {
		switch (report->id) {
		case 6:
			if (!(g110data->ready_stages & G110_READY_SUBSTAGE_1))
				g110data->ready_stages |= G110_READY_SUBSTAGE_1;
			else if (g110data->ready_stages & G110_READY_SUBSTAGE_4 &&
				 !(g110data->ready_stages & G110_READY_SUBSTAGE_5))
				g110data->ready_stages |= G110_READY_SUBSTAGE_5;
			else if (g110data->ready_stages & G110_READY_SUBSTAGE_6 &&
				 raw_data[1] >= 0x80)
				g110data->ready_stages |= G110_READY_SUBSTAGE_7;
			break;
		case 1:
			if (!(g110data->ready_stages & G110_READY_SUBSTAGE_2))
				g110data->ready_stages |= G110_READY_SUBSTAGE_2;
			else
				g110data->ready_stages |= G110_READY_SUBSTAGE_3;
			break;
		}

		if (g110data->ready_stages == G110_READY_STAGE_1 ||
		    g110data->ready_stages == G110_READY_STAGE_2 ||
		    g110data->ready_stages == G110_READY_STAGE_3)
			complete_all(&g110data->ready);

		spin_unlock_irqrestore(&gdata->lock, irq_flags);
		return 1;
	}

	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	if (likely(report->id == 2)) {
		g110_raw_event_process_input(hdev, gdata, raw_data);
		return 1;
	}

	return 0;
}

#ifdef CONFIG_PM

static int g110_resume(struct hid_device *hdev)
{
	unsigned long irq_flags;
	struct gcore_data *gdata = hid_get_gdata(hdev);

	spin_lock_irqsave(&gdata->lock, irq_flags);
	g110_led_bl_send(hdev);
	g110_led_mbtns_send(hdev);
	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	return 0;
}

static int g110_reset_resume(struct hid_device *hdev)
{
	return g110_resume(hdev);
}

#endif /* CONFIG_PM */

/***** probe-related functions *****/

static void g110_feature_report_4_send(struct hid_device *hdev, int which)
{
	struct g110_data *g110data = hid_get_g110data(hdev);

	if (which == G110_REPORT_4_INIT) {
		g110data->feature_report_4->field[0]->value[0] = 0x02;
		g110data->feature_report_4->field[0]->value[1] = 0x00;
		g110data->feature_report_4->field[0]->value[2] = 0x00;
		g110data->feature_report_4->field[0]->value[3] = 0x00;
	} else if (which == G110_REPORT_4_FINALIZE) {
		g110data->feature_report_4->field[0]->value[0] = 0x02;
		g110data->feature_report_4->field[0]->value[1] = 0x80;
		g110data->feature_report_4->field[0]->value[2] = 0x00;
		g110data->feature_report_4->field[0]->value[3] = 0xFF;
	} else {
		return;
	}

	hid_hw_request(hdev, g110data->feature_report_4, HID_REQ_SET_REPORT);
}


/* Unlock the urb so we can reuse it */
static void g110_ep1_urb_completion(struct urb *urb)
{
	struct hid_device *hdev = urb->context;
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g110_data *g110data = gdata->data;
	struct input_dev *idev = gdata->input_dev;
	int i;

	for (i = 0; i < 8; i++)
		gcore_input_report_key(gdata, 24+i,
				       g110data->ep1keys[0]&(1<<i));

	input_sync(idev);

	usb_submit_urb(urb, GFP_ATOMIC);
}

static int g110_ep1_read(struct hid_device *hdev)
{
	struct usb_interface *intf;
	struct usb_device *usb_dev;
	struct g110_data *g110data = hid_get_g110data(hdev);

	struct usb_host_endpoint *ep;
	unsigned int pipe;
	int retval = 0;

	/* Get the usb device to send the image on */
	intf = to_usb_interface(hdev->dev.parent);
	usb_dev = interface_to_usbdev(intf);

	pipe = usb_rcvintpipe(usb_dev, 0x01);
	ep = (usb_pipein(pipe) ?
	      usb_dev->ep_in : usb_dev->ep_out)[usb_pipeendpoint(pipe)];

	if (unlikely(!ep))
		return -EINVAL;

	usb_fill_int_urb(g110data->ep1_urb, usb_dev, pipe, g110data->ep1keys, 2,
			 g110_ep1_urb_completion, NULL, 10);
	g110data->ep1_urb->context = hdev;
	g110data->ep1_urb->actual_length = 0;

	retval = usb_submit_urb(g110data->ep1_urb, GFP_KERNEL);

	return retval;
}

static int read_feature_reports(struct gcore_data *gdata)
{
	struct hid_device *hdev = gdata->hdev;
	struct g110_data *g110data = gdata->data;

	struct list_head *feature_report_list =
			    &hdev->report_enum[HID_FEATURE_REPORT].report_list;
	struct hid_report *report;

	if (list_empty(feature_report_list)) {
		dev_err(&hdev->dev,
			"%s no feature report found\n",
			gdata->name);
		return -ENODEV;
	}
	dbg_hid("%s feature report found\n", gdata->name);

	list_for_each_entry(report, feature_report_list, list) {
		switch (report->id) {
		case 0x03:
			g110data->feature_report_4 = report;
			g110data->start_input_report = report;
			g110data->led_report = report;
			break;
		case 0x07:
			g110data->backlight_report = report;
			break;
		default:
			break;
		}
		dbg_hid("%s Feature report: id=%u type=%u size=%u maxfield=%u report_count=%u\n",
			gdata->name,
			report->id, report->type, report->size,
			report->maxfield, report->field[0]->report_count);
	}

	dbg_hid("%s found all reports\n", gdata->name);

	return 0;
}

static void wait_ready(struct gcore_data *gdata)
{
	struct g110_data *g110data = gdata->data;
	struct hid_device *hdev = gdata->hdev;
	unsigned long irq_flags;

	dbg_hid("Waiting for G110 to activate\n");

	/*
	 * Wait here for stage 1 (substages 1-3) to complete
	 */
	wait_for_completion_timeout(&g110data->ready, HZ);

	/* Protect data->ready_stages */
	spin_lock_irqsave(&gdata->lock, irq_flags);
	if (g110data->ready_stages != G110_READY_STAGE_1) {
		dev_warn(&hdev->dev,
			 "%s hasn't completed stage 1 yet, forging ahead with initialization\n",
			 gdata->name);
		/* Force the stage */
		g110data->ready_stages = G110_READY_STAGE_1;
	}
	init_completion(&g110data->ready);
	g110data->ready_stages |= G110_READY_SUBSTAGE_4;
	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	/*
	 * Send the init report, then follow with the input report to trigger
	 * report 6 and wait for us to get a response.
	 */
	g110_feature_report_4_send(hdev, G110_REPORT_4_INIT);
	hid_hw_request(hdev, g110data->start_input_report, HID_REQ_GET_REPORT);
	wait_for_completion_timeout(&g110data->ready, HZ);

	/* Protect data->ready_stages */
	spin_lock_irqsave(&gdata->lock, irq_flags);
	if (g110data->ready_stages != G110_READY_STAGE_2) {
		dev_warn(&hdev->dev,
			 "%s hasn't completed stage 2 yet, forging ahead with initialization\n",
			 gdata->name);
		/* Force the stage */
		g110data->ready_stages = G110_READY_STAGE_2;
	}
	init_completion(&g110data->ready);
	g110data->ready_stages |= G110_READY_SUBSTAGE_6;
	spin_unlock_irqrestore(&gdata->lock, irq_flags);
}

static void send_finalize_report(struct gcore_data *gdata)
{
	struct g110_data *g110data = gdata->data;
	struct hid_device *hdev = gdata->hdev;
	unsigned long irq_flags;

	/*
	 * Send the finalize report, then follow with the input report to
	 * trigger report 6 and wait for us to get a response.
	 */
	g110_feature_report_4_send(hdev, G110_REPORT_4_FINALIZE);
	hid_hw_request(hdev, g110data->start_input_report, HID_REQ_GET_REPORT);
	hid_hw_request(hdev, g110data->start_input_report, HID_REQ_GET_REPORT);
	wait_for_completion_timeout(&g110data->ready, HZ);

	/* Protect data->ready_stages */
	spin_lock_irqsave(&gdata->lock, irq_flags);

	if (g110data->ready_stages != G110_READY_STAGE_3) {
		dev_warn(&hdev->dev,
			 "%s hasn't completed stage 3 yet, forging ahead with initialization\n",
			 gdata->name);
		/* Force the stage */
		g110data->ready_stages = G110_READY_STAGE_3;
	} else {
		dbg_hid("%s stage 3 complete\n", gdata->name);
	}

	spin_unlock_irqrestore(&gdata->lock, irq_flags);
}

static int g110_probe(struct hid_device *hdev,
		      const struct hid_device_id *id)
{
	int error;
	struct gcore_data *gdata;
	struct g110_data *g110data;

	dev_dbg(&hdev->dev, "Logitech G110 HID hardware probe...");

	gdata = gcore_alloc_data(G110_NAME, hdev);
	if (gdata == NULL) {
		dev_err(&hdev->dev,
			G110_NAME
			" can't allocate space for device attributes\n");
		error = -ENOMEM;
		goto err_no_cleanup;
	}

	g110data = kzalloc(sizeof(struct g110_data), GFP_KERNEL);
	if (g110data == NULL) {
		error = -ENOMEM;
		goto err_cleanup_gdata;
	}
	gdata->data = g110data;
	init_completion(&g110data->ready);

	g110data->ep1_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (g110data->ep1_urb == NULL) {
		dev_err(&hdev->dev,
			"%s: ERROR: can't alloc ep1 urb stuff\n",
			gdata->name);
		error = -ENOMEM;
		goto err_cleanup_g110data;
	}

	error = gcore_hid_open(gdata);
	if (error) {
		dev_err(&hdev->dev,
			"%s error opening hid device\n",
			gdata->name);
		goto err_cleanup_ep1_urb;
	}

	error = gcore_input_probe(gdata, g110_default_keymap,
				  ARRAY_SIZE(g110_default_keymap));
	if (error) {
		dev_err(&hdev->dev,
			"%s error registering input device\n",
			gdata->name);
		goto err_cleanup_hid;
	}

	error = read_feature_reports(gdata);
	if (error) {
		dev_err(&hdev->dev,
			"%s error reading feature reports\n",
			gdata->name);
		goto err_cleanup_input;
	}

	error = gcore_leds_probe(gdata, g110_led_cdevs,
				 ARRAY_SIZE(g110_led_cdevs));
	if (error) {
		dev_err(&hdev->dev, "%s error registering leds\n", gdata->name);
		goto err_cleanup_input;
	}

	error = sysfs_create_group(&(hdev->dev.kobj), &g110_attr_group);
	if (error) {
		dev_err(&hdev->dev,
			"%s failed to create sysfs group attributes\n",
			gdata->name);
		goto err_cleanup_leds;
	}

	wait_ready(gdata);

	/*
	 * Clear the LEDs
	 */
	g110data->backlight_rb[0] = G110_DEFAULT_RED;
	g110data->backlight_rb[1] = G110_DEFAULT_BLUE;

	g110_led_mbtns_send(hdev);
	g110_led_bl_send(hdev);

	send_finalize_report(gdata);

	error = g110_ep1_read(hdev);
	if (error) {
		dev_err(&hdev->dev, "%s failed to read ep1\n", gdata->name);
		goto err_cleanup_sysfs;
	}

	dbg_hid("G110 activated and initialized\n");

	/* Everything went well */
	return 0;

err_cleanup_sysfs:
	sysfs_remove_group(&(hdev->dev.kobj), &g110_attr_group);

err_cleanup_leds:
	gcore_leds_remove(gdata);

err_cleanup_input:
	gcore_input_remove(gdata);

err_cleanup_hid:
	gcore_hid_close(gdata);

err_cleanup_ep1_urb:
	usb_free_urb(g110data->ep1_urb);

err_cleanup_g110data:
	kfree(g110data);

err_cleanup_gdata:
	gcore_free_data(gdata);

err_no_cleanup:
	hid_set_drvdata(hdev, NULL);
	return error;
}

static void g110_remove(struct hid_device *hdev)
{
	struct gcore_data *gdata = hid_get_drvdata(hdev);
	struct g110_data *g110data = gdata->data;

	usb_poison_urb(g110data->ep1_urb);

	sysfs_remove_group(&(hdev->dev.kobj), &g110_attr_group);

	gcore_leds_remove(gdata);
	gcore_input_remove(gdata);
	gcore_hid_close(gdata);

	usb_free_urb(g110data->ep1_urb);

	kfree(g110data);
	gcore_free_data(gdata);
}

static const struct hid_device_id g110_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_G110) },
	{ }
};
MODULE_DEVICE_TABLE(hid, g110_devices);

static struct hid_driver g110_driver = {
	.name			= "hid-g110",
	.id_table		= g110_devices,
	.probe			= g110_probe,
	.remove			= g110_remove,
	.raw_event		= g110_raw_event,

#ifdef CONFIG_PM
	.resume			= g110_resume,
	.reset_resume		= g110_reset_resume,
#endif
};

static int __init g110_init(void)
{
	return hid_register_driver(&g110_driver);
}

static void __exit g110_exit(void)
{
	hid_unregister_driver(&g110_driver);
}

module_init(g110_init);
module_exit(g110_exit);
MODULE_DESCRIPTION("Logitech G110 HID Driver");
MODULE_AUTHOR("Alistair Buxton (a.j.buxton@gmail.com)");
MODULE_AUTHOR("Ciubotariu Ciprian (cheepeero@gmx.net)");
MODULE_LICENSE("GPL");
