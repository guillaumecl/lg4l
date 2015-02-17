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

#define G15_NAME "Logitech G15"

/* Key defines */
#define G15_KEYS 64

/* Backlight defaults */
#define G15_DEFAULT_RED (0)
#define G15_DEFAULT_GREEN (255)
#define G15_DEFAULT_BLUE (0)

/* LED array indices */
#define G15_LED_M1 0
#define G15_LED_M2 1
#define G15_LED_M3 2
#define G15_LED_MR 3
#define G15_LED_BL_KEYS 4
#define G15_LED_BL_SCREEN 5
#define G15_LED_BL_CONTRAST 6 /* HACK ALERT contrast is nothing like a LED */

#define G15_REPORT_4_INIT	0x00
#define G15_REPORT_4_FINALIZE	0x01

#define G15_READY_SUBSTAGE_1 0x01
#define G15_READY_SUBSTAGE_2 0x02
#define G15_READY_SUBSTAGE_3 0x04
#define G15_READY_STAGE_1    0x07
#define G15_READY_SUBSTAGE_4 0x08
#define G15_READY_SUBSTAGE_5 0x10
#define G15_READY_STAGE_2    0x1F
#define G15_READY_SUBSTAGE_6 0x20
#define G15_READY_SUBSTAGE_7 0x40
#define G15_READY_STAGE_3    0x7F

#define G15_RESET_POST 0x01
#define G15_RESET_MESSAGE_1 0x02
#define G15_RESET_READY 0x03

/* Per device data structure */
struct g15_data {
	/* HID reports */
	struct hid_report *backlight_report;
	struct hid_report *start_input_report;
	struct hid_report *feature_report_4;
	struct hid_report *led_report;
	struct hid_report *output_report_3;

	/* led state */
	u8 backlight;		/* keyboard illumination */
	u8 screen_bl;		/* screen backlight */
	u8 screen_contrast;	/* screen contrast */
	u8 led_mbtns;			/* m1, m2, m3 and mr */

	/* initialization stages */
	struct completion ready;
	int ready_stages;
};

/* Convenience macros */
#define hid_get_g15data(hdev) \
	((struct g15_data *)(hid_get_gdata(hdev)->data))
#define dev_get_g15data(dev) \
	((struct g15_data *)(dev_get_gdata(dev)->data))

/*
 * Keymap array indices
 *
 * Key	  Byte	Mask Index
 * -----  ----	---- -----
 * G1	  0	0x01  0
 * G13	  0	0x04  2
 * LIT	  0	0x80  7
 * G7	  1	0x01  8
 * G2	  1	0x02  9
 * G14	  1	0x08 11
 * S2	  1	0x80 15
 * G8	  2	0x02 17
 * G3	  2	0x04 18
 * G15	  2	0x10 20
 * S3	  2	0x80 23
 * G9	  3	0x04 26
 * G4	  3	0x08 27
 * G16	  3	0x20 29
 * S4	  3	0x80 31
 * G10	  4	0x08 35
 * G5	  4	0x10 36
 * G17	  4	0x40 38
 * S5	  4	0x80 39
 * M1	  5	0x01 40
 * G11	  5	0x10 44
 * G6	  5	0x20 45
 * M2	  6	0x02 49
 * G12	  6	0x20 53
 * MR	  6	0x40 54
 * M3	  7	0x04 58
 * G18	  7	0x40 62
 * S1	  7	0x80 63
 */
static const unsigned int g15_default_keymap[G15_KEYS] = {
	KEY_F1, /* G1 */
	KEY_UNKNOWN,
	KEY_F13, /* G13 */
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_KBDILLUMTOGGLE, /* LIGHT */
	KEY_F7, /* G7 */
	KEY_F2, /* G2 */
	KEY_UNKNOWN,
	KEY_F14, /* G14 */
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_LEFT, /* S2 */
	KEY_UNKNOWN,
	KEY_F8, /* G8 */
	KEY_F3, /* G3 */
	KEY_UNKNOWN,
	KEY_F15, /* G15 */
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_UP, /* S3 */
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_F9, /* G9 */
	KEY_F4, /* G4 */
	KEY_UNKNOWN,
	KEY_F16, /* G16 */
	KEY_UNKNOWN,
	KEY_DOWN, /* S4 */
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_F10, /* G10 */
	KEY_F5, /* G5 */
	KEY_UNKNOWN,
	KEY_F17, /* G17 */
	KEY_RIGHT, /* S5 */
	KEY_PROG1, /* M1 */
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_F11, /* G11 */
	KEY_F6, /* G6 */
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_PROG2, /* M2 */
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_F12, /* G12 */
	KEY_RECORD, /* MR */
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_PROG3, /* M3 */
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_F18, /* G18 */
	KEY_OK, /* S1 */
};


