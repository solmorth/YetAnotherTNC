/*
 * UART0 <-> BLE NUS Bridge & APRS TNC Server for ProMicro nRF52840
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Uses interrupt-driven UART with a delayed work item for batching.
 * Supports hardware GPIO mode selection:
 *   - APP_MODE_BRIDGE: Transparent UART <-> BLE bridge (Pin High / Unbridged)
 *   - APP_MODE_PACKET_TNC: AX.25 / KISS TNC server (Pin Low / Grounded)
 *   - APP_MODE_DIGIPEATER: AX.25 Digipeater & BLE forwarding (Digi Pin Grounded)
 *
 * Console/printk goes over USB CDC ACM (configured by board DTS).
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/version.h>

#include "mode_select.h"
#include "ptt.h"
#include "gps.h"
#include "tnc.h"
#include "ax25.h"
#include "fx25.h"
#include "kiss.h"
#include "tnc_config.h"
#include "audio_tx_pwm.hpp"
#include "audio_rx_adc.hpp"
#include "log_ts.h"

/* ---------- BLE advertising -------------------------------------------- */

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

/* ---------- UART ------------------------------------------------------- */

#define UART_NODE DT_CHOSEN(zephyr_nus_uart)
static const struct device *const uart_dev = DEVICE_DT_GET(UART_NODE);

#define RING_BUF_SIZE 1024
RING_BUF_DECLARE(uart_ring_buf, RING_BUF_SIZE);

/*
 * Delayed work: scheduled 50 ms after last received byte.
 * When it fires, the UART line has been idle for 50 ms → full message ready.
 */
static void uart_flush_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(uart_flush_work, uart_flush_work_handler);

#define UART_IDLE_TIMEOUT_MS 50

/* ---------- Forward declarations --------------------------------------- */
static int process_uart_to_ble(void);

/* ---------- UART ISR --------------------------------------------------- */

/* printk() is not IRQ-safe on the USB CDC ACM console backend used here -
 * it can block waiting on the USB stack, which never completes from inside
 * an ISR and freezes the whole system (no fault, no reset, everything
 * including the tick just stops). Count drops here; report them from
 * uart_flush_work_handler() instead, which runs in thread context.
 */
static atomic_t uart_dropped_bytes = ATOMIC_INIT(0);

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	uart_irq_update(dev);

	if (uart_irq_rx_ready(dev)) {
		uint8_t buf[64];
		int len;

		while ((len = uart_fifo_read(dev, buf, sizeof(buf))) > 0) {
			uint32_t put = ring_buf_put(&uart_ring_buf, buf, len);
			if (put < (uint32_t)len) {
				atomic_add(&uart_dropped_bytes, (atomic_val_t)(len - (int)put));
			}
		}

		/*
		 * (Re)schedule the flush work 50 ms from now.
		 * Batch message when line goes quiet for 50ms.
		 */
		k_work_reschedule(&uart_flush_work, K_MSEC(UART_IDLE_TIMEOUT_MS));
	}
}

/* ---------- Flush work: sends accumulated UART data over BLE ----------- */

static void uart_flush_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	atomic_val_t dropped = atomic_set(&uart_dropped_bytes, 0);
	if (dropped) {
		printk("Warning: UART ring buffer full, dropped %d bytes\n", (int)dropped);
	}

	process_uart_to_ble();
}

/*
 * bt_nus_send() -> bt_gatt_notify() -> bt_att_create_pdu() blocks for up to
 * BT_ATT_TIMEOUT (30s, see att.c) when the host's ATT buffer pool is
 * exhausted - i.e. whenever the BLE peer isn't draining notifications as
 * fast as we're producing them. ble_send_raw() is called straight from
 * uart_flush_work_handler on the system workqueue, which is also the only
 * thing draining uart_ring_buf - if it blocks there, UART reception stalls
 * too. Decouple: enqueue here (fast, never blocks) and let a dedicated
 * thread take the blocking hit, same pattern as tx_thread_entry/tx_msgq.
 */
#define BLE_TX_ITEM_MAX_LEN 512
#define BLE_TX_MSGQ_DEPTH 16

struct ble_tx_item {
	uint8_t data[BLE_TX_ITEM_MAX_LEN];
	size_t len;
};

K_MSGQ_DEFINE(ble_tx_msgq, sizeof(struct ble_tx_item), BLE_TX_MSGQ_DEPTH, 4);

/* Ref'd in ble_connected(), unref'd in ble_disconnected() - see below. */
static struct bt_conn *active_conn;

