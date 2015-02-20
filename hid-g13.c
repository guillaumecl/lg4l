/***************************************************************************
 *   Copyright (C) 2009 by Rick L. Vinyard, Jr.				   *
 *   rvinyard@cs.nmsu.edu						   *
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

#define G13_NAME "Logitech G13"

/* Key defines */
#define G13_KEYS 35
#define G13_KEYMAP_SIZE (G13_KEYS*3)

/* Framebuffer defines */
#define G13FB_NAME "g13fb"
#define G13FB_WIDTH (160)
#define G13FB_LINE_LENGTH (160/8)
#define G13FB_HEIGHT (43)
#define G13FB_SIZE (G13FB_LINE_LENGTH*G13FB_HEIGHT)

#define G13FB_UPDATE_RATE_LIMIT (20)
#define G13FB_UPDATE_RATE_DEFAULT (10)

/* Backlight defaults */
#define G13_DEFAULT_RED (0)
#define G13_DEFAULT_GREEN (255)
#define G13_DEFAULT_BLUE (0)

#define LED_COUNT 7

/* LED array indices */
#define G13_LED_M1 0
#define G13_LED_M2 1
#define G13_LED_M3 2
#define G13_LED_MR 3
#define G13_LED_BL_R 4
#define G13_LED_BL_G 5
#define G13_LED_BL_B 6

#define G13_REPORT_4_INIT	0x00
#define G13_REPORT_4_FINALIZE	0x01

#define G13_READY_SUBSTAGE_1 0x01
#define G13_READY_SUBSTAGE_2 0x02
#define G13_READY_SUBSTAGE_3 0x04
#define G13_READY_STAGE_1    0x07
#define G13_READY_SUBSTAGE_4 0x08
#define G13_READY_SUBSTAGE_5 0x10
#define G13_READY_STAGE_2    0x1F
#define G13_READY_SUBSTAGE_6 0x20
#define G13_READY_SUBSTAGE_7 0x40
#define G13_READY_STAGE_3    0x7F

#define G13_RESET_POST 0x01
#define G13_RESET_MESSAGE_1 0x02
#define G13_RESET_READY 0x03

/* G13-specific device data structure */
struct g13_data {
	/* HID reports */
	struct hid_report *backlight_report;
	struct hid_report *start_input_report;
	struct hid_report *feature_report_4;
	struct hid_report *led_report;
	struct hid_report *output_report_3;

	/* led state */
	u8 backlight_rgb[3];	/* keyboard illumination */
	u8 led_mbtns;		/* m1, m2, m3 and mr */

	/* initialization stages */
	struct completion ready;
	int ready_stages;
};

/* Convenience macros */
#define hid_get_g13data(hdev) \
	((struct g13_data *)(hid_get_gdata(hdev)->data))
#define dev_get_g19data(dev) \
	((struct g13_data *)(dev_get_gdata(dev)->data))

/*
 * Keymap array indices
 *
 * Key	      Index
 * ---------  ------
 * G1-G22     0-21
 * FUNC	      22
 * LCD1	      23
 * LCD2	      24
 * LCD3	      25
 * LCD4	      26
 * M1	      27
 * M2	      28
 * M3	      29
 * MR	      30
 * BTN_LEFT   31
 * BTN_DOWN   32
 * BTN_STICK  33
 * LIGHT      34
 */
static const unsigned int g13_default_keymap[G13_KEYS] = {
	/* first row g1 - g7 */
	KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7,
	/* second row g8 - g11 */
	KEY_F8, KEY_F9, KEY_F10, KEY_F11,
	/* second row g12 - g14 */
	KEY_F12, KEY_F13, KEY_F14,
	/* third row g15 - g19 */
	KEY_F15, KEY_F16, KEY_F17, KEY_F18, KEY_F19,
	/* fourth row g20 - g22 */
	KEY_F20, KEY_F21, KEY_F22,
	/* next, lightLeft, lightCenterLeft, lightCenterRight, lightRight */
	/* BTN_0, BTN_1, BTN_2, BTN_3, BTN_4, */
	KEY_OK, KEY_LEFT, KEY_UP, KEY_DOWN, KEY_RIGHT,
	/* M1, M2, M3, MR */
	KEY_PROG1, KEY_PROG2, KEY_PROG3, KEY_RECORD,
	/* button left, button down, button stick, light */
	BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, KEY_KBDILLUMTOGGLE
};

