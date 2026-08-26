/*
 * Standalone host-side round-trip test for the FX.25 RS(255,239) codec.
 * fx25.c is Zephyr-free (pure C, only needs ax25_calc_crc16 from ax25.h,
 * itself dependency-free), so it's compiled and run directly here instead
 * of through the Zephyr build - the smallest thing that fails if the
 * Reed-Solomon port or the framing logic breaks.
 *
 * Build: gcc -std=c11 -o test_fx25 src/test_fx25.c src/ax25.c && ./test_fx25
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "fx25.h"

static int failures = 0;

#define CHECK(cond, msg)                                              \
	do {                                                            \
		if (cond) {                                              \
			printf("PASS: %s\n", msg);                       \
		} else {                                                  \
			printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
			failures++;                                       \
		}                                                        \
	} while (0)

int main(void)
{
	fx25_init();

	const uint8_t payload[] = "N0CALL>APRS,WIDE1-1:Hello FX.25 world!";
	size_t payload_len = sizeof(payload) - 1;

	uint8_t block[FX25_BLOCK_SIZE];
	CHECK(fx25_build_block(payload, payload_len, block) == 0, "build_block succeeds for a normal frame");

	/* Clean round trip: no corruption */
	{
		uint8_t clean[FX25_BLOCK_SIZE];
		uint8_t out[FX25_DATA_SIZE];
		size_t out_len = 0;
		memcpy(clean, block, sizeof(clean));

		int corrected = fx25_decode_block(clean, out, sizeof(out), &out_len);
		CHECK(corrected == 0, "clean block decodes with 0 corrections");
		CHECK(out_len == payload_len && memcmp(out, payload, payload_len) == 0,
		      "clean block recovers the exact payload");
	}

	/* RS(255,239) with 16 check bytes can fix up to floor(16/2) = 8 byte errors */
	{
		uint8_t corrupt[FX25_BLOCK_SIZE];
		uint8_t out[FX25_DATA_SIZE];
		size_t out_len = 0;
		memcpy(corrupt, block, sizeof(corrupt));
		for (int i = 0; i < 8; i++) {
			corrupt[i * 30] ^= 0xFF; /* spread errors across the block */
		}

		int corrected = fx25_decode_block(corrupt, out, sizeof(out), &out_len);
		CHECK(corrected == 8, "8 corrupted bytes reports exactly 8 corrections");
		CHECK(out_len == payload_len && memcmp(out, payload, payload_len) == 0,
		      "8 corrupted bytes still recovers the exact payload");
	}

	/* Beyond correction capacity (9 errors): must be rejected, never silently wrong */
	{
		uint8_t corrupt[FX25_BLOCK_SIZE];
		uint8_t out[FX25_DATA_SIZE];
		size_t out_len = 0;
		memcpy(corrupt, block, sizeof(corrupt));
		for (int i = 0; i < 9; i++) {
			corrupt[i * 27] ^= 0xFF;
		}

		int corrected = fx25_decode_block(corrupt, out, sizeof(out), &out_len);
		CHECK(corrected < 0, "9 corrupted bytes (beyond capacity) is rejected, not miscorrected");
	}

	/* A block that never contained a real AX.25 frame (all zero) must be rejected */
	{
		uint8_t garbage[FX25_BLOCK_SIZE];
		uint8_t out[FX25_DATA_SIZE];
		size_t out_len = 0;
		memset(garbage, 0, sizeof(garbage));

		int corrected = fx25_decode_block(garbage, out, sizeof(out), &out_len);
		CHECK(corrected < 0, "an all-zero (non-FX.25) block is rejected");
	}

	/* Oversized frame must fail to build, not overflow */
	{
		uint8_t big[500];
		uint8_t out_block[FX25_BLOCK_SIZE];
		memset(big, 'A', sizeof(big));
		CHECK(fx25_build_block(big, sizeof(big), out_block) != 0,
		      "oversized frame is rejected at build time, not overflowed");
	}

	if (failures == 0) {
		printf("ALL FX.25 TESTS PASSED\n");
		return 0;
	}
	printf("%d FX.25 TEST(S) FAILED\n", failures);
	return 1;
}
