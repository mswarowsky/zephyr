/*
 * Copyright (c) 2018 Alexander Wachter
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>

#define RX_THREAD_STACK_SIZE 512
#define RX_THREAD_PRIORITY 2
#define STATE_POLL_THREAD_STACK_SIZE 512
#define STATE_POLL_THREAD_PRIORITY 2
#define LED_MSG_ID 0x10
#define COUNTER_MSG_ID 0x12345
#define SET_LED 1
#define RESET_LED 0
#define SLEEP_TIME K_MSEC(250)

K_THREAD_STACK_DEFINE(rx_thread_stack, RX_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(poll_state_stack, STATE_POLL_THREAD_STACK_SIZE);

const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

struct k_thread rx_thread_data;
struct k_thread poll_state_thread_data;
struct k_work_poll change_led_work;
struct k_work state_change_work;
enum can_state current_state;
struct can_bus_err_cnt current_err_cnt;

CAN_MSGQ_DEFINE(change_led_msgq, 2);
CAN_MSGQ_DEFINE(counter_msgq, 2);

static struct k_poll_event change_led_events[1] = {
	K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE,
					K_POLL_MODE_NOTIFY_ONLY,
					&change_led_msgq, 0)
};

void tx_irq_callback(const struct device *dev, int error, void *arg)
{
	char *sender = (char *)arg;

	ARG_UNUSED(dev);

	if (error != 0) {
		printk("Callback! error-code: %d\nSender: %s\n",
		       error, sender);
	}
}

void rx_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);
	const struct can_filter filter = {
		.flags = CAN_FILTER_DATA | CAN_FILTER_IDE,
		.id = COUNTER_MSG_ID,
		.mask = CAN_EXT_ID_MASK
	};
	struct can_frame frame;
	int filter_id;

	filter_id = can_add_rx_filter_msgq(can_dev, &counter_msgq, &filter);
	printk("Counter filter id: %d\n", filter_id);

	while (1) {
		k_msgq_get(&counter_msgq, &frame, K_FOREVER);

		if (frame.dlc != 2U) {
			printk("Wrong data length: %u\n", frame.dlc);
			continue;
		}

		printk("Counter received: %u\n",
		       sys_be16_to_cpu(UNALIGNED_GET((uint16_t *)&frame.data)));
	}
}

void change_led_work_handler(struct k_work *work)
{
	struct can_frame frame;
	int ret;

	while (k_msgq_get(&change_led_msgq, &frame, K_NO_WAIT) == 0) {
		if (led.port == NULL) {
			printk("LED %s\n", frame.data[0] == SET_LED ? "ON" : "OFF");
		} else {
			gpio_pin_set(led.port, led.pin, frame.data[0] == SET_LED ? 1 : 0);
		}
	}

	ret = k_work_poll_submit(&change_led_work, change_led_events,
				 ARRAY_SIZE(change_led_events), K_FOREVER);
	if (ret != 0) {
		printk("Failed to resubmit msgq polling: %d", ret);
	}
}

char *state_to_str(enum can_state state)
{
	switch (state) {
	case CAN_STATE_ERROR_ACTIVE:
		return "error-active";
	case CAN_STATE_ERROR_WARNING:
		return "error-warning";
	case CAN_STATE_ERROR_PASSIVE:
		return "error-passive";
	case CAN_STATE_BUS_OFF:
		return "bus-off";
	case CAN_STATE_STOPPED:
		return "stopped";
	default:
		return "unknown";
	}
}

void poll_state_thread(void *unused1, void *unused2, void *unused3)
{
	struct can_bus_err_cnt err_cnt = {0, 0};
	struct can_bus_err_cnt err_cnt_prev = {0, 0};
	enum can_state state_prev = CAN_STATE_ERROR_ACTIVE;
	enum can_state state;
	int err;

	while (1) {
		err = can_get_state(can_dev, &state, &err_cnt);
		if (err != 0) {
			printk("Failed to get CAN controller state: %d", err);
			k_sleep(K_MSEC(100));
			continue;
		}

		if (err_cnt.tx_err_cnt != err_cnt_prev.tx_err_cnt ||
		    err_cnt.rx_err_cnt != err_cnt_prev.rx_err_cnt ||
		    state_prev != state) {

			err_cnt_prev.tx_err_cnt = err_cnt.tx_err_cnt;
			err_cnt_prev.rx_err_cnt = err_cnt.rx_err_cnt;
			state_prev = state;
			printk("state: %s\n"
			       "rx error count: %d\n"
			       "tx error count: %d\n",
			       state_to_str(state),
			       err_cnt.rx_err_cnt, err_cnt.tx_err_cnt);
		} else {
			k_sleep(K_MSEC(100));
		}
	}
}

void state_change_work_handler(struct k_work *work)
{
	printk("State Change ISR\nstate: %s\n"
	       "rx error count: %d\n"
	       "tx error count: %d\n",
		state_to_str(current_state),
		current_err_cnt.rx_err_cnt, current_err_cnt.tx_err_cnt);

#ifndef CONFIG_CAN_AUTO_BUS_OFF_RECOVERY
	if (current_state == CAN_STATE_BUS_OFF) {
		printk("Recover from bus-off\n");

		if (can_recover(can_dev, K_MSEC(100)) != 0) {
			printk("Recovery timed out\n");
		}
	}
#endif /* CONFIG_CAN_AUTO_BUS_OFF_RECOVERY */
}