static void g15_led_send(struct hid_device *hdev, u8 msg, u8 value1, u8 value2)
{
	struct g15_data *g15data = hid_get_g15data(hdev);

	g15data->led_report->field[0]->value[0] = msg;
	g15data->led_report->field[0]->value[1] = value1;
	g15data->led_report->field[0]->value[2] = value2;

	hid_hw_request(hdev, g15data->led_report, HID_REQ_SET_REPORT);
}

static void g15_led_mbtns_send(struct hid_device *hdev)
{
	struct g15_data *g15data = hid_get_g15data(hdev);

	g15_led_send(hdev, 0x04, ~(g15data->led_mbtns), 0);
}

static void g15_led_mbtns_brightness_set(struct led_classdev *led_cdev,
					 enum led_brightness value)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g15_data *g15data = gdata->data;
	u8 mask = 0;

	if (led_cdev == gdata->led_cdev[G15_LED_M1])
		mask = 0x01;
	else if (led_cdev == gdata->led_cdev[G15_LED_M2])
		mask = 0x02;
	else if (led_cdev == gdata->led_cdev[G15_LED_M3])
		mask = 0x04;
	else if (led_cdev == gdata->led_cdev[G15_LED_MR])
		mask = 0x08;

	if (mask && value)
		g15data->led_mbtns |= mask;
	else
		g15data->led_mbtns &= ~mask;

	g15_led_mbtns_send(hdev);
}

static enum led_brightness
g15_led_mbtns_brightness_get(struct led_classdev *led_cdev)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g15_data *g15data = gdata->data;
	int value = 0;

	if (led_cdev == gdata->led_cdev[G15_LED_M1])
		value = g15data->led_mbtns & 0x01;
	else if (led_cdev == gdata->led_cdev[G15_LED_M2])
		value = g15data->led_mbtns & 0x02;
	else if (led_cdev == gdata->led_cdev[G15_LED_M3])
		value = g15data->led_mbtns & 0x04;
	else if (led_cdev == gdata->led_cdev[G15_LED_MR])
		value = g15data->led_mbtns & 0x08;
	else
		dev_err(&hdev->dev,
			G15_NAME " error retrieving LED brightness\n");

	if (value)
		return LED_FULL;
	return LED_OFF;
}

static void g15_led_bl_send(struct hid_device *hdev)
{
	struct g15_data *g15data = hid_get_g15data(hdev);

	g15_led_send(hdev, 0x01, g15data->backlight, 0);
	g15_led_send(hdev, 0x02, g15data->screen_bl, 0);
	g15_led_send(hdev, 0x20, 129, g15data->screen_contrast);
}

static void g15_led_bl_set(struct led_classdev *led_cdev,
			   enum led_brightness value)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g15_data *g15data = gdata->data;

	if (led_cdev == gdata->led_cdev[G15_LED_BL_KEYS]) {
		if (value > 2)
			value = 2;
		g15data->backlight = value;
		g15_led_send(hdev, 0x01, g15data->backlight, 0);
	} else if (led_cdev == gdata->led_cdev[G15_LED_BL_SCREEN]) {
		if (value > 2)
			value = 2;
		g15data->screen_bl = value<<4;
		g15_led_send(hdev, 0x02, g15data->screen_bl, 0);
	} else if (led_cdev == gdata->led_cdev[G15_LED_BL_CONTRAST]) {
		if (value > 63)
			value = 63;
		g15data->screen_contrast = value;
		g15_led_send(hdev, 0x20, 0x81, g15data->screen_contrast);
	}
}

