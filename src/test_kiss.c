/*
 * Standalone host-side test for the KISS framing codec (kiss.c).
 * Zephyr-free pure C, same style as test_fx25.c/test_ax25.c.
 *
 * Build: gcc -std=c11 -o test_kiss src/test_kiss.c src/kiss.c && ./test_kiss
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "kiss.h"

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

/* Feed a byte string through the decoder, byte by byte. Returns the
 * number of the byte (index) that completed a frame, or -1 if none did.
 * On completion, cmd_out/payload_out/len_out are filled in.
 */
static int feed(kiss_decoder_t *dec, const uint8_t *data, size_t len,
		 uint8_t *cmd_out, uint8_t *payload_out, size_t max_len, size_t *len_out)
{
	for (size_t i = 0; i < len; i++) {
		int ret = kiss_decode_byte(dec, data[i], cmd_out, payload_out, max_len);
		if (ret > 0) {
			*len_out = (size_t)ret;
			return (int)i;
		}
		if (ret < 0) {
			return -2;
		}
	}
	return -1;
}

static void test_decode_simple_frame(void)
{
	kiss_decoder_t dec;
	kiss_decoder_init(&dec);

	uint8_t frame[] = {KISS_FEND, 0x00, 'H', 'I', KISS_FEND};
	uint8_t cmd, payload[64];
	size_t out_len;

	int idx = feed(&dec, frame, sizeof(frame), &cmd, payload, sizeof(payload), &out_len);
	CHECK(idx == (int)(sizeof(frame) - 1), "frame completes on closing FEND");
	CHECK(cmd == 0x00, "command byte decoded");
	CHECK(out_len == 2 && memcmp(payload, "HI", 2) == 0, "payload decoded correctly");
}

static void test_decode_ignores_bytes_before_frame(void)
{
	kiss_decoder_t dec;
	kiss_decoder_init(&dec);

	/* Bytes before the opening FEND must be dropped, not buffered. */
	uint8_t frame[] = {'x', 'y', KISS_FEND, 0x00, 'A', KISS_FEND};
	uint8_t cmd, payload[64];
	size_t out_len;

	int idx = feed(&dec, frame, sizeof(frame), &cmd, payload, sizeof(payload), &out_len);
	CHECK(idx == (int)(sizeof(frame) - 1), "frame completes despite leading garbage");
	CHECK(out_len == 1 && payload[0] == 'A', "leading garbage bytes are not part of payload");
}

static void test_decode_escaped_fend_and_fesc(void)
{
	kiss_decoder_t dec;
	kiss_decoder_init(&dec);

	/* Payload contains a literal FEND (0xC0) and FESC (0xDB), which must
	 * arrive escaped on the wire as FESC,TFEND and FESC,TFESC. */
	uint8_t frame[] = {KISS_FEND, 0x00, KISS_FESC, KISS_TFEND, KISS_FESC, KISS_TFESC, 'Z', KISS_FEND};
	uint8_t cmd, payload[64];
	size_t out_len;

	int idx = feed(&dec, frame, sizeof(frame), &cmd, payload, sizeof(payload), &out_len);
	CHECK(idx == (int)(sizeof(frame) - 1), "escaped frame completes");
	uint8_t expected[] = {KISS_FEND, KISS_FESC, 'Z'};
	CHECK(out_len == sizeof(expected) && memcmp(payload, expected, sizeof(expected)) == 0,
	      "escaped FEND/FESC decode back to literal bytes");
}

static void test_decode_empty_frame_ignored(void)
{
	kiss_decoder_t dec;
	kiss_decoder_init(&dec);

	uint8_t cmd, payload[64];
	/* FEND FEND: opens a frame then immediately closes with index==0,
	 * which the decoder treats as "start of a new frame", not a complete
	 * empty one - so no frame should be reported yet. */
	CHECK(kiss_decode_byte(&dec, KISS_FEND, &cmd, payload, sizeof(payload)) == 0, "opening FEND returns 0");
	CHECK(kiss_decode_byte(&dec, KISS_FEND, &cmd, payload, sizeof(payload)) == 0,
	      "back-to-back FEND with no data in between does not emit a frame");
}

