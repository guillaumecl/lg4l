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
#include <linux/fb.h>
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
#include "hid-gfb.h"

#define G19_NAME "Logitech G19"

/* Key defines */
#define G19_KEYS 32

/* Backlight defaults */
#define G19_DEFAULT_RED (0)
#define G19_DEFAULT_GREEN (255)
#define G19_DEFAULT_BLUE (0)
#define G19_DEFAULT_BRIGHTNESS (80)

/* LED array indices */
#define G19_LED_M1 0
#define G19_LED_M2 1
#define G19_LED_M3 2
#define G19_LED_MR 3
#define G19_LED_BL_R 4
#define G19_LED_BL_G 5
#define G19_LED_BL_B 6
#define G19_LED_BL_SCREEN 7

/* Housekeeping stuff */
#define G19_REPORT_4_INIT	0x00
#define G19_REPORT_4_FINALIZE	0x01

#define G19_READY_SUBSTAGE_1 0x01
#define G19_READY_SUBSTAGE_2 0x02
#define G19_READY_SUBSTAGE_3 0x04
#define G19_READY_STAGE_1    0x07
#define G19_READY_SUBSTAGE_4 0x08
#define G19_READY_SUBSTAGE_5 0x10
#define G19_READY_STAGE_2    0x1F
#define G19_READY_SUBSTAGE_6 0x20
#define G19_READY_SUBSTAGE_7 0x40
#define G19_READY_STAGE_3    0x7F

#define G19_RESET_POST 0x01
#define G19_RESET_MESSAGE_1 0x02
#define G19_RESET_READY 0x03


/* G19-specific device data structure */
struct g19_data {
	/* HID reports */
	struct hid_report *backlight_report;
	struct hid_report *start_input_report;
	struct hid_report *feature_report_4;
	struct hid_report *led_report;
	struct hid_report *output_report_3;

	/* led state */
	u8 backlight_rgb[3];	/* keyboard illumination */
	u8 led_mbtns;		/* m1, m2, m3 and mr */
	u8 screen_bl;		/* lcd backlight */

	/* non-standard buttons */
	u8 ep1keys[2];
	struct urb *ep1_urb;
	spinlock_t ep1_urb_lock;

	/* initialization stages */
	struct completion ready;
	int ready_stages;
};

/* Convenience macros */
#define hid_get_g19data(hdev) \
	((struct g19_data *)(hid_get_gdata(hdev)->data))
#define dev_get_g19data(dev) \
	((struct g19_data *)(dev_get_gdata(dev)->data))


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
 * LIGHT      19
 * GEAR	      24
 * BACK	      25
 * MENU	      26
 * OK	      27
 * RIGHT      28
 * LEFT	      29
 * DOWN	      30
 * UP	      31
 */
static const unsigned int g19_default_keymap[G19_KEYS] = {
	/* G1 - G12 */
	KEY_F1, KEY_F2, KEY_F3, KEY_F4,
	KEY_F5, KEY_F6, KEY_F7, KEY_F8,
	KEY_F9, KEY_F10, KEY_F11, KEY_F12,

	/* M1, M2, M3, MR */
	KEY_PROG1, KEY_PROG2, KEY_PROG3, KEY_RECORD,

	/* backlight toggle */
	KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_KBDILLUMTOGGLE,
	KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN,

	/* menu keys */
	KEY_FORWARD, KEY_BACK, KEY_MENU, KEY_OK,
	KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
};

static void g19_led_mbtns_send(struct hid_device *hdev)
{
	struct g19_data *g19data = hid_get_g19data(hdev);

	g19data->led_report->field[0]->value[0] = g19data->led_mbtns & 0xFF;

	hid_hw_request(hdev, g19data->led_report, HID_REQ_SET_REPORT);
}

static void g19_led_mbtns_brightness_set(struct led_classdev *led_cdev,
					 enum led_brightness value)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g19_data *g19data = gdata->data;
	u8 mask = 0;

	if (led_cdev == gdata->led_cdev[G19_LED_M1])
		mask = 0x80;
	else if (led_cdev == gdata->led_cdev[G19_LED_M2])
		mask = 0x40;
	else if (led_cdev == gdata->led_cdev[G19_LED_M3])
		mask = 0x20;
	else if (led_cdev == gdata->led_cdev[G19_LED_MR])
		mask = 0x10;

	if (mask && value)
		g19data->led_mbtns |= mask;
	else
		g19data->led_mbtns &= ~mask;

	g19_led_mbtns_send(hdev);
}