static enum led_brightness g15_led_bl_get(struct led_classdev *led_cdev)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g15_data *g15data = gdata->data;

	if (led_cdev == gdata->led_cdev[G15_LED_BL_KEYS])
		return g15data->backlight;
	else if (led_cdev == gdata->led_cdev[G15_LED_BL_SCREEN])
		return g15data->screen_bl;
	else if (led_cdev == gdata->led_cdev[G15_LED_BL_CONTRAST])
		return g15data->screen_contrast;

	dev_err(&hdev->dev, G15_NAME " error retrieving LED brightness\n");
	return LED_OFF;
}

static const struct led_classdev g15_led_cdevs[7] = {
	{
		.name			= "g15_%d:orange:m1",
		.brightness_set		= g15_led_mbtns_brightness_set,
		.brightness_get		= g15_led_mbtns_brightness_get,
	},
	{
		.name			= "g15_%d:orange:m2",
		.brightness_set		= g15_led_mbtns_brightness_set,
		.brightness_get		= g15_led_mbtns_brightness_get,
	},
	{
		.name			= "g15_%d:orange:m3",
		.brightness_set		= g15_led_mbtns_brightness_set,
		.brightness_get		= g15_led_mbtns_brightness_get,
	},
	{
		.name			= "g15_%d:blue:mr",
		.brightness_set		= g15_led_mbtns_brightness_set,
		.brightness_get		= g15_led_mbtns_brightness_get,
	},
	{
		.name			= "g15_%d:blue:keys",
		.brightness_set		= g15_led_bl_set,
		.brightness_get		= g15_led_bl_get,
	},
	{
		.name			= "g15_%d:white:screen",
		.brightness_set		= g15_led_bl_set,
		.brightness_get		= g15_led_bl_get,
	},
	{
		.name			= "g15_%d:contrast:screen",
		.brightness_set		= g15_led_bl_set,
		.brightness_get		= g15_led_bl_get,
	},
};


static DEVICE_ATTR(fb_node, 0444, gfb_fb_node_show, NULL);
static DEVICE_ATTR(fb_update_rate, 0664,
		   gfb_fb_update_rate_show, gfb_fb_update_rate_store);
static DEVICE_ATTR(name, 0664, gcore_name_show, gcore_name_store);
static DEVICE_ATTR(minor, 0444, gcore_minor_show, NULL);

static struct attribute *g15_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_minor.attr,
	&dev_attr_fb_update_rate.attr,
	&dev_attr_fb_node.attr,
	NULL,	/* need to NULL terminate the list of attributes */
};

static struct attribute_group g15_attr_group = {
	.attrs = g15_attrs,
};

static void g15_raw_event_process_input(struct hid_device *hdev,
					struct gcore_data *gdata,
					u8 *raw_data)
{
	struct input_dev *idev = gdata->input_dev;
	int scancode;
	int value;
	int i;
	int mask;

	raw_data[4] &= 0xFE; /* This bit turns on and off at random */

	for (i = 0, mask = 0x01; i < 8; i++, mask <<= 1) {
		scancode = i;
		value = raw_data[1] & mask;
		gcore_input_report_key(gdata, scancode, value);

		scancode = i + 8;
		value = raw_data[2] & mask;
		gcore_input_report_key(gdata, scancode, value);

		scancode = i + 16;
		value = raw_data[3] & mask;
		gcore_input_report_key(gdata, scancode, value);

		scancode = i + 24;
		value = raw_data[4] & mask;
		gcore_input_report_key(gdata, scancode, value);

		scancode = i + 32;
		value = raw_data[5] & mask;
		gcore_input_report_key(gdata, scancode, value);

		scancode = i + 40;
		value = raw_data[6] & mask;
		gcore_input_report_key(gdata, scancode, value);

		scancode = i + 48;
		value = raw_data[7] & mask;
		gcore_input_report_key(gdata, scancode, value);

		scancode = i + 56;
		value = raw_data[8] & mask;
		gcore_input_report_key(gdata, scancode, value);
	}

	input_sync(idev);
}

