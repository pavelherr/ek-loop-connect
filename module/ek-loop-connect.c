// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ek-loop-connect.c - Linux driver for EK Loop Connect
 * Copyright (C) 2021 Pavel Herrmann <pavelherr@gmail.com>
 *
 * This driver uses hid reports to communicate with the device to allow hidraw userspace drivers
 * still being used. The device does not use report ids. When using hidraw and this driver
 * simultaniously, reports could be switched.
 */

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/usb.h>


#define USB_VENDOR_ID_EK		0x0483
#define USB_PRODUCT_ID_EK_LOOP_CONNECT	0x5750

#define BUFFER_SIZE		63
#define CHANNEL_OFFSET		6
#define CHANNEL_SIZE		2

#define NUM_FANS		6
#define NUM_TEMP_SENSORS	3

#define REQ_TIMEOUT		500

// Specific byte offsets from response buffers
#define FAN_READ_RPM_OFFSET 12
#define FAN_READ_PWM_OFFSET 21
#define FAN_SET_PWM_OFFSET 24
#define SENSOR_T1_OFFSET 11
#define SENSOR_T2_OFFSET 15
#define SENSOR_T3_OFFSET 19
#define SENSOR_FLOW_OFFSET 22
#define SENSOR_LEVEL_OFFSET 27


