/*
 * Standalone host-side test for the AX.25 address/frame codec (ax25.c).
 * Zephyr-free pure C, same style as test_fx25.c - compiled directly with
 * gcc and run on host instead of through the Zephyr build.
 *
 * Build: gcc -std=c11 -o test_ax25 src/test_ax25.c src/ax25.c && ./test_ax25
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "ax25.h"

static int failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                  \
		if (cond) {                                                   \
			printf("PASS: %s\n", msg);                            \
		} else {                                                      \
			printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
			failures++;                                           \
		}                                                             \
	} while (0)

static void test_crc16(void)
{
	const uint8_t data[] = "123456789";
	CHECK(ax25_calc_crc16(data, 9) == 0x906E, "CRC16 matches known test vector");
}

static void test_parse_callsign_basic(void)
{
	ax25_address_t addr;
	CHECK(ax25_parse_callsign("n0call", &addr) == 0, "lowercase callsign parses");
	CHECK(strcmp(addr.callsign, "N0CALL") == 0, "callsign is upper-cased");
	CHECK(addr.ssid == 0, "no hyphen means SSID 0");
	CHECK(!addr.has_been_repeated, "no trailing '*' means not repeated");
}

static void test_parse_callsign_ssid_and_star(void)
{
	ax25_address_t addr;
	CHECK(ax25_parse_callsign("WIDE1-1*", &addr) == 0, "SSID+star parses");
	CHECK(strcmp(addr.callsign, "WIDE1") == 0, "callsign portion correct");
	CHECK(addr.ssid == 1, "SSID parsed correctly");
	CHECK(addr.has_been_repeated, "trailing '*' sets has_been_repeated");
}

static void test_parse_callsign_ssid_clamped(void)
{
	ax25_address_t addr;
	CHECK(ax25_parse_callsign("N0CALL-99", &addr) == 0, "out-of-range SSID still parses");
	CHECK(addr.ssid == 15, "SSID > 15 clamps to 15");

	CHECK(ax25_parse_callsign("N0CALL--5", &addr) == 0, "negative SSID still parses");
	CHECK(addr.ssid == 0, "negative SSID clamps to 0");
}

static void test_parse_callsign_too_long(void)
{
	ax25_address_t addr;
	CHECK(ax25_parse_callsign("N0CALLTOOLONG", &addr) == 0, "overlong callsign still parses");
	CHECK(strlen(addr.callsign) == AX25_CALLSIGN_LEN, "callsign truncated to 6 chars");
}

static void test_parse_callsign_invalid(void)
{
	ax25_address_t addr;
	CHECK(ax25_parse_callsign(NULL, &addr) != 0, "NULL string rejected");
	CHECK(ax25_parse_callsign("", &addr) != 0, "empty string rejected");
}

static void test_format_callsign(void)
{
	ax25_address_t addr;
	char buf[32];

	memset(&addr, 0, sizeof(addr));
	strcpy(addr.callsign, "N0CALL");
	ax25_format_callsign(&addr, buf, sizeof(buf));
	CHECK(strcmp(buf, "N0CALL") == 0, "SSID 0, not repeated: no suffix");

	addr.ssid = 9;
	ax25_format_callsign(&addr, buf, sizeof(buf));
	CHECK(strcmp(buf, "N0CALL-9") == 0, "nonzero SSID appends -N");

	addr.has_been_repeated = true;
	ax25_format_callsign(&addr, buf, sizeof(buf));
	CHECK(strcmp(buf, "N0CALL-9*") == 0, "H-bit appends '*'");
}

/* Build a frame, encode it, decode it back, and check round trip fidelity. */
static void test_encode_decode_round_trip(void)
{
	ax25_frame_t frame;
	memset(&frame, 0, sizeof(frame));

	ax25_parse_callsign("N0CALL", &frame.src);
	ax25_parse_callsign("APRS", &frame.dest);
	ax25_parse_callsign("WIDE1-1", &frame.digis[0]);
	ax25_parse_callsign("WIDE2-2", &frame.digis[1]);
	frame.digi_count = 2;

	const char *payload = "Hello AX.25!";
	memcpy(frame.payload, payload, strlen(payload));
	frame.payload_len = (uint16_t)strlen(payload);

	uint8_t buf[AX25_MAX_FRAME_LEN];
	int enc_len = ax25_encode(&frame, buf, sizeof(buf));
	CHECK(enc_len > 0, "encode succeeds");

	ax25_frame_t decoded;
	CHECK(ax25_decode(buf, (size_t)enc_len, &decoded) == 0, "decode succeeds on encoded frame");
	CHECK(strcmp(decoded.src.callsign, "N0CALL") == 0, "src callsign round trips");
	CHECK(strcmp(decoded.dest.callsign, "APRS") == 0, "dest callsign round trips");
	CHECK(decoded.digi_count == 2, "digi count round trips");
	CHECK(strcmp(decoded.digis[0].callsign, "WIDE1") == 0 && decoded.digis[0].ssid == 1,
	      "first digi round trips");
	CHECK(strcmp(decoded.digis[1].callsign, "WIDE2") == 0 && decoded.digis[1].ssid == 2,
	      "second digi round trips");
	CHECK(decoded.control == AX25_CTRL_UI, "control defaults to UI");
	CHECK(decoded.pid == AX25_PID_NO_L3, "pid defaults to no-layer-3");
	CHECK(decoded.payload_len == strlen(payload) &&
	      memcmp(decoded.payload, payload, decoded.payload_len) == 0,
	      "payload round trips");
}