static enum led_brightness
g19_led_mbtns_brightness_get(struct led_classdev *led_cdev)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g19_data *g19data = gdata->data;
	int value = 0;

	if (led_cdev == gdata->led_cdev[G19_LED_M1])
		value = g19data->led_mbtns & 0x80;
	else if (led_cdev == gdata->led_cdev[G19_LED_M2])
		value = g19data->led_mbtns & 0x40;
	else if (led_cdev == gdata->led_cdev[G19_LED_M3])
		value = g19data->led_mbtns & 0x20;
	else if (led_cdev == gdata->led_cdev[G19_LED_MR])
		value = g19data->led_mbtns & 0x10;
	else
		dev_err(&hdev->dev,
			G19_NAME " error retrieving LED brightness\n");

	if (value)
		return LED_FULL;
	return LED_OFF;
}

static void g19_led_bl_send(struct hid_device *hdev)
{
	struct g19_data *g19data = hid_get_g19data(hdev);

	struct hid_field *field0 = g19data->backlight_report->field[0];

	field0->value[0] = g19data->backlight_rgb[0];
	field0->value[1] = g19data->backlight_rgb[1];
	field0->value[2] = g19data->backlight_rgb[2];

	hid_hw_request(hdev, g19data->backlight_report, HID_REQ_SET_REPORT);
}

static void g19_led_bl_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g19_data *g19data = gdata->data;

	if (led_cdev == gdata->led_cdev[G19_LED_BL_R])
		g19data->backlight_rgb[0] = value;
	else if (led_cdev == gdata->led_cdev[G19_LED_BL_G])
		g19data->backlight_rgb[1] = value;
	else if (led_cdev == gdata->led_cdev[G19_LED_BL_B])
		g19data->backlight_rgb[2] = value;

	g19_led_bl_send(hdev);
}

static enum led_brightness
g19_led_bl_brightness_get(struct led_classdev *led_cdev)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g19_data *g19data = gdata->data;

	if (led_cdev == gdata->led_cdev[G19_LED_BL_R])
		return g19data->backlight_rgb[0];
	else if (led_cdev == gdata->led_cdev[G19_LED_BL_G])
		return g19data->backlight_rgb[1];
	else if (led_cdev == gdata->led_cdev[G19_LED_BL_B])
		return g19data->backlight_rgb[2];

	dev_err(&hdev->dev, G19_NAME " error retrieving LED brightness\n");
	return LED_OFF;
}

static void g19_led_screen_bl_send(struct hid_device *hdev)
{
	struct usb_interface *intf;
	struct usb_device *usb_dev;
	struct g19_data *g19data = hid_get_g19data(hdev);
	unsigned int pipe;
	int i = 0;

	unsigned char cp[9];

	cp[0] = g19data->screen_bl;
	cp[1] = 0xe2;
	cp[2] = 0x12;
	cp[3] = 0x00;
	cp[4] = 0x8c;
	cp[5] = 0x11;
	cp[6] = 0x00;
	cp[7] = 0x10;
	cp[8] = 0x00;

	intf = to_usb_interface(hdev->dev.parent);
	usb_dev = interface_to_usbdev(intf);
	pipe = usb_sndctrlpipe(usb_dev, 0x00);
	i = usb_control_msg(usb_dev, pipe, 0x0a,
			    USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
			    0, 0, cp, sizeof(cp),
			    1 * HZ);
	if (i < 0) {
		dev_warn(&hdev->dev,
			 G19_NAME " error setting LCD backlight level %d\n",
			 i);
	}
}

static void g19_led_screen_bl_set(struct led_classdev *led_cdev,
				  enum led_brightness value)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g19_data *g19data = gdata->data;

	if (led_cdev == gdata->led_cdev[G19_LED_BL_SCREEN]) {
		if (value > 100)
			value = 100;
		g19data->screen_bl = value;
		g19_led_screen_bl_send(hdev);
	}
}