static void test_decode_payload_truncated_to_max_len(void)
{
	kiss_decoder_t dec;
	kiss_decoder_init(&dec);

	uint8_t frame[] = {KISS_FEND, 0x00, 'A', 'B', 'C', 'D', 'E', KISS_FEND};
	uint8_t cmd, payload[3];
	size_t out_len;

	int idx = feed(&dec, frame, sizeof(frame), &cmd, payload, sizeof(payload), &out_len);
	CHECK(idx == (int)(sizeof(frame) - 1), "frame completes even when payload exceeds caller's buffer");
	CHECK(out_len == 3 && memcmp(payload, "ABC", 3) == 0, "payload truncated to caller-provided max_len");
}

static void test_decode_internal_buffer_overflow(void)
{
	kiss_decoder_t dec;
	kiss_decoder_init(&dec);

	uint8_t cmd, payload[600];
	CHECK(kiss_decode_byte(&dec, KISS_FEND, &cmd, payload, sizeof(payload)) == 0, "opening FEND");

	int ret = 0;
	/* Internal dec->buffer is 512 bytes; feed more than that without a
	 * closing FEND to force the overflow branch. */
	for (int i = 0; i < 513; i++) {
		ret = kiss_decode_byte(&dec, 'A', &cmd, payload, sizeof(payload));
		if (ret != 0) {
			break;
		}
	}
	CHECK(ret == -2, "internal buffer overflow reports -2");
	CHECK(dec.in_frame == false, "decoder resets out of in_frame state after overflow");
}

static void test_decode_null_args_rejected(void)
{
	kiss_decoder_t dec;
	kiss_decoder_init(&dec);
	uint8_t cmd, payload[8];

	CHECK(kiss_decode_byte(NULL, KISS_FEND, &cmd, payload, sizeof(payload)) == -1, "NULL decoder rejected");
	CHECK(kiss_decode_byte(&dec, KISS_FEND, NULL, payload, sizeof(payload)) == -1, "NULL cmd_out rejected");
	CHECK(kiss_decode_byte(&dec, KISS_FEND, &cmd, NULL, sizeof(payload)) == -1, "NULL payload_out rejected");
}

static void test_decode_two_frames_back_to_back(void)
{
	kiss_decoder_t dec;
	kiss_decoder_init(&dec);

	/* kiss_encode_frame() always emits its own opening AND closing FEND,
	 * so two concatenated encoded frames naturally produce a double FEND
	 * boundary (...FEND FEND...) - the extra FEND is a harmless no-op
	 * restart on top of the already-open next frame. */
	uint8_t stream[] = {KISS_FEND, 0x00, '1', KISS_FEND, KISS_FEND, 0x00, '2', KISS_FEND};
	uint8_t cmd, payload[64];
	int frames_decoded = 0;

	for (size_t i = 0; i < sizeof(stream); i++) {
		int ret = kiss_decode_byte(&dec, stream[i], &cmd, payload, sizeof(payload));
		if (ret > 0) {
			frames_decoded++;
			CHECK((frames_decoded == 1 && payload[0] == '1') ||
			      (frames_decoded == 2 && payload[0] == '2'),
			      "back-to-back frames decode in order");
		}
	}
	CHECK(frames_decoded == 2, "two frames decoded from a double-FEND-separated stream");
}

static void test_decode_single_shared_fend_between_frames(void)
{
	kiss_decoder_t dec;
	kiss_decoder_init(&dec);

	/* FEND is both a terminator and a starter: a sender may chain frames
	 * on a single shared FEND (the "canonical" KISS optimization) instead
	 * of a redundant FEND FEND pair, and both must decode correctly. */
	uint8_t stream[] = {KISS_FEND, 0x00, '1', KISS_FEND, 0x00, '2', KISS_FEND};
	uint8_t cmd, payload[64];
	int frames_decoded = 0;

	for (size_t i = 0; i < sizeof(stream); i++) {
		int ret = kiss_decode_byte(&dec, stream[i], &cmd, payload, sizeof(payload));
		if (ret > 0) {
			frames_decoded++;
			CHECK((frames_decoded == 1 && payload[0] == '1') ||
			      (frames_decoded == 2 && payload[0] == '2'),
			      "frames sharing a single FEND boundary decode in order");
		}
	}
	CHECK(frames_decoded == 2, "single shared FEND between frames yields both frames");
}