static void ble_tx_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct ble_tx_item item;
	int consecutive_fail = 0;

	k_thread_name_set(k_current_get(), "ble_tx");

	while (1) {
		k_msgq_get(&ble_tx_msgq, &item, K_FOREVER);

		int err = bt_nus_send(NULL, item.data, item.len);
		if (err) {
			if (err == -ENOTCONN) {
				printk("  (no BLE peer)\n");
				consecutive_fail = 0;
			} else {
				/* bt_att_create_pdu() blocks up to 30s per call when the
				 * peer isn't draining notifications fast enough, then
				 * fails with -ENOMEM - this thread would otherwise grind
				 * at ~1 item/30s forever while ble_send_raw() drops
				 * everything else as "queue full". Two in a row means the
				 * link is wedged, not just slow: force a disconnect so
				 * the peer (or a fresh one) gets a clean reconnect and
				 * the ATT buffer pool has a chance to recover.
				 */
				printk("  bt_nus_send err %d (%d in a row)\n", err, ++consecutive_fail);
				if (consecutive_fail >= 2 && active_conn) {
					printk("BLE TX stuck, forcing disconnect to recover\n");
					bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
					consecutive_fail = 0;
				}
			}
		} else {
			consecutive_fail = 0;
		}
	}
}

/* 1024 was tight for a thread that calls into bt_nus_send()/bt_gatt_notify()
 * on top of its own 520-byte struct ble_tx_item local - bumped as a
 * precaution alongside the system workqueue stack overflow fix (see
 * CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE in prj.conf).
 */
#define BLE_TX_THREAD_STACK_SIZE 2048
#define BLE_TX_THREAD_PRIORITY 7

K_THREAD_DEFINE(ble_tx_thread_id, BLE_TX_THREAD_STACK_SIZE, ble_tx_thread_entry,
		 NULL, NULL, NULL, BLE_TX_THREAD_PRIORITY, 0, 0);

static int ble_send_raw(const uint8_t *data, size_t len)
{
	if (data == NULL || len == 0 || len > BLE_TX_ITEM_MAX_LEN) {
		return -EINVAL;
	}

	struct ble_tx_item item;
	item.len = len;
	memcpy(item.data, data, len);

	int err = k_msgq_put(&ble_tx_msgq, &item, K_NO_WAIT);
	if (err) {
		printk("Warning: BLE TX queue full, dropped notification of %d bytes\n", (int)len);
	}
	return err;
}

static int process_uart_to_ble(void)
{
	uint8_t data[247]; /* max ATT payload */
	int len;

	app_mode_t mode = mode_select_get_current();

	while ((len = ring_buf_get(&uart_ring_buf, data, sizeof(data))) > 0) {
		if (mode == APP_MODE_BRIDGE) {
			printk("UART -> BLE (Bridge): %d bytes\n", len);
			ble_send_raw(data, len);
		} else {
			printk("UART -> TNC Engine (%s): %d bytes\n", mode_select_get_name(mode), len);
			tnc_process_radio_bytes(data, len, mode, uart_dev, ble_send_raw);
		}
	}

	return 0;
}

/* ---------- BLE NUS callbacks ------------------------------------------ */

static void nus_notif_enabled(bool enabled, void *ctx)
{
	ARG_UNUSED(ctx);
	printk("BLE Notifications %s\n", enabled ? "Enabled" : "Disabled");
}

static void nus_received(struct bt_conn *conn, const void *data,
			 uint16_t len, void *ctx)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(ctx);

	app_mode_t mode = mode_select_get_current();

	if (mode == APP_MODE_BRIDGE) {
		printk("BLE -> UART (Bridge): %d bytes\n", len);
		tnc_queue_tx_packet((const uint8_t *)data, len, mode);
	} else {
		printk("BLE -> TNC Engine (%s): %d bytes\n", mode_select_get_name(mode), len);
		tnc_process_ble_bytes((const uint8_t *)data, len, mode, uart_dev, ble_send_raw);
	}
}

static struct bt_nus_cb nus_listener = {
	.notif_enabled = nus_notif_enabled,
	.received      = nus_received,
};

/* ---------- BLE connection callbacks (re-advertise on disconnect) ------ */

static void ble_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("BLE Connect failed (err %u)\n", err);
		return;
	}
	printk("BLE Connected\n");
	active_conn = bt_conn_ref(conn);
}