static const u8 fan_read_request[] = {
        0x10, 0x12, 0x08, 0xaa, 0x01, 0x03, 0xff, 0xff,         // 6B header, 2B channel
        0x00, 0x20, 0x66, 0xff, 0xff, 0xed, 0x00, 0x00,         // 2B constant, 3B checksum? (bytes 10-12), 1B constant, 2B padding
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,         // padding
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const u8 sensor_read_request[] = {
        0x10, 0x12, 0x08, 0xaa, 0x01, 0x03, 0xa2, 0x20,		// 6B header, 2B channel
        0x00, 0x20, 0x66, 0x60, 0xfe, 0xed, 0x00, 0x00,		// constant?
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,		// padding  49B
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const u8 fan_set_request[] = {
        0x10, 0x12, 0x29, 0xaa, 0x01, 0x10, 0xff, 0xff,         // 6B header, 2B channel
        0x00, 0x10, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,         // 3B constant + 4B padding + high byte fan rpm (byte 15)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,         // low byte fan RPM (byte 16) + 7B padding
        0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,         // PWM percentage (byte 24), checksum? (byte 25), padding
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,         // padding
        0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xed, 0x00,         // padding, checksum? (byte 45), constant (byte 46)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,         // padding
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const u8 fan_channels[][CHANNEL_SIZE] = {
        {0xa0, 0xa0},
        {0xa0, 0xc0},
        {0xa0, 0xe0},
        {0xa1, 0x00},
        {0xa1, 0x20},
        {0xa1, 0xe0},
};

static const char fan_labels[][3] = {"F1", "F2", "F3", "F4", "F5", "F6"};
static const char temp_labels[][3] = {"T1", "T2", "T3"};

static const char level_label[] = "coolant level";
static const char flow_label[] = "coolant flow (l/h)";

struct ekloco_device {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct completion wait_input_report;
	struct mutex mutex; // whenever buffer is used
	u8 *buffer;
};


struct sensor_result {
	long temp[3];
	long flow_lph;
	bool level;
};

struct fan_read_result {
	long rpm;
	long pwm;
};

static int ekloco_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
	struct ekloco_device *ekloco = hid_get_drvdata(hdev);

	// only copy buffer when requested
	if (completion_done(&ekloco->wait_input_report))
		return 0;

	memcpy(ekloco->buffer, data, min(size, BUFFER_SIZE));
	complete(&ekloco->wait_input_report);

	return 0;

}

static int read_fan_speed(struct ekloco_device *ekloco, int channel, struct fan_read_result *result)
{
	int ret = 0;
	unsigned long t;
	int pwm, rpm;

	mutex_lock(&ekloco->mutex);

	reinit_completion(&ekloco->wait_input_report);
	memcpy(ekloco->buffer, fan_read_request, BUFFER_SIZE);
	memcpy(ekloco->buffer + CHANNEL_OFFSET, fan_channels[channel], CHANNEL_SIZE);

	hid_hw_output_report(ekloco->hdev, ekloco->buffer, BUFFER_SIZE);

	t = wait_for_completion_timeout(&ekloco->wait_input_report, msecs_to_jiffies(REQ_TIMEOUT));
	if (!t) {
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	// PWM is reported as one byte with value 0-100. Convert to more traditional 0-255
	pwm = ekloco->buffer[FAN_READ_PWM_OFFSET];
	result->pwm = mult_frac(pwm, 255, 100);

	// RPM value is stored as 2 bytes.
	rpm = ekloco->buffer[FAN_READ_RPM_OFFSET];
	rpm = (rpm<<8) + ekloco->buffer[FAN_READ_RPM_OFFSET+1];
	result->rpm = rpm;

out_unlock:
	mutex_unlock(&ekloco->mutex);
	return ret;
}

static int set_fan_pwm(struct ekloco_device *ekloco, int channel, long target)
{
	int ret = 0;
	unsigned long t;

	if (target > 255 || target < 0)
		return -EINVAL;

	mutex_lock(&ekloco->mutex);

	reinit_completion(&ekloco->wait_input_report);
	memcpy(ekloco->buffer, fan_set_request, BUFFER_SIZE);
	memcpy(ekloco->buffer + CHANNEL_OFFSET, fan_channels[channel], CHANNEL_SIZE);
	ekloco->buffer[FAN_SET_PWM_OFFSET] = DIV_ROUND_CLOSEST(target * 100, 255);

	hid_hw_output_report(ekloco->hdev, ekloco->buffer, BUFFER_SIZE);

	t = wait_for_completion_timeout(&ekloco->wait_input_report, msecs_to_jiffies(REQ_TIMEOUT));
	if (!t) {
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

out_unlock:
	mutex_unlock(&ekloco->mutex);
	return ret;
}

static int read_sensors(struct ekloco_device *ekloco, struct sensor_result *result)
{
	int ret = 0;
	unsigned long t;
	int flow;

	mutex_lock(&ekloco->mutex);

	reinit_completion(&ekloco->wait_input_report);
	memcpy(ekloco->buffer, sensor_read_request, BUFFER_SIZE);

	hid_hw_output_report(ekloco->hdev, ekloco->buffer, BUFFER_SIZE);

	t = wait_for_completion_timeout(&ekloco->wait_input_report, msecs_to_jiffies(REQ_TIMEOUT));
	if (!t) {
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	// Temperatures are reported as single-byte values in degC
	result->temp[0] = ekloco->buffer[SENSOR_T1_OFFSET];
	result->temp[1] = ekloco->buffer[SENSOR_T2_OFFSET];
	result->temp[2] = ekloco->buffer[SENSOR_T3_OFFSET];

	result->level = !!ekloco->buffer[SENSOR_LEVEL_OFFSET];

	// Flow measurement has a conversion factor of 0.8 l/h
	flow = ekloco->buffer[SENSOR_FLOW_OFFSET];
	flow = (flow<<8) + ekloco->buffer[SENSOR_FLOW_OFFSET+1];
	result->flow_lph = mult_frac(flow, 8, 10);

out_unlock:
	mutex_unlock(&ekloco->mutex);
	return ret;
}

static int ekloco_read_string(struct device *ekloco, enum hwmon_sensor_types type,
			      u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		if (channel < 0 || channel >= NUM_TEMP_SENSORS)
			break;
		switch (attr) {
		case hwmon_temp_label:
			*str = temp_labels[channel];
			return 0;
		default:
			break;
		}
		break;
	case hwmon_fan:
		// Coolant flow meter is exposed as an extra fan.
		if (channel < 0 || channel >= (NUM_FANS + 1))
			break;
		switch (attr) {
		case hwmon_fan_label:
			if (channel == NUM_FANS)
				*str = flow_label;
			else
				*str = fan_labels[channel];
			return 0;
		default:
			break;
		}
		break;
	case hwmon_humidity:
		if (channel != 0)
			break;
		switch (attr) {
		case hwmon_humidity_label:
			*str = level_label;
			return 0;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int ekloco_read(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long *val)
{
	struct ekloco_device *ekloco = dev_get_drvdata(dev);
	int ret;

	switch (type) {
	case hwmon_temp:
		if (channel < 0 || channel >= NUM_TEMP_SENSORS)
			break;
		switch (attr) {
		case hwmon_temp_input:
			{
				struct sensor_result result;
				ret = read_sensors(ekloco, &result);
				if (ret < 0)
					return ret;
				// Temperature is already reported as degC, scale to expected unit.
				*val = result.temp[channel] * 1000;
			}
			return 0;
		default:
			break;
		}
		break;
	case hwmon_fan:
		// Coolant flow meter is exposed as an extra fan.
		if (channel < 0 || channel >= (NUM_FANS + 1))
			break;
		switch (attr) {
		case hwmon_fan_input:
			if (channel == NUM_FANS) {
				struct sensor_result result;
				ret = read_sensors(ekloco, &result);
				if (ret < 0)
					return ret;
				*val = result.flow_lph;
			} else {
				struct fan_read_result result;
				ret = read_fan_speed(ekloco, channel, &result);
				if (ret < 0)
					return ret;
				*val = result.rpm;
			}
			return 0;
		default:
			break;
		}
		break;
	case hwmon_pwm:
		if (channel < 0 || channel >= NUM_FANS)
			break;
		switch (attr) {
		case hwmon_pwm_input:
			{
				struct fan_read_result result;
				ret = read_fan_speed(ekloco, channel, &result);
				if (ret < 0)
					return ret;
				*val = result.pwm;
			}
			return 0;
		default:
			break;
		}
		break;
	case hwmon_humidity:
		if (channel != 0)
			break;
		switch (attr) {
		case hwmon_humidity_alarm:
			{
				struct sensor_result result;
				ret = read_sensors(ekloco, &result);
				if (ret < 0)
					return ret;
				*val = !result.level;
			}
			return 0;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
};

static int ekloco_write(struct device *dev, enum hwmon_sensor_types type,
		        u32 attr, int channel, long val)
{
	struct ekloco_device *ekloco = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_pwm:
		if (channel < 0 || channel >= NUM_FANS)
			break;
		switch (attr) {
		case hwmon_pwm_input:
			return set_fan_pwm(ekloco, channel, val);
		default:
			break;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
};


static umode_t ekloco_is_visible(const void *data, enum hwmon_sensor_types type,
			         u32 attr, int channel)
{
	switch (type) {
	case hwmon_temp:
		if (channel < 0 || channel >= NUM_TEMP_SENSORS)
			break;
		switch (attr) {
		case hwmon_temp_input:
			return 0444;
		case hwmon_temp_label:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_fan:
		// Coolant flow meter is exposed as an extra fan.
		if (channel < 0 || channel >= (NUM_FANS + 1))
			break;
		switch (attr) {
		case hwmon_fan_input:
			return 0444;
		case hwmon_fan_label:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_pwm:
		if (channel < 0 || channel >= NUM_FANS)
			break;
		switch (attr) {
		case hwmon_pwm_input:
			return 0644;
		default:
			break;
		}
		break;
	// Coolant level sensor exports as humidity.
	case hwmon_humidity:
		if (channel != 0)
			break;
		switch (attr) {
		case hwmon_humidity_label:
			return 0444;
		case hwmon_humidity_alarm:
			return 0444;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return 0;
}


static const struct hwmon_ops ekloco_hwmon_ops = {
	.is_visible = ekloco_is_visible,
	.read = ekloco_read,
	.read_string = ekloco_read_string,
	.write = ekloco_write,
};


static const struct hwmon_channel_info *ekloco_info[] = {
	HWMON_CHANNEL_INFO(chip,
			   HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL
			   ),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL
			   ),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT
			   ),
	// Coolant level is exposed as humidity alarm, due to lack of better options.
	HWMON_CHANNEL_INFO(humidity,
			   HWMON_H_LABEL | HWMON_H_ALARM
			   ),
	NULL
};

static const struct hwmon_chip_info ekloco_chip_info = {
	.ops = &ekloco_hwmon_ops,
	.info = ekloco_info,
};


static int ekloco_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct ekloco_device *ekloco;
	int ret;

	// The controller exposes 2 interfaces, we only talk to interface 0.
	struct usb_interface *usbif = to_usb_interface(hdev->dev.parent);
	if (usbif->cur_altsetting->desc.bInterfaceNumber != 0) {
		hid_set_drvdata(hdev, NULL);
		return 0;
	}

	ekloco = devm_kzalloc(&hdev->dev, sizeof(*ekloco), GFP_KERNEL);
	if (!ekloco)
		return -ENOMEM;

	ekloco->buffer = devm_kmalloc(&hdev->dev, BUFFER_SIZE, GFP_KERNEL);
	if (!ekloco->buffer)
		return -ENOMEM;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret)
		goto out_hw_stop;

	ekloco->hdev = hdev;
	hid_set_drvdata(hdev, ekloco);
	mutex_init(&ekloco->mutex);
	init_completion(&ekloco->wait_input_report);

	hid_device_io_start(hdev);

	ekloco->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, "ekloopconnect",
							 ekloco, &ekloco_chip_info, 0);
	if (IS_ERR(ekloco->hwmon_dev)) {
		ret = PTR_ERR(ekloco->hwmon_dev);
		goto out_hw_close;
	}

	return 0;

out_hw_close:
	hid_hw_close(hdev);
out_hw_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void ekloco_remove(struct hid_device *hdev)
{
	struct ekloco_device *ekloco = hid_get_drvdata(hdev);
	struct usb_interface *usbif = to_usb_interface(hdev->dev.parent);
	if (usbif->cur_altsetting->desc.bInterfaceNumber != 0) {
		return;
	}

	hwmon_device_unregister(ekloco->hwmon_dev);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id ekloco_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_EK, USB_PRODUCT_ID_EK_LOOP_CONNECT) },
	{ }
};

static struct hid_driver ekloco_driver = {
	.name = "ek-loop-connect",
	.id_table = ekloco_devices,
	.probe = ekloco_probe,
	.remove = ekloco_remove,
	.raw_event = ekloco_raw_event,
};

MODULE_DEVICE_TABLE(hid, ekloco_devices);
MODULE_LICENSE("GPL");

static int __init ekloco_init(void)
{
	return hid_register_driver(&ekloco_driver);
}

static void __exit ekloco_exit(void)
{
	hid_unregister_driver(&ekloco_driver);
}

/*
 * When compiling this driver as built-in, hwmon initcalls will get called before the
 * hid driver and this driver would fail to register. late_initcall solves this.
 */
late_initcall(ekloco_init);
module_exit(ekloco_exit);