static enum led_brightness g19_led_screen_bl_get(struct led_classdev *led_cdev)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g19_data *g19data = gdata->data;

	if (led_cdev == gdata->led_cdev[G19_LED_BL_SCREEN])
		return g19data->screen_bl;

	dev_err(&hdev->dev, G19_NAME " error retrieving LED brightness\n");
	return LED_OFF;
}


/* use the name field to convery a format string, */
/* that will be used by gcore_leds_probe */
static const struct led_classdev g19_led_cdevs[] = {
	{
		.name			= "g19_%d:orange:m1",
		.brightness_set		= g19_led_mbtns_brightness_set,
		.brightness_get		= g19_led_mbtns_brightness_get,
	},
	{
		.name			= "g19_%d:orange:m2",
		.brightness_set		= g19_led_mbtns_brightness_set,
		.brightness_get		= g19_led_mbtns_brightness_get,
	},
	{
		.name			= "g19_%d:orange:m3",
		.brightness_set		= g19_led_mbtns_brightness_set,
		.brightness_get		= g19_led_mbtns_brightness_get,
	},
	{
		.name			= "g19_%d:red:mr",
		.brightness_set		= g19_led_mbtns_brightness_set,
		.brightness_get		= g19_led_mbtns_brightness_get,
	},
	{
		.name			= "g19_%d:red:bl",
		.brightness_set		= g19_led_bl_brightness_set,
		.brightness_get		= g19_led_bl_brightness_get,
	},
	{
		.name			= "g19_%d:green:bl",
		.brightness_set		= g19_led_bl_brightness_set,
		.brightness_get		= g19_led_bl_brightness_get,
	},
	{
		.name			= "g19_%d:blue:bl",
		.brightness_set		= g19_led_bl_brightness_set,
		.brightness_get		= g19_led_bl_brightness_get,
	},
	{
		.name			= "g19_%d:white:screen",
		.brightness_set		= g19_led_screen_bl_set,
		.brightness_get		= g19_led_screen_bl_get,
	},
};

static DEVICE_ATTR(fb_node, 0444, gfb_fb_node_show, NULL);
static DEVICE_ATTR(fb_update_rate, 0664,
		   gfb_fb_update_rate_show, gfb_fb_update_rate_store);
static DEVICE_ATTR(name, 0664, gcore_name_show, gcore_name_store);
static DEVICE_ATTR(minor, 0444, gcore_minor_show, NULL);

static struct attribute *g19_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_minor.attr,
	&dev_attr_fb_update_rate.attr,
	&dev_attr_fb_node.attr,
	NULL,	 /* need to NULL terminate the list of attributes */
};

static struct attribute_group g19_attr_group = {
	.attrs = g19_attrs,
};


static void g19_raw_event_process_input(struct hid_device *hdev,
					struct gcore_data *gdata,
					u8 *raw_data)
{
	int scancode, value;
	int i, mask;

	raw_data[3] &= 0xBF; /* bit 6 is always on */

	for (i = 0, mask = 0x01; i < 8; i++, mask <<= 1) {
		/* Keys G1 through G8 */
		scancode = i;
		value = raw_data[1] & mask;
		gcore_input_report_key(gdata, scancode, value);

		/* Keys G9 through G12, M1 through MR */
		scancode = i + 8;
		value = raw_data[2] & mask;
		gcore_input_report_key(gdata, scancode, value);

		/* Keys G17 through G22 */
		scancode = i + 16;
		value = raw_data[3] & mask;
		gcore_input_report_key(gdata, scancode, value);
	}

	input_sync(gdata->input_dev);
}