static void test_encode_no_digis(void)
{
	ax25_frame_t frame;
	memset(&frame, 0, sizeof(frame));
	ax25_parse_callsign("N0CALL", &frame.src);
	ax25_parse_callsign("APRS", &frame.dest);

	uint8_t buf[AX25_MAX_FRAME_LEN];
	int enc_len = ax25_encode(&frame, buf, sizeof(buf));
	CHECK(enc_len == AX25_ADDR_LEN * 2 + 2, "no-digi frame encodes to exactly 2 addresses + ctrl/pid");

	ax25_frame_t decoded;
	CHECK(ax25_decode(buf, (size_t)enc_len, &decoded) == 0, "no-digi frame decodes");
	CHECK(decoded.digi_count == 0, "digi count is 0");
}

static void test_encode_buffer_too_small(void)
{
	ax25_frame_t frame;
	memset(&frame, 0, sizeof(frame));
	ax25_parse_callsign("N0CALL", &frame.src);
	ax25_parse_callsign("APRS", &frame.dest);

	uint8_t buf[10]; /* too small for even the two addresses + ctrl/pid */
	CHECK(ax25_encode(&frame, buf, sizeof(buf)) == -2, "undersized output buffer is rejected");
}

static void test_decode_invalid_inputs(void)
{
	ax25_frame_t frame;
	CHECK(ax25_decode(NULL, 20, &frame) == -1, "NULL raw buffer rejected");
	CHECK(ax25_decode((const uint8_t *)"short", 5, &frame) == -1, "too-short buffer rejected (< 14 bytes)");

	uint8_t garbage[20];
	memset(garbage, 0xFF, sizeof(garbage));
	CHECK(ax25_decode(garbage, sizeof(garbage), &frame) == -4, "non-address bytes (bit0 set) rejected");
}

static void test_decode_missing_source(void)
{
	ax25_frame_t frame;
	ax25_address_t dest;
	memset(&dest, 0, sizeof(dest));
	ax25_parse_callsign("APRS", &dest);

	uint8_t raw[14];
	memset(raw, 0, sizeof(raw));
	/* Manually encode just the destination address, but with the
	 * end-of-address bit set on it - claims no source address follows. */
	ax25_frame_t tmp;
	memset(&tmp, 0, sizeof(tmp));
	tmp.dest = dest;
	tmp.src = dest;
	uint8_t full[AX25_MAX_FRAME_LEN];
	ax25_encode(&tmp, full, sizeof(full));
	memcpy(raw, full, AX25_ADDR_LEN);
	raw[6] |= 0x01; /* force is_last on the destination address itself */

	CHECK(ax25_decode(raw, sizeof(raw), &frame) == -2, "destination marked as last address is rejected (no source)");
}

static void test_decode_truncated_after_addresses(void)
{
	ax25_frame_t frame;
	ax25_frame_t tmp;
	memset(&tmp, 0, sizeof(tmp));
	ax25_parse_callsign("N0CALL", &tmp.src);
	ax25_parse_callsign("APRS", &tmp.dest);

	uint8_t buf[AX25_MAX_FRAME_LEN];
	int len = ax25_encode(&tmp, buf, sizeof(buf));
	CHECK(ax25_decode(buf, (size_t)len - 1, &frame) == -3, "frame truncated before PID byte is rejected");
}