static void g13_led_mbtns_send(struct hid_device *hdev)
{
	struct g13_data *g13data = hid_get_g13data(hdev);

	g13data->led_report->field[0]->value[0] = g13data->led_mbtns&0x0F;
	g13data->led_report->field[0]->value[1] = 0x00;
	g13data->led_report->field[0]->value[2] = 0x00;
	g13data->led_report->field[0]->value[3] = 0x00;

	hid_hw_request(hdev, g13data->led_report, HID_REQ_SET_REPORT);
}

static void g13_led_mbtns_brightness_set(struct led_classdev *led_cdev,
					 enum led_brightness value)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g13_data *g13data = gdata->data;
	u8 mask = 0;

	if (led_cdev == gdata->led_cdev[G13_LED_M1])
		mask = 0x01;
	else if (led_cdev == gdata->led_cdev[G13_LED_M2])
		mask = 0x02;
	else if (led_cdev == gdata->led_cdev[G13_LED_M3])
		mask = 0x04;
	else if (led_cdev == gdata->led_cdev[G13_LED_MR])
		mask = 0x08;

	if (mask && value)
		g13data->led_mbtns |= mask;
	else
		g13data->led_mbtns &= ~mask;

	g13_led_mbtns_send(hdev);
}

static enum led_brightness
g13_led_mbtns_brightness_get(struct led_classdev *led_cdev)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g13_data *g13data = gdata->data;
	int value = 0;

	if (led_cdev == gdata->led_cdev[G13_LED_M1])
		value = g13data->led_mbtns & 0x01;
	else if (led_cdev == gdata->led_cdev[G13_LED_M2])
		value = g13data->led_mbtns & 0x02;
	else if (led_cdev == gdata->led_cdev[G13_LED_M3])
		value = g13data->led_mbtns & 0x04;
	else if (led_cdev == gdata->led_cdev[G13_LED_MR])
		value = g13data->led_mbtns & 0x08;
	else
		dev_err(&hdev->dev,
			G13_NAME " error retrieving LED brightness\n");

	if (value)
		return LED_FULL;
	return LED_OFF;
}

static void g13_led_bl_send(struct hid_device *hdev)
{
	struct g13_data *g13data = hid_get_g13data(hdev);

	struct hid_field *field0 = g13data->backlight_report->field[0];

	field0->value[0] = g13data->backlight_rgb[0];
	field0->value[1] = g13data->backlight_rgb[1];
	field0->value[2] = g13data->backlight_rgb[2];
	field0->value[3] = 0x00;

	hid_hw_request(hdev, g13data->backlight_report, HID_REQ_SET_REPORT);
}

static void g13_led_bl_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g13_data *g13data = gdata->data;

	if (led_cdev == gdata->led_cdev[G13_LED_BL_R])
		g13data->backlight_rgb[0] = value;
	else if (led_cdev == gdata->led_cdev[G13_LED_BL_G])
		g13data->backlight_rgb[1] = value;
	else if (led_cdev == gdata->led_cdev[G13_LED_BL_B])
		g13data->backlight_rgb[2] = value;

	g13_led_bl_send(hdev);
}

static enum led_brightness
g13_led_bl_brightness_get(struct led_classdev *led_cdev)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g13_data *g13data = gdata->data;

	if (led_cdev == gdata->led_cdev[G13_LED_BL_R])
		return g13data->backlight_rgb[0];
	else if (led_cdev == gdata->led_cdev[G13_LED_BL_G])
		return g13data->backlight_rgb[1];
	else if (led_cdev == gdata->led_cdev[G13_LED_BL_B])
		return g13data->backlight_rgb[2];

	dev_err(&hdev->dev, G13_NAME " error retrieving LED brightness\n");
	return LED_OFF;
}

