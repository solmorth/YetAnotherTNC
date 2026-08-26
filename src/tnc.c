#include "tnc.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/uart.h>
#include "log_ts.h"

static kiss_decoder_t ble_kiss_dec;
static kiss_decoder_t radio_kiss_dec;

void tnc_init(void)
{
	kiss_decoder_init(&ble_kiss_dec);
	kiss_decoder_init(&radio_kiss_dec);
	printk("TNC Core Engine Initialized\n");
}

#include "audio_tx_pwm.hpp"

#define TX_PKT_MAX_LEN 512
#define TX_MSGQ_DEPTH 8

struct tx_packet {
	const struct device *uart_dev;
	uint8_t data[TX_PKT_MAX_LEN];
	size_t len;
	app_mode_t mode;
};

K_MSGQ_DEFINE(tx_msgq, sizeof(struct tx_packet), TX_MSGQ_DEPTH, 4);

static const struct device *g_tx_uart_dev = NULL;

static void tx_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct tx_packet pkt;

	k_thread_name_set(k_current_get(), "tx_afsk");

	while (1) {
		if (k_msgq_get(&tx_msgq, &pkt, K_FOREVER) == 0) {
			if (pkt.mode == APP_MODE_BRIDGE) {
				/* Transparent bridge: write straight through to the UART,
				 * not the AFSK/PWM modem (audio_tx_pwm_play_packet() is a
				 * no-op for this mode anyway).
				 */
				if (pkt.uart_dev != NULL) {
					for (size_t i = 0; i < pkt.len; i++) {
						uart_poll_out(pkt.uart_dev, pkt.data[i]);
					}
				}
			} else {
				audio_tx_pwm_play_packet(pkt.data, pkt.len, pkt.mode);
			}
		}
	}
}

/* AFSKModulator builds a dozen nested std::vector<uint8_t/float> temporaries
 * per frame plus libm sin(). CONFIG_HW_STACK_PROTECTION turns any overflow
 * of this into an immediate hard fault instead of silent corruption - 4096
 * was already a guess at the right headroom and wasn't enough in practice
 * (the tx thread was faulting out before ever reaching ptt_set()/the PWM
 * loop, i.e. total silence with no visible error besides the fault dump).
 */
#define TX_THREAD_STACK_SIZE 8192
#define TX_THREAD_PRIORITY 5

K_THREAD_DEFINE(tx_thread_id, TX_THREAD_STACK_SIZE, tx_thread_entry, NULL, NULL, NULL, TX_THREAD_PRIORITY, 0, 0);

int tnc_tx_queue_init(const struct device *uart_dev)
{
	g_tx_uart_dev = uart_dev;
	return 0;
}

int tnc_queue_tx_packet(const uint8_t *data, size_t len, app_mode_t mode)
{
	if (data == NULL || len == 0 || len > TX_PKT_MAX_LEN) {
		return -EINVAL;
	}

	struct tx_packet pkt;
	pkt.uart_dev = g_tx_uart_dev;
	pkt.len = len;
	pkt.mode = mode;
	memcpy(pkt.data, data, len);

	int err = k_msgq_put(&tx_msgq, &pkt, K_NO_WAIT);
	if (err != 0) {
		printk("Warning: TX Queue full, dropped packet of len %d\n", len);
	}
	return err;
}

static bool digipeat_process_frame(ax25_frame_t *frame)
{
	if (frame == NULL || frame->digi_count == 0) {
		return false;
	}

	for (uint8_t i = 0; i < frame->digi_count; i++) {
		ax25_address_t *digi = &frame->digis[i];

		if (!digi->has_been_repeated) {
			/* Found first unused digipeater entry in path */
			char call_str[16];
			ax25_format_callsign(digi, call_str, sizeof(call_str));

			/* Check for WIDE1, WIDE2, or general digi match */
			if (strncmp(digi->callsign, "WIDE", 4) == 0 || strncmp(digi->callsign, "DIGI", 4) == 0) {
				if (digi->ssid > 0) {
					digi->ssid--;
				}
				if (digi->ssid == 0) {
					digi->has_been_repeated = true;
				}
				printk("[DIGIPEATER] Repeated via %s -> new SSID %d (H-bit=%d)\n",
				       digi->callsign, digi->ssid, digi->has_been_repeated);
				return true;
			}
		}
	}

	return false;
}

#define BLE_RAW_BUF_SIZE 1024
static uint8_t ble_raw_buf[BLE_RAW_BUF_SIZE];
static size_t ble_raw_len = 0;
static app_mode_t ble_raw_mode;