static int g15_raw_event(struct hid_device *hdev,
			 struct hid_report *report,
			 u8 *raw_data, int size)
{
	/*
	* On initialization receive a 258 byte message with
	* data = 6 0 255 255 255 255 255 255 255 255 ...
	*/
	unsigned long irq_flags;
	struct gcore_data *gdata = dev_get_gdata(&hdev->dev);
	struct g15_data *g15data = gdata->data;

	spin_lock_irqsave(&gdata->lock, irq_flags);

	if (unlikely(g15data->ready_stages != G15_READY_STAGE_3)) {
		switch (report->id) {
		case 6:
			if (!(g15data->ready_stages & G15_READY_SUBSTAGE_1))
				g15data->ready_stages |= G15_READY_SUBSTAGE_1;
			else if (g15data->ready_stages & G15_READY_SUBSTAGE_4 &&
				 !(g15data->ready_stages & G15_READY_SUBSTAGE_5)
				)
				g15data->ready_stages |= G15_READY_SUBSTAGE_5;
			else if (g15data->ready_stages & G15_READY_SUBSTAGE_6 &&
				 raw_data[1] >= 0x80)
				g15data->ready_stages |= G15_READY_SUBSTAGE_7;
			break;
		case 1:
			if (!(g15data->ready_stages & G15_READY_SUBSTAGE_2))
				g15data->ready_stages |= G15_READY_SUBSTAGE_2;
			else
				g15data->ready_stages |= G15_READY_SUBSTAGE_3;
			break;
		}

		if (g15data->ready_stages == G15_READY_STAGE_1 ||
		    g15data->ready_stages == G15_READY_STAGE_2 ||
		    g15data->ready_stages == G15_READY_STAGE_3)
			complete_all(&g15data->ready);

		spin_unlock_irqrestore(&gdata->lock, irq_flags);
		return 1;
	}

	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	if (likely(report->id == 2)) {
		g15_raw_event_process_input(hdev, gdata, raw_data);
		return 1;
	}

	return 0;
}


#ifdef CONFIG_PM

static int g15_resume(struct hid_device *hdev)
{
	unsigned long irq_flags;
	struct gcore_data *gdata = hid_get_gdata(hdev);

	spin_lock_irqsave(&gdata->lock, irq_flags);
	g15_led_mbtns_send(hdev);
	g15_led_bl_send(hdev);
	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	return 0;
}

static int g15_reset_resume(struct hid_device *hdev)
{
	return g15_resume(hdev);
}

#endif /* CONFIG_PM */

/***** probe-related functions *****/

static void g15_feature_report_4_send(struct hid_device *hdev, int which)
{
	struct g15_data *g15data = hid_get_g15data(hdev);

	if (which == G15_REPORT_4_INIT) {
		g15data->feature_report_4->field[0]->value[0] = 0x02;
		g15data->feature_report_4->field[0]->value[1] = 0x00;
		g15data->feature_report_4->field[0]->value[2] = 0x00;
		g15data->feature_report_4->field[0]->value[3] = 0x00;
	} else if (which == G15_REPORT_4_FINALIZE) {
		g15data->feature_report_4->field[0]->value[0] = 0x02;
		g15data->feature_report_4->field[0]->value[1] = 0x80;
		g15data->feature_report_4->field[0]->value[2] = 0x00;
		g15data->feature_report_4->field[0]->value[3] = 0xFF;
	} else {
		return;
	}

	hid_hw_request(hdev, g15data->feature_report_4, HID_REQ_SET_REPORT);
}

