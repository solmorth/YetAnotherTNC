#ifndef AX25_H_
#define AX25_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AX25_CALLSIGN_LEN 6
#define AX25_ADDR_LEN 7
#define AX25_MAX_DIGIS 8
#define AX25_MAX_PAYLOAD 256
#define AX25_MAX_FRAME_LEN (AX25_ADDR_LEN * (2 + AX25_MAX_DIGIS) + 2 + AX25_MAX_PAYLOAD + 2)

#define AX25_CTRL_UI 0x03
#define AX25_PID_NO_L3 0xF0

typedef struct {
	char callsign[AX25_CALLSIGN_LEN + 1];
	uint8_t ssid;
	bool has_been_repeated; /* H-bit for digipeaters */
} ax25_address_t;

typedef struct {
	ax25_address_t dest;
	ax25_address_t src;
	ax25_address_t digis[AX25_MAX_DIGIS];
	uint8_t digi_count;
	uint8_t control;
	uint8_t pid;
	uint8_t payload[AX25_MAX_PAYLOAD];
	uint16_t payload_len;
} ax25_frame_t;

uint16_t ax25_calc_crc16(const uint8_t *buffer, size_t len);

int ax25_decode(const uint8_t *raw, size_t raw_len, ax25_frame_t *frame);
int ax25_encode(const ax25_frame_t *frame, uint8_t *out_buf, size_t out_buf_size);

void ax25_format_callsign(const ax25_address_t *addr, char *buf, size_t buf_size);
int ax25_parse_callsign(const char *str, ax25_address_t *addr);

/* Parse a TNC2 monitor line ("SRC>DEST,DIGI1,DIGI2:payload") into a UI
 * frame. Returns 0 on success, -1 if the text doesn't look like a
 * monitor line (no '>'/':' in the right order, or an empty call).
 */
int ax25_parse_monitor(const uint8_t *data, size_t len, ax25_frame_t *frame);

#endif /* AX25_H_ */