/* ble_raw_buf/ble_raw_len/ble_raw_mode are written from two different
 * threads: tnc_process_ble_bytes() runs on the Bluetooth RX thread, while
 * ble_flush_work_handler() runs on the system workqueue. Both read-modify-
 * write ble_raw_len, so unsynchronized access can tear a concurrent
 * append/flush and corrupt or truncate an in-progress frame. Held across
 * each full read-or-append operation (including the logging/decode calls
 * that consume the buffer) so the two threads are fully serialized.
 */
static K_MUTEX_DEFINE(ble_raw_lock);

static void ble_flush_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(ble_flush_work, ble_flush_work_handler);

/*
 * BLE writes arrive one per connection interval (commonly 30-50ms+ on
 * phones), so a short idle window flushes mid-packet and splits one
 * logical frame into several bogus ones. 250ms comfortably outlasts
 * realistic inter-fragment gaps while still being a short wait once the
 * sender is actually done.
 */
#define BLE_IDLE_TIMEOUT_MS 250

static void tnc_log_frame_text(const uint8_t *data, size_t len)
{
	static char text_buf[BLE_RAW_BUF_SIZE + 1];
	size_t n = (len < sizeof(text_buf) - 1) ? len : sizeof(text_buf) - 1;

	for (size_t i = 0; i < n; i++) {
		text_buf[i] = isprint(data[i]) ? (char)data[i] : '.';
	}
	text_buf[n] = '\0';

	printk("  Text: %s\n", text_buf);
}

/* Hex dump, 16 bytes per line: "  Hex[0000]: C0 00 82 A0 ..." */
static void tnc_log_frame_hex(const uint8_t *data, size_t len)
{
	char line[16 * 3 + 1];

	for (size_t off = 0; off < len; off += 16) {
		size_t n = (len - off < 16) ? (len - off) : 16;
		size_t pos = 0;

		for (size_t j = 0; j < n; j++) {
			pos += snprintf(&line[pos], sizeof(line) - pos, "%02X ", data[off + j]);
		}
		printk("  Hex[%04u]: %s\n", (unsigned int)off, line);
	}
}

/* Full frame dump used for both BLE- and radio-received KISS payloads:
 * hex bytes, printable text, and the decoded AX.25 header if it parses.
 */
static void tnc_log_ax25_payload(const uint8_t *payload, size_t len)
{
	tnc_log_frame_hex(payload, len);
	tnc_log_frame_text(payload, len);

	ax25_frame_t f;
	int decode_err = ax25_decode(payload, len, &f);
	if (decode_err == 0) {
		char src[16], dest[16];
		ax25_format_callsign(&f.src, src, sizeof(src));
		ax25_format_callsign(&f.dest, dest, sizeof(dest));
		printk("  AX.25: %s -> %s, ctrl 0x%02X, pid 0x%02X, payload %d bytes",
		       src, dest, f.control, f.pid, f.payload_len);
		for (uint8_t i = 0; i < f.digi_count; i++) {
			char digi[16];
			ax25_format_callsign(&f.digis[i], digi, sizeof(digi));
			printk(",%s", digi);
		}
		printk("\n");
	} else {
		printk("  AX.25: not decodable (err %d)\n", decode_err);
	}
}

/*
 * Most phone APRS apps don't KISS-frame anything - they write a
 * human-readable TNC2 monitor line instead, e.g.
 * "SP3LSN-9>APVEC0,WIDE1-1,WIDE2-1:!5227.76N/01654.03E>comment". Try to
 * parse that into a real AX.25 UI frame before queueing; if it doesn't
 * parse, only fall back to sending the bytes as-is when they actually
 * decode as an AX.25 frame - some apps share this same BLE link for
 * unrelated data (e.g. raw NMEA GPS sentences), and that must never get
 * blasted out over RF as if it were a packet.
 */
static void tnc_send_raw_frame(const uint8_t *data, size_t len, app_mode_t mode)
{
	ax25_frame_t frame;

	if (ax25_parse_monitor(data, len, &frame) == 0) {
		uint8_t tx_buf[AX25_MAX_FRAME_LEN];
		int tx_len = ax25_encode(&frame, tx_buf, sizeof(tx_buf));

		if (tx_len > 0) {
			printk("BLE -> TNC: Parsed TNC2 monitor line, encoded to %d-byte AX.25 UI frame\n", tx_len);
			tnc_queue_tx_packet(tx_buf, (size_t)tx_len, mode);
			return;
		}
	}

	if (ax25_decode(data, len, &frame) == 0) {
		tnc_queue_tx_packet(data, len, mode);
	} else {
		printk("BLE -> TNC: %d bytes are neither a TNC2 line nor a valid AX.25 frame, dropping\n", (int)len);
	}
}