static int read_feature_reports(struct gcore_data *gdata)
{
	struct hid_device *hdev = gdata->hdev;
	struct g15_data *g15data = gdata->data;

	struct list_head *feature_report_list =
			    &hdev->report_enum[HID_FEATURE_REPORT].report_list;
	struct list_head *output_report_list =
			    &hdev->report_enum[HID_OUTPUT_REPORT].report_list;
	struct hid_report *report;

	if (list_empty(feature_report_list)) {
		dev_err(&hdev->dev, "no feature report found\n");
		return -ENODEV;
	}
	dbg_hid(G15_NAME " feature report found\n");

	list_for_each_entry(report, feature_report_list, list) {
		switch (report->id) {
		case 0x02: /* G15 has only one feature report 0x02 */
			g15data->feature_report_4
			    = g15data->led_report
			      = g15data->start_input_report
				= g15data->backlight_report
				  = report;
			break;
		default:
			break;
		}
		dbg_hid(
			"%s Feature report: id=%u type=%u size=%u maxfield=%u report_count=%u\n",
			gdata->name,
			report->id, report->type, report->size,
			report->maxfield, report->field[0]->report_count);
	}

	if (list_empty(output_report_list)) {
		dev_err(&hdev->dev, "no output report found\n");
		return -ENODEV;
	}
	dbg_hid(G15_NAME " output report found\n");

	list_for_each_entry(report, output_report_list, list) {
		dbg_hid("%s output report %d found size=%u maxfield=%u\n",
			gdata->name,
			report->id, report->size, report->maxfield);
		if (report->maxfield > 0) {
			dbg_hid("%s offset=%u size=%u count=%u type=%u\n",
				gdata->name,
				report->field[0]->report_offset,
				report->field[0]->report_size,
				report->field[0]->report_count,
				report->field[0]->report_type);
		}
		switch (report->id) {
		case 0x03:
			g15data->output_report_3 = report;
			break;
		}
	}

	dbg_hid("Found all reports\n");

	return 0;
}

static void wait_ready(struct gcore_data *gdata)
{
	struct g15_data *g15data = gdata->data;
	struct hid_device *hdev = gdata->hdev;
	unsigned long irq_flags;

	dbg_hid("Waiting for G15 to activate\n");

	/*
	 * Wait here for stage 1 (substages 1-3) to complete
	 */
	wait_for_completion_timeout(&g15data->ready, HZ);

	/* Protect data->ready_stages */
	spin_lock_irqsave(&gdata->lock, irq_flags);
	if (g15data->ready_stages != G15_READY_STAGE_1) {
		dev_warn(&hdev->dev,
			 "%s hasn't completed stage 1 yet, forging ahead with initialization\n",
			 gdata->name);
		/* Force the stage */
		g15data->ready_stages = G15_READY_STAGE_1;
	}
	init_completion(&g15data->ready);
	g15data->ready_stages |= G15_READY_SUBSTAGE_4;
	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	/*
	 * Send the init report, then follow with the input report to trigger
	 * report 6 and wait for us to get a response.
	 */
	g15_feature_report_4_send(hdev, G15_REPORT_4_INIT);
	hid_hw_request(hdev, g15data->start_input_report, HID_REQ_GET_REPORT);
	wait_for_completion_timeout(&g15data->ready, HZ);

	/* Protect data->ready_stages */
	spin_lock_irqsave(&gdata->lock, irq_flags);
	if (g15data->ready_stages != G15_READY_STAGE_2) {
		dev_warn(&hdev->dev,
			 "%s hasn't completed stage 2 yet, forging ahead with initialization\n",
			 gdata->name);
		/* Force the stage */
		g15data->ready_stages = G15_READY_STAGE_2;
	}
	init_completion(&g15data->ready);
	g15data->ready_stages |= G15_READY_SUBSTAGE_6;
	spin_unlock_irqrestore(&gdata->lock, irq_flags);
}

static void send_finalize_report(struct gcore_data *gdata)
{
	struct g15_data *g15data = gdata->data;
	struct hid_device *hdev = gdata->hdev;
	unsigned long irq_flags;

	/*
	 * Send the finalize report, then follow with the input report to
	 * trigger report 6 and wait for us to get a response.
	 */
	g15_feature_report_4_send(hdev, G15_REPORT_4_FINALIZE);
	hid_hw_request(hdev, g15data->start_input_report, HID_REQ_GET_REPORT);
	hid_hw_request(hdev, g15data->start_input_report, HID_REQ_GET_REPORT);
	wait_for_completion_timeout(&g15data->ready, HZ);

	/* Protect data->ready_stages */
	spin_lock_irqsave(&gdata->lock, irq_flags);

	if (g15data->ready_stages != G15_READY_STAGE_3) {
		dev_warn(&hdev->dev, G15_NAME " hasn't completed stage 3 yet, forging ahead with initialization\n");
		/* Force the stage */
		g15data->ready_stages = G15_READY_STAGE_3;
	} else {
		dbg_hid(G15_NAME " stage 3 complete\n");
	}

	spin_unlock_irqrestore(&gdata->lock, irq_flags);
}