static int g19_raw_event(struct hid_device *hdev,
			 struct hid_report *report,
			 u8 *raw_data, int size)
{
	struct gcore_data *gdata = dev_get_gdata(&hdev->dev);
	struct g19_data *g19data = gdata->data;
	unsigned long irq_flags;

	/*
	* On initialization receive a 258 byte message with
	* data = 6 0 255 255 255 255 255 255 255 255 ...
	*/


	spin_lock_irqsave(&gdata->lock, irq_flags);

	if (unlikely(g19data->ready_stages != G19_READY_STAGE_3)) {
		switch (report->id) {
		case 6:
			if (!(g19data->ready_stages & G19_READY_SUBSTAGE_1))
				g19data->ready_stages |= G19_READY_SUBSTAGE_1;
			else if (g19data->ready_stages & G19_READY_SUBSTAGE_4 &&
				 !(g19data->ready_stages & G19_READY_SUBSTAGE_5)
				)
				g19data->ready_stages |= G19_READY_SUBSTAGE_5;
			else if (g19data->ready_stages & G19_READY_SUBSTAGE_6 &&
				 raw_data[1] >= 0x80)
				g19data->ready_stages |= G19_READY_SUBSTAGE_7;
			break;
		case 1:
			if (!(g19data->ready_stages & G19_READY_SUBSTAGE_2))
				g19data->ready_stages |= G19_READY_SUBSTAGE_2;
			else
				g19data->ready_stages |= G19_READY_SUBSTAGE_3;
			break;
		}

		if (g19data->ready_stages == G19_READY_STAGE_1 ||
		    g19data->ready_stages == G19_READY_STAGE_2 ||
		    g19data->ready_stages == G19_READY_STAGE_3)
			complete_all(&g19data->ready);

		spin_unlock_irqrestore(&gdata->lock, irq_flags);
		return 1;
	}

	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	if (likely(report->id == 2)) {
		g19_raw_event_process_input(hdev, gdata, raw_data);
		return 1;
	}

	return 0;
}


#ifdef CONFIG_PM

static int g19_resume(struct hid_device *hdev)
{
	unsigned long irq_flags;
	struct gcore_data *gdata = hid_get_gdata(hdev);

	spin_lock_irqsave(&gdata->lock, irq_flags);
	g19_led_bl_send(hdev);
	g19_led_mbtns_send(hdev);
	g19_led_screen_bl_send(hdev);
	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	return 0;
}

static int g19_reset_resume(struct hid_device *hdev)
{
	return g19_resume(hdev);
}

#endif /* CONFIG_PM */

/***** probe-related functions *****/

static void g19_ep1_urb_completion(struct urb *urb)
{
	/* don't process unlinked or failed urbs */
	if (likely(urb->status == 0)) {
		struct hid_device *hdev = urb->context;
		struct gcore_data *gdata = hid_get_gdata(hdev);
		struct g19_data *g19data = gdata->data;
		int i;

		for (i = 0; i < 8; i++)
			gcore_input_report_key(gdata, 24+i,
					       g19data->ep1keys[0]&(1<<i));

		input_sync(gdata->input_dev);

		usb_submit_urb(urb, GFP_ATOMIC);
	}
}

static void g19_feature_report_4_send(struct hid_device *hdev, int which)
{
	struct g19_data *g19data = hid_get_g19data(hdev);

	if (which == G19_REPORT_4_INIT) {
		g19data->feature_report_4->field[0]->value[0] = 0x02;
		g19data->feature_report_4->field[0]->value[1] = 0x00;
		g19data->feature_report_4->field[0]->value[2] = 0x00;
		g19data->feature_report_4->field[0]->value[3] = 0x00;
	} else if (which == G19_REPORT_4_FINALIZE) {
		g19data->feature_report_4->field[0]->value[0] = 0x02;
		g19data->feature_report_4->field[0]->value[1] = 0x80;
		g19data->feature_report_4->field[0]->value[2] = 0x00;
		g19data->feature_report_4->field[0]->value[3] = 0xFF;
	} else {
		return;
	}

	hid_hw_request(hdev, g19data->feature_report_4, HID_REQ_SET_REPORT);
}