static void ble_flush_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&ble_raw_lock, K_FOREVER);
	if (ble_raw_len > 0) {
		printk("BLE -> TNC: Assembled raw frame (%d bytes), queueing to TX engine\n", ble_raw_len);
		tnc_log_ax25_payload(ble_raw_buf, ble_raw_len);
		tnc_send_raw_frame(ble_raw_buf, ble_raw_len, ble_raw_mode);
		ble_raw_len = 0;
	}
	k_mutex_unlock(&ble_raw_lock);
}

void tnc_process_ble_bytes(const uint8_t *data, size_t len, app_mode_t mode, const struct device *uart_dev)
{
	if (data == NULL || len == 0) {
		return;
	}

	if (uart_dev != NULL && g_tx_uart_dev == NULL) {
		g_tx_uart_dev = uart_dev;
	}

	ble_raw_mode = mode;
	bool kiss_frame_completed = false;

	for (size_t i = 0; i < len; i++) {
		uint8_t cmd;
		uint8_t payload[512];
		int ret = kiss_decode_byte(&ble_kiss_dec, data[i], &cmd, payload, sizeof(payload));

		if (ret > 0) {
			/* Complete KISS frame decoded from BLE (closing FEND seen -
			 * kiss_decode_byte holds partial frames across calls, so this
			 * only fires once every byte of the frame has arrived).
			 */
			size_t payload_len = (size_t)ret;
			printk("KISS Frame Recv from BLE: cmd 0x%02X, len %d\n", cmd, payload_len);
			tnc_log_ax25_payload(payload, payload_len);

			/* Any complete KISS frame (not just cmd DATA) has already
			 * consumed these bytes - don't let the raw-accumulation
			 * fallback below re-parse them as a TNC2 line.
			 */
			k_work_cancel_delayable(&ble_flush_work);
			k_mutex_lock(&ble_raw_lock, K_FOREVER);
			ble_raw_len = 0;
			k_mutex_unlock(&ble_raw_lock);
			kiss_frame_completed = true;

			if (cmd == KISS_CMD_DATA) {
				/* Standard KISS: the host does NOT send an FCS - the TNC's
				 * modem computes it. tnc_queue_tx_packet()'s consumer
				 * (audio_tx_pwm_play_packet -> AFSKModulator) computes and
				 * appends the real one right before transmission, same as
				 * every other producer in this codebase (see ax25_encode()'s
				 * comment) - so the raw KISS payload goes in as-is.
				 */
				tnc_queue_tx_packet(payload, payload_len, mode);
			}
		}
	}

	/* Accumulate raw bytes if KISS frame not completed and not currently decoding in-frame */
	if (!kiss_frame_completed && !ble_kiss_dec.in_frame) {
		k_mutex_lock(&ble_raw_lock, K_FOREVER);
		if (ble_raw_len + len <= sizeof(ble_raw_buf)) {
			memcpy(&ble_raw_buf[ble_raw_len], data, len);
			ble_raw_len += len;
		} else {
			/* Buffer overflow: flush immediately (still under the lock -
			 * this whole operation, including the decode/log calls that
			 * read ble_raw_buf, must stay serialized against
			 * ble_flush_work_handler()).
			 */
			printk("BLE -> TNC: Raw buffer full, flushing %d bytes early\n", ble_raw_len);
			tnc_log_ax25_payload(ble_raw_buf, ble_raw_len);
			tnc_send_raw_frame(ble_raw_buf, ble_raw_len, mode);
			ble_raw_len = (len <= sizeof(ble_raw_buf)) ? len : sizeof(ble_raw_buf);
			memcpy(ble_raw_buf, data, ble_raw_len);
		}
		k_mutex_unlock(&ble_raw_lock);
		k_work_reschedule(&ble_flush_work, K_MSEC(BLE_IDLE_TIMEOUT_MS));
	}
}