static int g15_probe(struct hid_device *hdev,
		     const struct hid_device_id *id)
{
	int error;
	struct gcore_data *gdata;
	struct g15_data *g15data;

	dev_dbg(&hdev->dev, "Logitech G15 HID hardware probe...");

	gdata = gcore_alloc_data(G15_NAME, hdev);
	if (gdata == NULL) {
		dev_err(&hdev->dev, G15_NAME " can't allocate space for device attributes\n");
		error = -ENOMEM;
		goto err_no_cleanup;
	}

	g15data = kzalloc(sizeof(struct g15_data), GFP_KERNEL);
	if (g15data == NULL) {
		error = -ENOMEM;
		goto err_cleanup_gdata;
	}
	gdata->data = g15data;
	init_completion(&g15data->ready);

	error = gcore_hid_open(gdata);
	if (error) {
		dev_err(&hdev->dev,
			"%s error opening hid device\n",
			gdata->name);
		goto err_cleanup_g15data;
	}

	error = gcore_input_probe(gdata, g15_default_keymap,
				  ARRAY_SIZE(g15_default_keymap));
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

	error = gcore_leds_probe(gdata, g15_led_cdevs,
				 ARRAY_SIZE(g15_led_cdevs));
	if (error) {
		dev_err(&hdev->dev, "%s error registering leds\n", gdata->name);
		goto err_cleanup_input;
	}

	gdata->gfb_data = gfb_probe(hdev, GFB_PANEL_TYPE_160_43_1);
	if (gdata->gfb_data == NULL) {
		dev_err(&hdev->dev,
			G15_NAME " error registering framebuffer\n");
		goto err_cleanup_leds;
	}

	error = sysfs_create_group(&(hdev->dev.kobj), &g15_attr_group);
	if (error) {
		dev_err(&hdev->dev,
			G15_NAME " failed to create sysfs group attributes\n");
		goto err_cleanup_gfb;
	}

	wait_ready(gdata);

	/*
	 * Clear the LEDs
	 */
	g15_led_mbtns_send(hdev);
	g15_led_bl_send(hdev);

	send_finalize_report(gdata);

	dbg_hid("G15 activated and initialized\n");

	/* Everything went well */
	return 0;

err_cleanup_gfb:
	gfb_remove(gdata->gfb_data);

err_cleanup_leds:
	gcore_leds_remove(gdata);
err_cleanup_input:
	gcore_input_remove(gdata);

err_cleanup_hid:
	gcore_hid_close(gdata);

err_cleanup_g15data:
	kfree(g15data);

err_cleanup_gdata:
	gcore_free_data(gdata);

err_no_cleanup:
	hid_set_drvdata(hdev, NULL);
	return error;
}

static void g15_remove(struct hid_device *hdev)
{
	struct gcore_data *gdata = hid_get_drvdata(hdev);
	struct g15_data *g15data = gdata->data;

	sysfs_remove_group(&(hdev->dev.kobj), &g15_attr_group);

	gfb_remove(gdata->gfb_data);

	gcore_leds_remove(gdata);
	gcore_input_remove(gdata);
	gcore_hid_close(gdata);

	kfree(g15data);
	gcore_free_data(gdata);
}

static const struct hid_device_id g15_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_G15_LCD) },
	{ }
};
MODULE_DEVICE_TABLE(hid, g15_devices);

static struct hid_driver g15_driver = {
	.name			= "hid-g15",
	.id_table		= g15_devices,
	.probe			= g15_probe,
	.remove			= g15_remove,
	.raw_event		= g15_raw_event,

#ifdef CONFIG_PM
	.resume			= g15_resume,
	.reset_resume		= g15_reset_resume,
#endif
};

static int __init g15_init(void)
{
	return hid_register_driver(&g15_driver);
}

static void __exit g15_exit(void)
{
	hid_unregister_driver(&g15_driver);
}

module_init(g15_init);
module_exit(g15_exit);
MODULE_DESCRIPTION("Logitech G15 HID Driver");
MODULE_AUTHOR("Alistair Buxton (a.j.buxton@gmail.com)");
MODULE_AUTHOR("Ciubotariu Ciprian (cheepeero@gmx.net)");
MODULE_LICENSE("GPL");