static const struct led_classdev g13_led_cdevs[LED_COUNT] = {
	{
		.name			= "g13_%d:red:m1",
		.brightness_set		= g13_led_mbtns_brightness_set,
		.brightness_get		= g13_led_mbtns_brightness_get,
	},
	{
		.name			= "g13_%d:red:m2",
		.brightness_set		= g13_led_mbtns_brightness_set,
		.brightness_get		= g13_led_mbtns_brightness_get,
	},
	{
		.name			= "g13_%d:red:m3",
		.brightness_set		= g13_led_mbtns_brightness_set,
		.brightness_get		= g13_led_mbtns_brightness_get,
	},
	{
		.name			= "g13_%d:red:mr",
		.brightness_set		= g13_led_mbtns_brightness_set,
		.brightness_get		= g13_led_mbtns_brightness_get,
	},
	{
		.name			= "g13_%d:red:bl",
		.brightness_set		= g13_led_bl_brightness_set,
		.brightness_get		= g13_led_bl_brightness_get,
	},
	{
		.name			= "g13_%d:green:bl",
		.brightness_set		= g13_led_bl_brightness_set,
		.brightness_get		= g13_led_bl_brightness_get,
	},
	{
		.name			= "g13_%d:blue:bl",
		.brightness_set		= g13_led_bl_brightness_set,
		.brightness_get		= g13_led_bl_brightness_get,
	},
};

static DEVICE_ATTR(fb_node, 0444, gfb_fb_node_show, NULL);
static DEVICE_ATTR(fb_update_rate, 0664,
		   gfb_fb_update_rate_show, gfb_fb_update_rate_store);
static DEVICE_ATTR(name, 0664, gcore_name_show, gcore_name_store);
static DEVICE_ATTR(minor, 0444, gcore_minor_show, NULL);

static struct attribute *g13_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_minor.attr,
	&dev_attr_fb_update_rate.attr,
	&dev_attr_fb_node.attr,
	NULL,	/* need to NULL terminate the list of attributes */
};

static struct attribute_group g13_attr_group = {
	.attrs = g13_attrs,
};


static void g13_raw_event_process_input(struct hid_device *hdev,
					struct gcore_data *gdata,
					u8 *raw_data)
{
	struct input_dev *idev = gdata->input_dev;
	int scancode;
	int value;
	int i;
	int mask;

	for (i = 0, mask = 0x01; i < 8; i++, mask <<= 1) {
		/* Keys G1 through G8 */
		scancode = i;
		value = raw_data[3] & mask;
		gcore_input_report_key(gdata, scancode, value);

		/* Keys G9 through G16 */
		scancode = i + 8;
		value = raw_data[4] & mask;
		gcore_input_report_key(gdata, scancode, value);

		/* Keys G17 through G22 */
		scancode = i + 16;
		value = raw_data[5] & mask;
		if (i <= 5)
			gcore_input_report_key(gdata, scancode, value);

		/* Keys FUNC through M3 */
		scancode = i + 22;
		value = raw_data[6] & mask;
		gcore_input_report_key(gdata, scancode, value);

		/* Keys MR through LIGHT */
		scancode = i + 30;
		value = raw_data[7] & mask;
		if (i <= 4)
			gcore_input_report_key(gdata, scancode, value);
	}

	input_report_abs(idev, ABS_X, raw_data[1]);
	input_report_abs(idev, ABS_Y, raw_data[2]);
	input_sync(idev);
}