static int read_feature_reports(struct gcore_data *gdata)
{
	struct hid_device *hdev = gdata->hdev;
	struct g19_data *g19data = gdata->data;

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
		case 0x04:
			g19data->feature_report_4 = report;
			break;
		case 0x05:
			g19data->led_report = report;
			break;
		case 0x06:
			g19data->start_input_report = report;
			break;
		case 0x07:
			g19data->backlight_report = report;
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
	struct g19_data *g19data = gdata->data;
	struct hid_device *hdev = gdata->hdev;
	unsigned long irq_flags;

	dbg_hid("Waiting for G19 to activate\n");

	/* Wait here for stage 1 (substages 1-3) to complete */
	wait_for_completion_timeout(&g19data->ready, HZ);

	/* Protect data->ready_stages */
	spin_lock_irqsave(&gdata->lock, irq_flags);
	if (g19data->ready_stages != G19_READY_STAGE_1) {
		dev_warn(&hdev->dev,
			 "%s hasn't completed stage 1 yet, forging ahead with initialization\n",
			 gdata->name);
		/* Force the stage */
		g19data->ready_stages = G19_READY_STAGE_1;
	}
	init_completion(&g19data->ready);
	g19data->ready_stages |= G19_READY_SUBSTAGE_4;
	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	/*
	 * Send the init report, then follow with the input report to trigger
	 * report 6 and wait for us to get a response.
	 */
	g19_feature_report_4_send(hdev, G19_REPORT_4_INIT);
	hid_hw_request(hdev, g19data->start_input_report, HID_REQ_GET_REPORT);
	wait_for_completion_timeout(&g19data->ready, HZ);

	/* Protect g19data->ready_stages */
	spin_lock_irqsave(&gdata->lock, irq_flags);
	if (g19data->ready_stages != G19_READY_STAGE_2) {
		dev_warn(&hdev->dev,
			 "%s hasn't completed stage 2 yet, forging ahead with initialization\n",
			 gdata->name);
		/* Force the stage */
		g19data->ready_stages = G19_READY_STAGE_2;
	}
	init_completion(&g19data->ready);
	g19data->ready_stages |= G19_READY_SUBSTAGE_6;
	spin_unlock_irqrestore(&gdata->lock, irq_flags);
}

static void send_finalize_report(struct gcore_data *gdata)
{
	struct g19_data *g19data = gdata->data;
	struct hid_device *hdev = gdata->hdev;
	unsigned long irq_flags;

	/*
	 * Send the finalize report, then follow with the input report to
	 * trigger report 6 and wait for us to get a response.
	 */
	g19_feature_report_4_send(hdev, G19_REPORT_4_FINALIZE);
	hid_hw_request(hdev, g19data->start_input_report, HID_REQ_GET_REPORT);
	hid_hw_request(hdev, g19data->start_input_report, HID_REQ_GET_REPORT);
	wait_for_completion_timeout(&g19data->ready, HZ);

	/* Protect data->ready_stages */
	spin_lock_irqsave(&gdata->lock, irq_flags);

	if (g19data->ready_stages != G19_READY_STAGE_3) {
		dev_warn(&hdev->dev,
			 "%s hasn't completed stage 3 yet, forging ahead with initialization\n",
			 gdata->name);
		/* Force the stage */
		g19data->ready_stages = G19_READY_STAGE_3;
	} else {
		dbg_hid("%s stage 3 complete\n", gdata->name);
	}

	spin_unlock_irqrestore(&gdata->lock, irq_flags);
}

static int g19_ep1_read(struct hid_device *hdev)
{
	struct usb_interface *intf;
	struct usb_device *usb_dev;
	struct g19_data *g19data = hid_get_g19data(hdev);

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

	usb_fill_int_urb(g19data->ep1_urb, usb_dev, pipe, g19data->ep1keys, 2,
			 g19_ep1_urb_completion, NULL, 10);
	g19data->ep1_urb->context = hdev;
	g19data->ep1_urb->actual_length = 0;

	retval = usb_submit_urb(g19data->ep1_urb, GFP_KERNEL);

	return retval;
}