static void test_decode_oversized_payload_truncates(void)
{
	ax25_frame_t frame;
	memset(&frame, 0, sizeof(frame));
	ax25_parse_callsign("N0CALL", &frame.src);
	ax25_parse_callsign("APRS", &frame.dest);
	frame.payload_len = AX25_MAX_PAYLOAD;
	memset(frame.payload, 'A', frame.payload_len);

	uint8_t buf[AX25_ADDR_LEN * 2 + 2 + AX25_MAX_PAYLOAD + 50];
	size_t offset = 0;
	/* Build manually with an oversized payload beyond AX25_MAX_PAYLOAD to
	 * exercise ax25_decode's truncation path (ax25_encode's own frame
	 * struct can't hold more than AX25_MAX_PAYLOAD in the first place). */
	int enc_len = ax25_encode(&frame, buf, sizeof(buf));
	CHECK(enc_len > 0, "max-size payload frame encodes");

	uint8_t raw[AX25_ADDR_LEN * 2 + 2 + AX25_MAX_PAYLOAD + 50];
	memcpy(raw, buf, (size_t)enc_len);
	/* Append extra bytes beyond AX25_MAX_PAYLOAD to force decode's clamp. */
	size_t extra = 20;
	memset(raw + enc_len, 'B', extra);
	offset = (size_t)enc_len + extra;

	ax25_frame_t decoded;
	CHECK(ax25_decode(raw, offset, &decoded) == 0, "oversized-payload frame still decodes");
	CHECK(decoded.payload_len == AX25_MAX_PAYLOAD, "payload is clamped to AX25_MAX_PAYLOAD, not overflowed");
}

static void test_parse_monitor_basic(void)
{
	const char *line = "N0CALL>APRS,WIDE1-1,WIDE2-2:Hello world!";
	ax25_frame_t frame;
	CHECK(ax25_parse_monitor((const uint8_t *)line, strlen(line), &frame) == 0, "TNC2 monitor line parses");
	CHECK(strcmp(frame.src.callsign, "N0CALL") == 0, "src parsed from monitor line");
	CHECK(strcmp(frame.dest.callsign, "APRS") == 0, "dest parsed from monitor line");
	CHECK(frame.digi_count == 2, "two digis parsed from monitor line");
	CHECK(strcmp(frame.digis[0].callsign, "WIDE1") == 0 && frame.digis[0].ssid == 1, "digi 1 correct");
	CHECK(strcmp(frame.digis[1].callsign, "WIDE2") == 0 && frame.digis[1].ssid == 2, "digi 2 correct");
	CHECK(frame.control == AX25_CTRL_UI, "control set to UI");
	CHECK(frame.pid == AX25_PID_NO_L3, "pid set to no-layer-3");
	CHECK(frame.payload_len == strlen("Hello world!") &&
	      memcmp(frame.payload, "Hello world!", frame.payload_len) == 0,
	      "payload parsed from monitor line");
}

static void test_parse_monitor_no_digis(void)
{
	const char *line = "N0CALL>APRS:no digis here";
	ax25_frame_t frame;
	CHECK(ax25_parse_monitor((const uint8_t *)line, strlen(line), &frame) == 0, "monitor line without digis parses");
	CHECK(frame.digi_count == 0, "digi count is 0");
}

static void test_parse_monitor_rejects_non_monitor_text(void)
{
	ax25_frame_t frame;
	const char *no_colon = "N0CALL>APRS";
	CHECK(ax25_parse_monitor((const uint8_t *)no_colon, strlen(no_colon), &frame) != 0,
	      "line missing ':' is rejected");

	const char *no_gt = "N0CALL:APRS hello";
	CHECK(ax25_parse_monitor((const uint8_t *)no_gt, strlen(no_gt), &frame) != 0,
	      "line missing '>' is rejected");

	const char *reversed = "N0CALL:APRS>hello"; /* colon before '>' */
	CHECK(ax25_parse_monitor((const uint8_t *)reversed, strlen(reversed), &frame) != 0,
	      "':' before '>' is rejected");

	const char *empty_src = ">APRS:hello";
	CHECK(ax25_parse_monitor((const uint8_t *)empty_src, strlen(empty_src), &frame) != 0,
	      "empty source callsign is rejected");
}

static void test_encode_null_args_and_hbit(void)
{
	ax25_frame_t frame;
	uint8_t buf[AX25_MAX_FRAME_LEN];
	CHECK(ax25_encode(NULL, buf, sizeof(buf)) == -1, "NULL frame rejected");
	CHECK(ax25_encode(&frame, NULL, sizeof(buf)) == -1, "NULL out_buf rejected");

	memset(&frame, 0, sizeof(frame));
	ax25_parse_callsign("N0CALL", &frame.src);
	ax25_parse_callsign("APRS", &frame.dest);
	ax25_parse_callsign("WIDE1-1", &frame.digis[0]);
	frame.digis[0].has_been_repeated = true;
	frame.digi_count = 1;

	int len = ax25_encode(&frame, buf, sizeof(buf));
	CHECK(len > 0, "encode with a repeated digi succeeds");

	ax25_frame_t decoded;
	CHECK(ax25_decode(buf, (size_t)len, &decoded) == 0, "decode round trips the repeated digi");
	CHECK(decoded.digis[0].has_been_repeated, "H-bit is set on encode and survives decode");
}