/*
 * Restarting advertising is deferred to the system workqueue instead of
 * calling bt_le_adv_start() straight from the disconnected callback: the
 * connection object for the link that just dropped isn't necessarily
 * released yet at that point, and starting connectable advertising before
 * it is can fail (observed as bt_le_adv_start() returning -12/-ENOMEM,
 * "no more connection objects available"). Deferring gives the stack a
 * chance to finish tearing down the old connection first.
 */
static void adv_restart_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Failed to restart advertising: %d\n", err);
	} else {
		printk("Advertising restarted\n");
	}
}
static K_WORK_DEFINE(adv_restart_work, adv_restart_work_handler);

static void ble_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	printk("BLE Disconnected (reason 0x%02X), restarting advertising\n", reason);

	if (active_conn) {
		bt_conn_unref(active_conn);
		active_conn = NULL;
	}

	k_work_submit(&adv_restart_work);
}

static struct bt_conn_cb conn_callbacks = {
	.connected    = ble_connected,
	.disconnected = ble_disconnected,
};

/* ---------- Console banner (deferred until a terminal is attached) ----- */

/*
 * CONFIG_BOOT_BANNER is off (see prj.conf) because the normal Zephyr version
 * banner prints before main() even runs - long before a USB CDC ACM terminal
 * has enumerated and opened the port, so it's just dropped. Re-print it (and
 * the hardware mode) from a low-priority thread that polls DTR instead of
 * blocking main() / BLE / UART bring-up on a terminal ever connecting.
 */
static void console_banner_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	const struct device *const console_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0;

	while (!dtr) {
		uart_line_ctrl_get(console_dev, UART_LINE_CTRL_DTR, &dtr);
		k_sleep(K_MSEC(100));
	}

	printk("*** Booting Zephyr OS build %s ***\n", KERNEL_VERSION_STRING);
	printk("=== UART <-> BLE NUS Bridge & APRS TNC Server ===\n");
	printk("Hardware Mode Selected: %s\n", mode_select_get_name(mode_select_get_current()));
}

K_THREAD_DEFINE(console_banner_thread_id, 1024, console_banner_thread_entry,
		 NULL, NULL, NULL, 10, 0, 0);


/* ---------- main ------------------------------------------------------- */

int main(void)
{
	int err;

	printk("=== UART <-> BLE NUS Bridge & APRS TNC Server ===\n");

	mode_select_init();
	tnc_config_init();
	ptt_init();

	if (mode_select_get_current() == APP_MODE_STANDALONE) {
		if (tnc_config_get_fixed_pos_enabled()) {
			gps_start_fixed_position_beacon();
		} else {
			gps_init();
			gps_enable_set(true);
		}
	}

	tnc_init();
	fx25_init();
	printk("FX.25 FEC Engine Initialized (RS(255,239), Tag_01 0x%016llX)\n",
	       (unsigned long long)FX25_TAG_01);
	audio_tx_pwm_init();
	/* TEMPORARY bring-up aid: plain 1200Hz tone on every boot, no BLE/KISS
	 * needed - remove once the PWM DAC -> RC filter -> radio chain is
	 * confirmed working end to end.
	 */
	// audio_tx_pwm_test_tone();
	audio_rx_adc_init(ble_send_raw);

	if (!device_is_ready(uart_dev)) {
		printk("ERROR: UART device not ready\n");
	} else {
		tnc_tx_queue_init(uart_dev);
	}

	/* ---- BLE init (first, so device is always discoverable) ------- */
	err = bt_nus_cb_register(&nus_listener, NULL);
	if (err) {
		printk("Failed to register NUS callback: %d\n", err);
		return err;
	}

	err = bt_conn_cb_register(&conn_callbacks);
	if (err) {
		printk("Failed to register connection callbacks: %d\n", err);
		return err;
	}

	err = bt_enable(NULL);
	if (err) {
		printk("Failed to enable Bluetooth: %d\n", err);
		return err;
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Failed to start advertising: %d\n", err);
		return err;
	}

	printk("Advertising as \"%s\"\n", DEVICE_NAME);

	/* ---- UART init ------------------------------------------------ */
	if (!device_is_ready(uart_dev)) {
		printk("ERROR: UART device not ready\n");
		goto idle;
	}

	uart_irq_callback_set(uart_dev, uart_isr);
	uart_irq_rx_enable(uart_dev);

	printk("UART bridge & TNC Server active (IRQ + 50ms idle-batch)\n");

idle:
	/* Main loop */
	while (true) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