void tnc_process_radio_bytes(const uint8_t *data, size_t len, app_mode_t mode, const struct device *uart_dev, ble_send_func_t ble_send)
{
	if (data == NULL || len == 0) {
		return;
	}

	if (uart_dev != NULL && g_tx_uart_dev == NULL) {
		g_tx_uart_dev = uart_dev;
	}

	uint8_t cmd;
	uint8_t payload[512];

	for (size_t i = 0; i < len; i++) {
		int ret = kiss_decode_byte(&radio_kiss_dec, data[i], &cmd, payload, sizeof(payload));

		if (ret > 0) {
			size_t payload_len = (size_t)ret;
			printk("Radio -> TNC: Frame decoded, len %d\n", payload_len);
			tnc_log_frame_hex(payload, payload_len);

			ax25_frame_t ax_frame;
			int decode_err = ax25_decode(payload, payload_len, &ax_frame);

			if (decode_err == 0) {
				char src_str[16], dest_str[16];
				ax25_format_callsign(&ax_frame.src, src_str, sizeof(src_str));
				ax25_format_callsign(&ax_frame.dest, dest_str, sizeof(dest_str));
				printk("AX.25 Frame: %s -> %s (Payload: %d bytes)\n", src_str, dest_str, ax_frame.payload_len);

				/* If DIGIPEATER mode, check if we need to retransmit on Radio */
				if (mode == APP_MODE_DIGIPEATER) {
					if (digipeat_process_frame(&ax_frame)) {
						uint8_t tx_buf[512];
						int tx_len = ax25_encode(&ax_frame, tx_buf, sizeof(tx_buf));
						if (tx_len > 0) {
							printk("[DIGIPEATER] Retransmitting repeated packet to radio (%d bytes)\n", tx_len);
							tnc_queue_tx_packet(tx_buf, tx_len, mode);
						}
					}
				}
			}

			/* Wrap in KISS frame and send over BLE NUS */
			uint8_t kiss_buf[512];
			int kiss_len = kiss_encode_frame(KISS_CMD_DATA, payload, payload_len, kiss_buf, sizeof(kiss_buf));
			if (kiss_len > 0 && ble_send != NULL) {
				ble_send(kiss_buf, kiss_len);
			}
		}
	}

	/* Fallback for raw stream when no KISS delimiters present */
	if (!radio_kiss_dec.in_frame && memchr(data, KISS_FEND, len) == NULL) {
		printk("Raw stream detected, len %d\n", len);
		/* Attempt raw AX.25 decode */
		ax25_frame_t ax_frame;
		if (ax25_decode(data, len, &ax_frame) == 0) {
			char src_str[16], dest_str[16];
			ax25_format_callsign(&ax_frame.src, src_str, sizeof(src_str));
			ax25_format_callsign(&ax_frame.dest, dest_str, sizeof(dest_str));
			printk("AX.25 Frame: %s -> %s (Payload: %d bytes)\n", src_str, dest_str, ax_frame.payload_len);

			if (mode == APP_MODE_DIGIPEATER) {
				if (digipeat_process_frame(&ax_frame)) {
					uint8_t tx_buf[512];
					int tx_len = ax25_encode(&ax_frame, tx_buf, sizeof(tx_buf));
					if (tx_len > 0) {
						printk("[DIGIPEATER] Retransmitting repeated packet to radio (%d bytes)\n", tx_len);
						tnc_queue_tx_packet(tx_buf, tx_len, mode);
					}
				}
			}

			/* Genuine binary AX.25: wrap in KISS for the BLE app */
			uint8_t kiss_buf[512];
			int kiss_len = kiss_encode_frame(KISS_CMD_DATA, data, len, kiss_buf, sizeof(kiss_buf));
			if (kiss_len > 0 && ble_send != NULL) {
				ble_send(kiss_buf, kiss_len);
			}
		} else if (ax25_parse_monitor(data, len, &ax_frame) == 0) {
			/* Not binary AX.25 - this is the radio-side TNC's own
			 * human-readable TNC2 monitor line (see ax25_parse_monitor
			 * comment above). Still a real frame though, so it needs to
			 * go through the same digipeat check as the binary path -
			 * otherwise DIGIPEATER mode can never repeat anything from a
			 * text-based attached TNC.
			 */
			char src_str[16], dest_str[16];
			ax25_format_callsign(&ax_frame.src, src_str, sizeof(src_str));
			ax25_format_callsign(&ax_frame.dest, dest_str, sizeof(dest_str));
			printk("AX.25 Frame (TNC2 text): %s -> %s (Payload: %d bytes)\n", src_str, dest_str, ax_frame.payload_len);

			if (mode == APP_MODE_DIGIPEATER) {
				if (digipeat_process_frame(&ax_frame)) {
					uint8_t tx_buf[512];
					int tx_len = ax25_encode(&ax_frame, tx_buf, sizeof(tx_buf));
					if (tx_len > 0) {
						printk("[DIGIPEATER] Retransmitting repeated packet to radio (%d bytes)\n", tx_len);
						tnc_queue_tx_packet(tx_buf, tx_len, mode);
					}
				}
			}

			/* Forward as plain text: phone apps read these directly and
			 * don't expect a KISS envelope.
			 */
			if (ble_send != NULL) {
				ble_send(data, len);
			}
		} else if (ble_send != NULL) {
			/* Doesn't parse as either binary AX.25 or a TNC2 line -
			 * forward it anyway, unexamined, same as before.
			 */
			ble_send(data, len);
		}
	}
}
