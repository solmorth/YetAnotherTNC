#include "ax25.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

uint16_t ax25_calc_crc16(const uint8_t *buffer, size_t len)
{
	uint16_t crc = 0xFFFF;

	for (size_t i = 0; i < len; i++) {
		uint8_t byte = buffer[i];
		crc ^= byte;
		for (int b = 0; b < 8; b++) {
			if (crc & 0x0001) {
				crc = (crc >> 1) ^ 0x8408;
			} else {
				crc >>= 1;
			}
		}
	}

	return crc ^ 0xFFFF;
}

static int ax25_decode_address(const uint8_t *raw, ax25_address_t *addr, bool *is_last)
{
	int i;
	for (i = 0; i < AX25_CALLSIGN_LEN; i++) {
		/* Callsign octets always carry a shifted ASCII char with bit0
		 * clear (bit0 is only meaningful as the extension bit on the
		 * 7th/SSID octet) - if that's not true, this isn't a real
		 * AX.25 address field, just bytes that happen to be 14+ long.
		 */
		if (raw[i] & 0x01) {
			return -1;
		}
		char c = (raw[i] >> 1) & 0x7F;
		if (!(c == ' ' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
			return -1;
		}
		addr->callsign[i] = c;
	}
	/* Null-terminate and trim trailing spaces */
	addr->callsign[AX25_CALLSIGN_LEN] = '\0';
	for (i = AX25_CALLSIGN_LEN - 1; i >= 0; i--) {
		if (addr->callsign[i] == ' ') {
			addr->callsign[i] = '\0';
		} else {
			break;
		}
	}

	uint8_t ssid_byte = raw[6];
	addr->ssid = (ssid_byte >> 1) & 0x0F;
	addr->has_been_repeated = (ssid_byte & 0x80) != 0;
	if (is_last) {
		*is_last = (ssid_byte & 0x01) != 0;
	}
	return 0;
}

static int ax25_encode_address(const ax25_address_t *addr, uint8_t *out, bool is_last)
{
	size_t len = strlen(addr->callsign);
	for (int i = 0; i < AX25_CALLSIGN_LEN; i++) {
		char c = (i < len) ? addr->callsign[i] : ' ';
		if (c >= 'a' && c <= 'z') {
			c -= 32;
		}
		out[i] = (uint8_t)(c << 1);
	}

	uint8_t ssid_byte = 0x60; /* Reserved bits 1 1 */
	if (addr->has_been_repeated) {
		ssid_byte |= 0x80; /* H-bit */
	}
	ssid_byte |= ((addr->ssid & 0x0F) << 1);
	if (is_last) {
		ssid_byte |= 0x01; /* End of address field */
	}
	out[6] = ssid_byte;
	return 0;
}

int ax25_decode(const uint8_t *raw, size_t raw_len, ax25_frame_t *frame)
{
	if (raw == NULL || frame == NULL || raw_len < 14) {
		return -1;
	}

	memset(frame, 0, sizeof(*frame));
	size_t offset = 0;
	bool is_last = false;

	/* 1. Destination Address */
	if (ax25_decode_address(&raw[offset], &frame->dest, &is_last) != 0) {
		return -4; /* Not a real AX.25 address field */
	}
	offset += AX25_ADDR_LEN;
	if (is_last || offset >= raw_len) {
		return -2; /* Must have source address */
	}

	/* 2. Source Address */
	if (ax25_decode_address(&raw[offset], &frame->src, &is_last) != 0) {
		return -4;
	}
	offset += AX25_ADDR_LEN;

	/* 3. Digipeaters (0 to AX25_MAX_DIGIS) */
	while (!is_last && offset < raw_len && frame->digi_count < AX25_MAX_DIGIS) {
		if (offset + AX25_ADDR_LEN > raw_len) {
			break;
		}
		if (ax25_decode_address(&raw[offset], &frame->digis[frame->digi_count], &is_last) != 0) {
			return -4;
		}
		frame->digi_count++;
		offset += AX25_ADDR_LEN;
	}

	if (!is_last) {
		/* More than AX25_MAX_DIGIS repeater addresses (or a truncated
		 * address field) - the rest of raw[] isn't Control/PID, it's
		 * still address bytes. Reject instead of misparsing it.
		 */
		return -5;
	}

	if (offset + 2 > raw_len) {
		return -3; /* Missing control or PID */
	}

	/* 4. Control & PID */
	frame->control = raw[offset++];
	frame->pid = raw[offset++];

	/* 5. Payload
	 * No FCS to strip here: every producer that reaches ax25_decode() in
	 * this codebase (KISS DATA payloads from BLE/radio, ax25_encode()
	 * output) already excludes the FCS - it's a modem-only concept,
	 * computed fresh by AFSKModulator on TX and consumed/verified by
	 * AFSKDemodulator on RX before the payload ever gets here.
	 */
	size_t remaining = raw_len - offset;
	frame->payload_len = remaining;

	if (frame->payload_len > AX25_MAX_PAYLOAD) {
		frame->payload_len = AX25_MAX_PAYLOAD;
	}

	if (frame->payload_len > 0) {
		memcpy(frame->payload, &raw[offset], frame->payload_len);
	}

	return 0;
}

int ax25_encode(const ax25_frame_t *frame, uint8_t *out_buf, size_t out_buf_size)
{
	if (frame == NULL || out_buf == NULL) {
		return -1;
	}

	size_t total_addr_count = 2 + frame->digi_count;
	size_t req_size = (total_addr_count * AX25_ADDR_LEN) + 2 + frame->payload_len;

	if (out_buf_size < req_size) {
		return -2;
	}

	size_t offset = 0;

	/* 1. Destination Address */
	bool is_last = (total_addr_count == 1);
	ax25_encode_address(&frame->dest, &out_buf[offset], is_last);
	offset += AX25_ADDR_LEN;

	/* 2. Source Address */
	is_last = (frame->digi_count == 0);
	ax25_encode_address(&frame->src, &out_buf[offset], is_last);
	offset += AX25_ADDR_LEN;

	/* 3. Digipeaters */
	for (uint8_t i = 0; i < frame->digi_count; i++) {
		is_last = (i == frame->digi_count - 1);
		ax25_encode_address(&frame->digis[i], &out_buf[offset], is_last);
		offset += AX25_ADDR_LEN;
	}

	/* 4. Control & PID */
	out_buf[offset++] = (frame->control != 0) ? frame->control : AX25_CTRL_UI;
	out_buf[offset++] = (frame->pid != 0) ? frame->pid : AX25_PID_NO_L3;

	/* 5. Payload */
	if (frame->payload_len > 0) {
		memcpy(&out_buf[offset], frame->payload, frame->payload_len);
		offset += frame->payload_len;
	}

	/* No FCS appended here - see ax25_decode()'s comment. The FCS is a
	 * modem-only concept; AFSKModulator::modulate_frame() computes and
	 * appends the real one right before transmission.
	 */
	return (int)offset;
}

void ax25_format_callsign(const ax25_address_t *addr, char *buf, size_t buf_size)
{
	if (addr == NULL || buf == NULL || buf_size == 0) {
		return;
	}

	if (addr->ssid > 0) {
		snprintf(buf, buf_size, "%s-%d%s", addr->callsign, addr->ssid,
			 addr->has_been_repeated ? "*" : "");
	} else {
		snprintf(buf, buf_size, "%s%s", addr->callsign,
			 addr->has_been_repeated ? "*" : "");
	}
}

int ax25_parse_callsign(const char *str, ax25_address_t *addr)
{
	if (str == NULL || addr == NULL) {
		return -1;
	}

	memset(addr, 0, sizeof(*addr));
	size_t len = strlen(str);
	if (len == 0) {
		return -1;
	}

	char temp[32];
	if (len >= sizeof(temp)) {
		len = sizeof(temp) - 1;
	}
	memcpy(temp, str, len);
	temp[len] = '\0';

	/* Check trailing '*' for has_been_repeated bit */
	if (len > 0 && temp[len - 1] == '*') {
		addr->has_been_repeated = true;
		temp[len - 1] = '\0';
	}

	/* Parse SSID hyphen */
	char *dash = strchr(temp, '-');
	if (dash != NULL) {
		*dash = '\0';
		int ssid = atoi(dash + 1);
		/* Valid AX.25 SSID is a 4-bit field (0-15); ax25_encode_address
		 * masks with & 0x0F when writing to the wire, so clamp here too -
		 * otherwise ax25_format_callsign would display a value (e.g. 99)
		 * that doesn't match what actually goes out over the air.
		 */
		if (ssid < 0) {
			ssid = 0;
		} else if (ssid > 15) {
			ssid = 15;
		}
		addr->ssid = (uint8_t)ssid;
	}

	size_t call_len = strlen(temp);
	if (call_len > AX25_CALLSIGN_LEN) {
		call_len = AX25_CALLSIGN_LEN;
	}

	for (size_t i = 0; i < call_len; i++) {
		addr->callsign[i] = (char)toupper((unsigned char)temp[i]);
	}
	addr->callsign[call_len] = '\0';

	return 0;
}

static bool is_valid_callsign(const ax25_address_t *addr)
{
	if (addr->callsign[0] == '\0') {
		return false;
	}
	for (size_t i = 0; addr->callsign[i] != '\0'; i++) {
		if (!isalnum((unsigned char)addr->callsign[i])) {
			return false;
		}
	}
	return true;
}

int ax25_parse_monitor(const uint8_t *data, size_t len, ax25_frame_t *frame)
{
	if (data == NULL || frame == NULL || len == 0) {
		return -1;
	}

	char buf[512];
	size_t n = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
	memcpy(buf, data, n);
	buf[n] = '\0';

	char *gt = strchr(buf, '>');
	char *colon = strchr(buf, ':');
	if (gt == NULL || colon == NULL || colon < gt) {
		return -1;
	}

	memset(frame, 0, sizeof(*frame));

	*gt = '\0';
	if (ax25_parse_callsign(buf, &frame->src) != 0 || !is_valid_callsign(&frame->src)) {
		return -1;
	}

	*colon = '\0';
	char *path = gt + 1; /* "DEST,DIGI1,DIGI2" */

	char *comma = strchr(path, ',');
	if (comma != NULL) {
		*comma = '\0';
	}
	if (ax25_parse_callsign(path, &frame->dest) != 0 || !is_valid_callsign(&frame->dest)) {
		return -1;
	}

	char *p = (comma != NULL) ? comma + 1 : NULL;
	while (p != NULL && *p != '\0' && frame->digi_count < AX25_MAX_DIGIS) {
		char *next = strchr(p, ',');
		if (next != NULL) {
			*next = '\0';
		}
		if (ax25_parse_callsign(p, &frame->digis[frame->digi_count]) != 0 ||
		    !is_valid_callsign(&frame->digis[frame->digi_count])) {
			return -1;
		}
		frame->digi_count++;
		p = (next != NULL) ? next + 1 : NULL;
	}

	frame->control = AX25_CTRL_UI;
	frame->pid = AX25_PID_NO_L3;

	const char *payload = colon + 1;
	size_t payload_len = strlen(payload);
	if (payload_len > AX25_MAX_PAYLOAD) {
		payload_len = AX25_MAX_PAYLOAD;
	}
	memcpy(frame->payload, payload, payload_len);
	frame->payload_len = (uint16_t)payload_len;

	return 0;
}