static void test_encode_basic(void)
{
	uint8_t out[64];
	const uint8_t data[] = "HI";
	int len = kiss_encode_frame(KISS_CMD_DATA, data, 2, out, sizeof(out));
	CHECK(len == 5, "simple encode produces FEND CMD H I FEND (5 bytes)");
	uint8_t expected[] = {KISS_FEND, 0x00, 'H', 'I', KISS_FEND};
	CHECK(memcmp(out, expected, sizeof(expected)) == 0, "encoded bytes match expected framing");
}

static void test_encode_escapes_special_bytes(void)
{
	uint8_t out[64];
	const uint8_t data[] = {KISS_FEND, KISS_FESC};
	int len = kiss_encode_frame(KISS_CMD_DATA, data, sizeof(data), out, sizeof(out));
	uint8_t expected[] = {KISS_FEND, 0x00, KISS_FESC, KISS_TFEND, KISS_FESC, KISS_TFESC, KISS_FEND};
	CHECK(len == (int)sizeof(expected), "escaped encode produces correct length");
	CHECK(memcmp(out, expected, sizeof(expected)) == 0, "FEND/FESC bytes in payload are escaped on encode");
}

static void test_encode_round_trips_through_decode(void)
{
	uint8_t encoded[64];
	const uint8_t data[] = {KISS_FEND, 'A', KISS_FESC, 'B', 0x00, 0xFF};
	int enc_len = kiss_encode_frame(0x00, data, sizeof(data), encoded, sizeof(encoded));
	CHECK(enc_len > 0, "encode with mixed special/normal bytes succeeds");

	kiss_decoder_t dec;
	kiss_decoder_init(&dec);
	uint8_t cmd, payload[64];
	size_t out_len = 0;
	int idx = feed(&dec, encoded, (size_t)enc_len, &cmd, payload, sizeof(payload), &out_len);
	CHECK(idx == enc_len - 1, "encoded frame decodes completely");
	CHECK(out_len == sizeof(data) && memcmp(payload, data, sizeof(data)) == 0,
	      "encode -> decode round trips to the original bytes");
}

static void test_encode_output_buffer_too_small(void)
{
	uint8_t out[2];
	const uint8_t data[] = "HI";
	CHECK(kiss_encode_frame(0x00, data, 2, out, sizeof(out)) == -1, "out_buf_size < 3 rejected up front");

	uint8_t out2[4]; /* room for FEND, CMD, but not payload + closing FEND */
	CHECK(kiss_encode_frame(0x00, data, 2, out2, sizeof(out2)) == -2, "undersized buffer mid-payload rejected");

	CHECK(kiss_encode_frame(0x00, NULL, 0, NULL, 10) == -1, "NULL out_buf rejected");
}

static void test_encode_empty_payload(void)
{
	uint8_t out[8];
	int len = kiss_encode_frame(0x00, NULL, 0, out, sizeof(out));
	CHECK(len == 3, "empty payload still frames as FEND CMD FEND");
	uint8_t expected[] = {KISS_FEND, 0x00, KISS_FEND};
	CHECK(memcmp(out, expected, sizeof(expected)) == 0, "empty-payload framing bytes correct");
}

int main(void)
{
	test_decode_simple_frame();
	test_decode_ignores_bytes_before_frame();
	test_decode_escaped_fend_and_fesc();
	test_decode_empty_frame_ignored();
	test_decode_payload_truncated_to_max_len();
	test_decode_internal_buffer_overflow();
	test_decode_null_args_rejected();
	test_decode_two_frames_back_to_back();
	test_decode_single_shared_fend_between_frames();
	test_encode_basic();
	test_encode_escapes_special_bytes();
	test_encode_round_trips_through_decode();
	test_encode_output_buffer_too_small();
	test_encode_empty_payload();

	if (failures == 0) {
		printf("ALL KISS TESTS PASSED\n");
		return 0;
	}
	printf("%d KISS TEST(S) FAILED\n", failures);
	return 1;
}
