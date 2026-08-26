#include "kiss.h"
#include <string.h>

void kiss_decoder_init(kiss_decoder_t *dec)
{
	if (dec != NULL) {
		dec->index = 0;
		dec->in_frame = false;
		dec->escape = false;
	}
}

int kiss_decode_byte(kiss_decoder_t *dec, uint8_t byte, uint8_t *cmd_out, uint8_t *payload_out, size_t max_len)
{
	if (dec == NULL || cmd_out == NULL || payload_out == NULL) {
		return -1;
	}

	if (byte == KISS_FEND) {
		if (dec->in_frame && dec->index > 0) {
			/* Frame complete! Extract command and payload */
			uint8_t cmd = dec->buffer[0];
			size_t payload_len = dec->index - 1;

			if (payload_len > max_len) {
				payload_len = max_len;
			}

			*cmd_out = cmd;
			memcpy(payload_out, &dec->buffer[1], payload_len);

			/* Reset for the next frame. In KISS, FEND is both a
			 * terminator and a starter, so this same byte also opens
			 * the next frame - a sender may chain frames on a single
			 * shared FEND instead of a redundant FEND FEND pair.
			 */
			dec->index = 0;
			dec->in_frame = true;
			dec->escape = false;

			return (int)payload_len;
		} else {
			/* Start of a new frame */
			dec->in_frame = true;
			dec->index = 0;
			dec->escape = false;
			return 0;
		}
	}

	if (!dec->in_frame) {
		return 0;
	}

	if (dec->escape) {
		dec->escape = false;
		if (byte == KISS_TFEND) {
			byte = KISS_FEND;
		} else if (byte == KISS_TFESC) {
			byte = KISS_FESC;
		}
	} else if (byte == KISS_FESC) {
		dec->escape = true;
		return 0;
	}

	if (dec->index < sizeof(dec->buffer)) {
		dec->buffer[dec->index++] = byte;
	} else {
		/* Overflow: reset frame state */
		dec->index = 0;
		dec->in_frame = false;
		dec->escape = false;
		return -2;
	}

	return 0;
}

int kiss_encode_frame(uint8_t cmd, const uint8_t *data, size_t data_len, uint8_t *out_buf, size_t out_buf_size)
{
	if (out_buf == NULL || out_buf_size < 3) {
		return -1;
	}

	size_t idx = 0;

	/* Start FEND */
	out_buf[idx++] = KISS_FEND;

	/* Command Byte */
	out_buf[idx++] = cmd;

	/* Escaped payload */
	for (size_t i = 0; i < data_len; i++) {
		uint8_t b = data[i];

		if (b == KISS_FEND) {
			if (idx + 2 >= out_buf_size) return -2;
			out_buf[idx++] = KISS_FESC;
			out_buf[idx++] = KISS_TFEND;
		} else if (b == KISS_FESC) {
			if (idx + 2 >= out_buf_size) return -2;
			out_buf[idx++] = KISS_FESC;
			out_buf[idx++] = KISS_TFESC;
		} else {
			if (idx + 1 >= out_buf_size) return -2;
			out_buf[idx++] = b;
		}
	}

	/* End FEND */
	if (idx + 1 > out_buf_size) {
		return -2;
	}
	out_buf[idx++] = KISS_FEND;

	return (int)idx;
}