static int g13_raw_event(struct hid_device *hdev,
			 struct hid_report *report,
			 u8 *raw_data, int size)
{
	unsigned long irq_flags;

	/*
	 * On initialization receive a 258 byte message with
	 * data = 6 0 255 255 255 255 255 255 255 255 ...
	 */
	struct gcore_data *gdata = dev_get_gdata(&hdev->dev);
	struct g13_data *g13data = gdata->data;

	spin_lock_irqsave(&gdata->lock, irq_flags);

	if (unlikely(g13data->ready_stages != G13_READY_STAGE_3)) {
		switch (report->id) {
		case 6:
			if (!(g13data->ready_stages & G13_READY_SUBSTAGE_1))
				g13data->ready_stages |= G13_READY_SUBSTAGE_1;
			else if (g13data->ready_stages & G13_READY_SUBSTAGE_4 &&
				 !(g13data->ready_stages & G13_READY_SUBSTAGE_5)
				)
				g13data->ready_stages |= G13_READY_SUBSTAGE_5;
			else if (g13data->ready_stages & G13_READY_SUBSTAGE_6 &&
				 raw_data[1] >= 0x80)
				g13data->ready_stages |= G13_READY_SUBSTAGE_7;
			break;
		case 1:
			if (!(g13data->ready_stages & G13_READY_SUBSTAGE_2))
				g13data->ready_stages |= G13_READY_SUBSTAGE_2;
			else
				g13data->ready_stages |= G13_READY_SUBSTAGE_3;
			break;
		}

		if (g13data->ready_stages == G13_READY_STAGE_1 ||
		    g13data->ready_stages == G13_READY_STAGE_2 ||
		    g13data->ready_stages == G13_READY_STAGE_3)
			complete_all(&g13data->ready);

		spin_unlock_irqrestore(&gdata->lock, irq_flags);
		return 1;
	}

	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	if (likely(report->id == 1)) {
		g13_raw_event_process_input(hdev, gdata, raw_data);
		return 1;
	}

	return 0;
}

#ifdef CONFIG_PM

static int g13_resume(struct hid_device *hdev)
{
	unsigned long irq_flags;
	struct gcore_data *gdata = hid_get_gdata(hdev);

	spin_lock_irqsave(&gdata->lock, irq_flags);
	g13_led_bl_send(hdev);
	g13_led_mbtns_send(hdev);
	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	return 0;
}

static int g13_reset_resume(struct hid_device *hdev)
{
	return g13_resume(hdev);
}

#endif /* CONFIG_PM */


/***** probe-related functions *****/


static void g13_feature_report_4_send(struct hid_device *hdev, int which)
{
	struct g13_data *g13data = hid_get_g13data(hdev);

	if (which == G13_REPORT_4_INIT) {
		g13data->feature_report_4->field[0]->value[0] = 0x02;
		g13data->feature_report_4->field[0]->value[1] = 0x00;
		g13data->feature_report_4->field[0]->value[2] = 0x00;
		g13data->feature_report_4->field[0]->value[3] = 0x00;
	} else if (which == G13_REPORT_4_FINALIZE) {
		g13data->feature_report_4->field[0]->value[0] = 0x02;
		g13data->feature_report_4->field[0]->value[1] = 0x80;
		g13data->feature_report_4->field[0]->value[2] = 0x00;
		g13data->feature_report_4->field[0]->value[3] = 0xFF;
	} else {
		return;
	}

	hid_hw_request(hdev, g13data->feature_report_4, HID_REQ_SET_REPORT);
}

static int read_feature_reports(struct gcore_data *gdata)
{
	struct hid_device *hdev = gdata->hdev;
	struct g13_data *g13data = gdata->data;

	struct list_head *feature_report_list =
			    &hdev->report_enum[HID_FEATURE_REPORT].report_list;
	struct list_head *output_report_list =
			    &hdev->report_enum[HID_OUTPUT_REPORT].report_list;
	struct hid_report *report;

	if (list_empty(feature_report_list)) {
		dev_err(&hdev->dev, "no feature report found\n");
		return -ENODEV;
	}
	dbg_hid(G13_NAME " feature report found\n");

	list_for_each_entry(report, feature_report_list, list) {
		switch (report->id) {
		case 0x04:
			g13data->feature_report_4 = report;
			break;
		case 0x05:
			g13data->led_report = report;
			break;
		case 0x06:
			g13data->start_input_report = report;
			break;
		case 0x07:
			g13data->backlight_report = report;
			break;
		default:
			break;
		}
		dbg_hid("%s Feature report: id=%u type=%u size=%u maxfield=%u report_count=%u\n",
			gdata->name,
			report->id, report->type, report->size,
			report->maxfield, report->field[0]->report_count);
	}

	if (list_empty(output_report_list)) {
		dev_err(&hdev->dev, "no output report found\n");
		return -ENODEV;
	}
	dbg_hid(G13_NAME " output report found\n");

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
			g13data->output_report_3 = report;
			break;
		}
	}

	dbg_hid("%s found all reports\n", gdata->name);

	return 0;
}