static void test_format_callsign_null_args(void)
{
	ax25_address_t addr;
	memset(&addr, 0, sizeof(addr));
	char buf[8];
	/* Must not crash on any of these - nothing to CHECK() but the run itself. */
	ax25_format_callsign(NULL, buf, sizeof(buf));
	ax25_format_callsign(&addr, NULL, sizeof(buf));
	ax25_format_callsign(&addr, buf, 0);
	CHECK(true, "format_callsign tolerates NULL/zero-size args without crashing");
}

static void test_parse_callsign_overlong_input(void)
{
	ax25_address_t addr;
	char long_input[64];
	memset(long_input, 'A', sizeof(long_input) - 1);
	long_input[sizeof(long_input) - 1] = '\0';

	CHECK(ax25_parse_callsign(long_input, &addr) == 0, "input longer than internal temp buffer still parses");
	CHECK(strlen(addr.callsign) == AX25_CALLSIGN_LEN, "callsign still truncated to 6 chars");
}

static void test_decode_broken_digipeater_field(void)
{
	ax25_frame_t frame;
	ax25_frame_t tmp;
	memset(&tmp, 0, sizeof(tmp));
	ax25_parse_callsign("N0CALL", &tmp.src);
	ax25_parse_callsign("APRS", &tmp.dest);
	ax25_parse_callsign("WIDE1-1", &tmp.digis[0]);
	tmp.digi_count = 1;

	uint8_t buf[AX25_MAX_FRAME_LEN];
	int len = ax25_encode(&tmp, buf, sizeof(buf));
	CHECK(len > 0, "one-digi frame encodes");

	/* Truncate right in the middle of the (only) digipeater address field -
	 * the loop's "offset + AX25_ADDR_LEN > raw_len" guard should break out,
	 * is_last never gets set, and decode must reject rather than misparse
	 * whatever's left as control/PID. */
	(void)len;
	size_t cut_len = 2 * AX25_ADDR_LEN + 3; /* dest+src fully present, 3 bytes into the digi field */
	CHECK(ax25_decode(buf, cut_len, &frame) == -5, "address field truncated mid-digipeater is rejected");
}

static void test_parse_monitor_invalid_callsign_chars(void)
{
	ax25_frame_t frame;
	CHECK(ax25_parse_monitor(NULL, 5, &frame) != 0, "NULL data rejected");

	const char *bad_dest = "N0CALL>AP.RS:hello";
	CHECK(ax25_parse_monitor((const uint8_t *)bad_dest, strlen(bad_dest), &frame) != 0,
	      "non-alphanumeric char in dest callsign is rejected");

	const char *bad_digi = "N0CALL>APRS,W!DE1-1:hello";
	CHECK(ax25_parse_monitor((const uint8_t *)bad_digi, strlen(bad_digi), &frame) != 0,
	      "non-alphanumeric char in digi callsign is rejected");
}

static void test_parse_monitor_oversized_payload_truncates(void)
{
	char line[900];
	memcpy(line, "N0CALL>APRS:", 12);
	memset(line + 12, 'X', sizeof(line) - 12 - 1);
	line[sizeof(line) - 1] = '\0';

	ax25_frame_t frame;
	CHECK(ax25_parse_monitor((const uint8_t *)line, strlen(line), &frame) == 0, "oversized monitor payload still parses");
	CHECK(frame.payload_len == AX25_MAX_PAYLOAD, "payload clamped to AX25_MAX_PAYLOAD");
}

static void test_parse_monitor_empty_payload(void)
{
	const char *line = "N0CALL>APRS:";
	ax25_frame_t frame;
	CHECK(ax25_parse_monitor((const uint8_t *)line, strlen(line), &frame) == 0, "empty payload still parses");
	CHECK(frame.payload_len == 0, "payload length is 0");
}

int main(void)
{
	test_crc16();
	test_parse_callsign_basic();
	test_parse_callsign_ssid_and_star();
	test_parse_callsign_ssid_clamped();
	test_parse_callsign_too_long();
	test_parse_callsign_invalid();
	test_format_callsign();
	test_encode_decode_round_trip();
	test_encode_no_digis();
	test_encode_buffer_too_small();
	test_decode_invalid_inputs();
	test_decode_missing_source();
	test_decode_truncated_after_addresses();
	test_decode_oversized_payload_truncates();
	test_encode_null_args_and_hbit();
	test_format_callsign_null_args();
	test_parse_callsign_overlong_input();
	test_decode_broken_digipeater_field();
	test_parse_monitor_basic();
	test_parse_monitor_no_digis();
	test_parse_monitor_rejects_non_monitor_text();
	test_parse_monitor_invalid_callsign_chars();
	test_parse_monitor_oversized_payload_truncates();
	test_parse_monitor_empty_payload();

	if (failures == 0) {
		printf("ALL AX.25 TESTS PASSED\n");
		return 0;
	}
	printf("%d AX.25 TEST(S) FAILED\n", failures);
	return 1;
}
