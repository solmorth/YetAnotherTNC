#ifndef KISS_H_
#define KISS_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define KISS_FEND  0xC0
#define KISS_FESC  0xDB
#define KISS_TFEND 0xDC
#define KISS_TFESC 0xDD

#define KISS_CMD_DATA 0x00

typedef struct {
	uint8_t buffer[512];
	size_t index;
	bool in_frame;
	bool escape;
} kiss_decoder_t;

#ifdef __cplusplus
extern "C" {
#endif

void kiss_decoder_init(kiss_decoder_t *dec);

/* Parse stream byte-by-byte. Returns frame length > 0 when complete frame decoded, 0 if in progress, <0 on error */
int kiss_decode_byte(kiss_decoder_t *dec, uint8_t byte, uint8_t *cmd_out, uint8_t *payload_out, size_t max_len);

/* Wrap payload into KISS frame [FEND, CMD, ESCAPED_DATA, FEND] */
int kiss_encode_frame(uint8_t cmd, const uint8_t *data, size_t data_len, uint8_t *out_buf, size_t out_buf_size);

#ifdef __cplusplus
}
#endif

#endif /* KISS_H_ */