static void wait_ready(struct gcore_data *gdata)
{
	struct g13_data *g13data = gdata->data;
	struct hid_device *hdev = gdata->hdev;
	unsigned long irq_flags;

	dbg_hid("Waiting for G13 to activate\n");

	/*
	 * Wait here for stage 1 (substages 1-3) to complete
	 */
	wait_for_completion_timeout(&g13data->ready, HZ);

	/* Protect data->ready_stages */
	spin_lock_irqsave(&gdata->lock, irq_flags);
	if (g13data->ready_stages != G13_READY_STAGE_1) {
		dev_warn(&hdev->dev,
			 "%s hasn't completed stage 1 yet, forging ahead with initialization\n",
			 gdata->name);
		/* Force the stage */
		g13data->ready_stages = G13_READY_STAGE_1;
	}
	init_completion(&g13data->ready);
	g13data->ready_stages |= G13_READY_SUBSTAGE_4;
	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	/*
	 * Send the init report, then follow with the input report to trigger
	 * report 6 and wait for us to get a response.
	 */
	g13_feature_report_4_send(hdev, G13_REPORT_4_INIT);
	hid_hw_request(hdev, g13data->start_input_report, HID_REQ_GET_REPORT);
	wait_for_completion_timeout(&g13data->ready, HZ);

	/* Protect data->ready_stages */
	spin_lock_irqsave(&gdata->lock, irq_flags);
	if (g13data->ready_stages != G13_READY_STAGE_2) {
		dev_warn(&hdev->dev,
			 "%s hasn't completed stage 2 yet, forging ahead with initialization\n",
			 gdata->name);
		/* Force the stage */
		g13data->ready_stages = G13_READY_STAGE_2;
	}
	init_completion(&g13data->ready);
	g13data->ready_stages |= G13_READY_SUBSTAGE_6;
	spin_unlock_irqrestore(&gdata->lock, irq_flags);
}

static void send_finalize_report(struct gcore_data *gdata)
{
	struct g13_data *g13data = gdata->data;
	struct hid_device *hdev = gdata->hdev;
	unsigned long irq_flags;

	/*
	 * Send the finalize report, then follow with the input report to
	 * trigger report 6 and wait for us to get a response.
	 */
	g13_feature_report_4_send(hdev, G13_REPORT_4_FINALIZE);
	hid_hw_request(hdev, g13data->start_input_report, HID_REQ_GET_REPORT);
	hid_hw_request(hdev, g13data->start_input_report, HID_REQ_GET_REPORT);
	wait_for_completion_timeout(&g13data->ready, HZ);

	/* Protect data->ready_stages */
	spin_lock_irqsave(&gdata->lock, irq_flags);

	if (g13data->ready_stages != G13_READY_STAGE_3) {
		dev_warn(&hdev->dev, G13_NAME " hasn't completed stage 3 yet, forging ahead with initialization\n");
		/* Force the stage */
		g13data->ready_stages = G13_READY_STAGE_3;
	} else {
		dbg_hid(G13_NAME " stage 3 complete\n");
	}

	spin_unlock_irqrestore(&gdata->lock, irq_flags);
}