void state_change_callback(const struct device *dev, enum can_state state,
			   struct can_bus_err_cnt err_cnt, void *user_data)
{
	struct k_work *work = (struct k_work *)user_data;

	ARG_UNUSED(dev);

	current_state = state;
	current_err_cnt = err_cnt;
	k_work_submit(work);
}

void main(void)
{
	const struct can_filter change_led_filter = {
		.flags = CAN_FILTER_DATA,
		.id = LED_MSG_ID,
		.mask = CAN_STD_ID_MASK
	};
	struct can_frame change_led_frame = {
		.flags = 0,
		.id = LED_MSG_ID,
		.dlc = 1
	};
	struct can_frame counter_frame = {
		.flags = CAN_FRAME_IDE,
		.id = COUNTER_MSG_ID,
		.dlc = 2
	};
	uint8_t toggle = 1;
	uint16_t counter = 0;
	k_tid_t rx_tid, get_state_tid;
	int ret;

	if (!device_is_ready(can_dev)) {
		printk("CAN: Device %s not ready.\n", can_dev->name);
		return;
	}

#ifdef CONFIG_LOOPBACK_MODE
	ret = can_set_mode(can_dev, CAN_MODE_LOOPBACK);
	if (ret != 0) {
		printk("Error setting CAN mode [%d]", ret);
		return;
	}
#endif
	ret = can_start(can_dev);
	if (ret != 0) {
		printk("Error starting CAN controller [%d]", ret);
		return;
	}

	if (led.port != NULL) {
		if (!device_is_ready(led.port)) {
			printk("LED: Device %s not ready.\n",
			       led.port->name);
			return;
		}
		ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_HIGH);
		if (ret < 0) {
			printk("Error setting LED pin to output mode [%d]",
			       ret);
			led.port = NULL;
		}
	}

	k_work_init(&state_change_work, state_change_work_handler);
	k_work_poll_init(&change_led_work, change_led_work_handler);

	ret = can_add_rx_filter_msgq(can_dev, &change_led_msgq, &change_led_filter);
	if (ret == -ENOSPC) {
		printk("Error, no filter available!\n");
		return;
	}

	printk("Change LED filter ID: %d\n", ret);

	ret = k_work_poll_submit(&change_led_work, change_led_events,
				 ARRAY_SIZE(change_led_events), K_FOREVER);
	if (ret != 0) {
		printk("Failed to submit msgq polling: %d", ret);
		return;
	}

	rx_tid = k_thread_create(&rx_thread_data, rx_thread_stack,
				 K_THREAD_STACK_SIZEOF(rx_thread_stack),
				 rx_thread, NULL, NULL, NULL,
				 RX_THREAD_PRIORITY, 0, K_NO_WAIT);
	if (!rx_tid) {
		printk("ERROR spawning rx thread\n");
	}

	get_state_tid = k_thread_create(&poll_state_thread_data,
					poll_state_stack,
					K_THREAD_STACK_SIZEOF(poll_state_stack),
					poll_state_thread, NULL, NULL, NULL,
					STATE_POLL_THREAD_PRIORITY, 0,
					K_NO_WAIT);
	if (!get_state_tid) {
		printk("ERROR spawning poll_state_thread\n");
	}

	can_set_state_change_callback(can_dev, state_change_callback, &state_change_work);

	printk("Finished init.\n");

	while (1) {
		change_led_frame.data[0] = toggle++ & 0x01 ? SET_LED : RESET_LED;
		/* This sending call is none blocking. */
		can_send(can_dev, &change_led_frame, K_FOREVER,
			 tx_irq_callback,
			 "LED change");
		k_sleep(SLEEP_TIME);

		UNALIGNED_PUT(sys_cpu_to_be16(counter),
			      (uint16_t *)&counter_frame.data[0]);
		counter++;
		/* This sending call is blocking until the message is sent. */
		can_send(can_dev, &counter_frame, K_MSEC(100), NULL, NULL);
		k_sleep(SLEEP_TIME);
	}
}