static int g19_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int error;
	struct gcore_data *gdata;
	struct g19_data *g19data;

	dev_dbg(&hdev->dev, "Logitech G19 HID hardware probe...");

	gdata = gcore_alloc_data(G19_NAME, hdev);
	if (gdata == NULL) {
		dev_err(&hdev->dev,
			G19_NAME
			" can't allocate space for device attributes\n");
		error = -ENOMEM;
		goto err_no_cleanup;
	}

	g19data = kzalloc(sizeof(struct g19_data), GFP_KERNEL);
	if (g19data == NULL) {
		error = -ENOMEM;
		goto err_cleanup_gdata;
	}
	gdata->data = g19data;
	init_completion(&g19data->ready);

	g19data->ep1_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (g19data->ep1_urb == NULL) {
		dev_err(&hdev->dev,
			"%s: ERROR: can't alloc ep1 urb stuff\n",
			gdata->name);
		error = -ENOMEM;
		goto err_cleanup_g19data;
	}

	error = gcore_hid_open(gdata);
	if (error) {
		dev_err(&hdev->dev,
			"%s error opening hid device\n",
			gdata->name);
		goto err_cleanup_ep1_urb;
	}

	error = gcore_input_probe(gdata, g19_default_keymap,
				  ARRAY_SIZE(g19_default_keymap));
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

	error = gcore_leds_probe(gdata, g19_led_cdevs,
				 ARRAY_SIZE(g19_led_cdevs));
	if (error) {
		dev_err(&hdev->dev, "%s error registering leds\n", gdata->name);
		goto err_cleanup_input;
	}

	gdata->gfb_data = gfb_probe(hdev, GFB_PANEL_TYPE_320_240_16);
	if (gdata->gfb_data == NULL) {
		dev_err(&hdev->dev,
			"%s error registering framebuffer\n",
			gdata->name);
		goto err_cleanup_leds;
	}

	error = sysfs_create_group(&(hdev->dev.kobj), &g19_attr_group);
	if (error) {
		dev_err(&hdev->dev,
			"%s failed to create sysfs group attributes\n",
			gdata->name);
		goto err_cleanup_gfb;
	}

	wait_ready(gdata);

	/*
	 * Clear the LEDs
	 */
	g19data->backlight_rgb[0] = G19_DEFAULT_RED;
	g19data->backlight_rgb[1] = G19_DEFAULT_GREEN;
	g19data->backlight_rgb[2] = G19_DEFAULT_BLUE;
	g19data->screen_bl = G19_DEFAULT_BRIGHTNESS;

	g19_led_bl_send(hdev);
	g19_led_mbtns_send(hdev);
	g19_led_screen_bl_send(hdev);

	send_finalize_report(gdata);

	error = g19_ep1_read(hdev);
	if (error) {
		dev_err(&hdev->dev, "%s failed to read ep1\n", gdata->name);
		goto err_cleanup_sysfs;
	}

	dbg_hid("G19 activated and initialized\n");

	/* Everything went well */
	return 0;

err_cleanup_sysfs:
	sysfs_remove_group(&(hdev->dev.kobj), &g19_attr_group);

err_cleanup_gfb:
	gfb_remove(gdata->gfb_data);

err_cleanup_leds:
	gcore_leds_remove(gdata);

err_cleanup_input:
	gcore_input_remove(gdata);

err_cleanup_hid:
	gcore_hid_close(gdata);

err_cleanup_ep1_urb:
	usb_free_urb(g19data->ep1_urb);

err_cleanup_g19data:
	kfree(g19data);

err_cleanup_gdata:
	gcore_free_data(gdata);

err_no_cleanup:
	hid_set_drvdata(hdev, NULL);
	return error;
}

static void g19_remove(struct hid_device *hdev)
{
	struct gcore_data *gdata = hid_get_drvdata(hdev);
	struct g19_data *g19data = gdata->data;

	usb_poison_urb(g19data->ep1_urb);

	sysfs_remove_group(&(hdev->dev.kobj), &g19_attr_group);

	gfb_remove(gdata->gfb_data);

	gcore_leds_remove(gdata);
	gcore_input_remove(gdata);
	gcore_hid_close(gdata);

	usb_free_urb(g19data->ep1_urb);

	kfree(g19data);
	gcore_free_data(gdata);
}


static const struct hid_device_id g19_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_G19_LCD) },
	{ }
};
MODULE_DEVICE_TABLE(hid, g19_devices);

static struct hid_driver g19_driver = {
	.name			= "hid-g19",
	.id_table		= g19_devices,
	.probe			= g19_probe,
	.remove			= g19_remove,
	.raw_event		= g19_raw_event,

#ifdef CONFIG_PM
	.resume			= g19_resume,
	.reset_resume		= g19_reset_resume,
#endif

};

static int __init g19_init(void)
{
	return hid_register_driver(&g19_driver);
}

static void __exit g19_exit(void)
{
	hid_unregister_driver(&g19_driver);
}

module_init(g19_init);
module_exit(g19_exit);
MODULE_DESCRIPTION("Logitech G19 HID Driver");
MODULE_AUTHOR("Alistair Buxton (a.j.buxton@gmail.com)");
MODULE_AUTHOR("Thomas Berger (tbe@boreus.de)");
MODULE_AUTHOR("Ciubotariu Ciprian (cheepeero@gmx.net)");
MODULE_LICENSE("GPL v2");