static int g13_probe(struct hid_device *hdev,
		     const struct hid_device_id *id)
{
	int error;
	struct gcore_data *gdata;
	struct g13_data *g13data;

	dev_dbg(&hdev->dev, "Logitech G13 HID hardware probe...");

	gdata = gcore_alloc_data(G13_NAME, hdev);
	if (gdata == NULL) {
		dev_err(&hdev->dev,
			G13_NAME
			" can't allocate space for device attributes\n");
		error = -ENOMEM;
		goto err_no_cleanup;
	}

	g13data = kzalloc(sizeof(struct g13_data), GFP_KERNEL);
	if (g13data == NULL) {
		error = -ENOMEM;
		goto err_cleanup_gdata;
	}
	gdata->data = g13data;
	init_completion(&g13data->ready);

	error = gcore_hid_open(gdata);
	if (error) {
		dev_err(&hdev->dev,
			"%s error opening hid device\n",
			gdata->name);
		goto err_cleanup_g13data;
	}

	error = gcore_input_probe(gdata, g13_default_keymap,
				  ARRAY_SIZE(g13_default_keymap));
	if (error) {
		dev_err(&hdev->dev,
			"%s error registering input device\n",
			gdata->name);
		goto err_cleanup_hid;
	}

	/* initialize the joystick on the G13 */
	input_set_capability(gdata->input_dev, EV_ABS, ABS_X);
	input_set_capability(gdata->input_dev, EV_ABS, ABS_Y);
	input_set_capability(gdata->input_dev, EV_MSC, MSC_SCAN);

	/* 4 center values */
	input_set_abs_params(gdata->input_dev, ABS_X, 0, 0xff, 0, 4);
	input_set_abs_params(gdata->input_dev, ABS_Y, 0, 0xff, 0, 4);

	error = read_feature_reports(gdata);
	if (error) {
		dev_err(&hdev->dev,
			"%s error reading feature reports\n",
			gdata->name);
		goto err_cleanup_input;
	}

	error = gcore_leds_probe(gdata, g13_led_cdevs,
				 ARRAY_SIZE(g13_led_cdevs));
	if (error) {
		dev_err(&hdev->dev, "%s error registering leds\n", gdata->name);
		goto err_cleanup_input;
	}

	gdata->gfb_data = gfb_probe(hdev, GFB_PANEL_TYPE_160_43_1);
	if (gdata->gfb_data == NULL) {
		dev_err(&hdev->dev, G13_NAME " error registering framebuffer\n");
		goto err_cleanup_leds;
	}

	error = sysfs_create_group(&(hdev->dev.kobj), &g13_attr_group);
	if (error) {
		dev_err(&hdev->dev, G13_NAME " failed to create sysfs group attributes\n");
		goto err_cleanup_gfb;
	}

	wait_ready(gdata);

	/*
	 * Clear the LEDs
	 */
	g13data->backlight_rgb[0] = G13_DEFAULT_RED;
	g13data->backlight_rgb[1] = G13_DEFAULT_GREEN;
	g13data->backlight_rgb[2] = G13_DEFAULT_BLUE;

	g13_led_mbtns_send(hdev);
	g13_led_bl_send(hdev);

	send_finalize_report(gdata);

	dbg_hid("G13 activated and initialized\n");

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

err_cleanup_g13data:
	kfree(g13data);

err_cleanup_gdata:
	gcore_free_data(gdata);

err_no_cleanup:
	hid_set_drvdata(hdev, NULL);
	return error;
}

static void g13_remove(struct hid_device *hdev)
{
	struct gcore_data *gdata = hid_get_drvdata(hdev);
	struct g13_data *g13data = gdata->data;

	sysfs_remove_group(&(hdev->dev.kobj), &g13_attr_group);

	gfb_remove(gdata->gfb_data);

	gcore_leds_remove(gdata);
	gcore_input_remove(gdata);
	gcore_hid_close(gdata);

	kfree(g13data);
	gcore_free_data(gdata);
}

static const struct hid_device_id g13_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_G13) },
	{ }
};
MODULE_DEVICE_TABLE(hid, g13_devices);

static struct hid_driver g13_driver = {
	.name			= "hid-g13",
	.id_table		= g13_devices,
	.probe			= g13_probe,
	.remove			= g13_remove,
	.raw_event		= g13_raw_event,

#ifdef CONFIG_PM
	.resume			= g13_resume,
	.reset_resume		= g13_reset_resume,
#endif
};

static int __init g13_init(void)
{
	return hid_register_driver(&g13_driver);
}

static void __exit g13_exit(void)
{
	hid_unregister_driver(&g13_driver);
}

module_init(g13_init);
module_exit(g13_exit);
MODULE_DESCRIPTION("Logitech G13 HID Driver");
MODULE_AUTHOR("Rick L Vinyard Jr (rvinyard@cs.nmsu.edu)");
MODULE_AUTHOR("Ciubotariu Ciprian (cheepeero@gmx.net)");
MODULE_LICENSE("GPL");
