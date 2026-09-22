/*
 * Standalone host-side test for NMEA-to-APRS position formatting
 * (gps_format.c). Zephyr-free pure C, same style as test_kiss.c/test_ax25.c.
 *
 * Build: gcc -std=c11 -o test_gps_format src/test_gps_format.c src/gps_format.c && ./test_gps_format
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "gps_format.h"

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

static void test_parse_valid_gga(void)
{
	char lat[9], lon[10];
	bool ok = gps_format_parse_gga_position(
		"$GPGGA,181908.00,3404.7041778,N,07044.3966270,W,4,13,1.00,495.144,M,29.200,M,0.10,0000*40",
		lat, lon);

	CHECK(ok, "valid GGA sentence parses");
	CHECK(strcmp(lat, "3404.70N") == 0, "latitude truncated to APRS's ddmm.hh width");
	CHECK(strcmp(lon, "07044.39W") == 0, "longitude truncated to APRS's dddmm.hh width");
}

static void test_parse_gngga_variant(void)
{
	char lat[9], lon[10];
	bool ok = gps_format_parse_gga_position("$GNGGA,,4322.20,N,00608.10,E,1,,,,,,,,", lat, lon);

	CHECK(ok, "$GNGGA (multi-constellation) sentence recognized same as $GPGGA");
	CHECK(strcmp(lat, "4322.20N") == 0, "latitude parsed from $GNGGA");
	CHECK(strcmp(lon, "00608.10E") == 0, "longitude parsed from $GNGGA");
}

static void test_parse_south_west(void)
{
	char lat[9], lon[10];
	bool ok = gps_format_parse_gga_position("$GPGGA,,2540.90,S,08021.91,W,1,,,,,,,,", lat, lon);

	CHECK(ok, "southern/western hemisphere sentence parses");
	CHECK(strcmp(lat, "2540.90S") == 0, "S hemisphere letter carried through");
	CHECK(strcmp(lon, "08021.91W") == 0, "W hemisphere letter carried through");
}

static void test_parse_rejects_non_gga(void)
{
	char lat[9], lon[10];
	CHECK(!gps_format_parse_gga_position("$GPRMC,181908.00,A,3404.70,N,07044.39,W,0.0,0.0,,,,A",
					      lat, lon),
	      "non-GGA sentence rejected");
}

static void test_parse_rejects_no_fix_yet(void)
{
	char lat[9], lon[10];
	/* Receiver hasn't acquired a fix yet: lat/lon/hemisphere fields are
	 * all empty, only the trailing fix-quality '0' is populated. */
	CHECK(!gps_format_parse_gga_position("$GPGGA,,,,,,0,00,99.99,,,,,,", lat, lon),
	      "GGA with empty lat/lon fields (no fix) rejected");
}

static void test_parse_rejects_truncated_field(void)
{
	char lat[9], lon[10];
	/* Latitude field present but too short (receiver mid-acquiring). */
	CHECK(!gps_format_parse_gga_position("$GPGGA,181908.00,34.7,N,07044.39,W,1,,,,,,,,", lat,
					      lon),
	      "latitude field shorter than ddmm.hh rejected");
}

static void test_parse_rejects_bad_hemisphere_letter(void)
{
	char lat[9], lon[10];
	CHECK(!gps_format_parse_gga_position("$GPGGA,,3404.70,X,07044.39,W,1,,,,,,,,", lat, lon),
	      "invalid N/S letter rejected");
}

static void test_build_payload_basic(void)
{
	uint8_t out[64];
	size_t len =
		gps_format_build_position_payload("3404.70N", "07044.39W", "Test comment", out,
						   sizeof(out));

	CHECK(len == 1 + 8 + 1 + 9 + 1 + 12, "payload length matches fixed fields + comment");
	CHECK(memcmp(out, "!3404.70N/07044.39W>Test comment", len) == 0,
	      "payload matches APRS101 uncompressed position format");
}

static void test_build_payload_no_comment(void)
{
	uint8_t out[64];
	size_t len = gps_format_build_position_payload("3404.70N", "07044.39W", NULL, out,
							 sizeof(out));

	CHECK(len == 1 + 8 + 1 + 9 + 1, "NULL comment treated as empty, not a crash");
	CHECK(memcmp(out, "!3404.70N/07044.39W>", len) == 0, "payload has no trailing bytes for NULL comment");
}

static void test_build_payload_truncates_long_comment(void)
{
	uint8_t out[25];
	/* Fixed fields take 20 bytes, leaving 5 for the comment. */
	size_t len = gps_format_build_position_payload("3404.70N", "07044.39W",
							 "this comment is way too long", out,
							 sizeof(out));

	CHECK(len == sizeof(out), "comment truncated to fit out_buf exactly");
	CHECK(memcmp(out, "!3404.70N/07044.39W>this ", len) == 0,
	      "truncated comment keeps only what fits");
}

static void test_build_payload_buffer_too_small_for_fixed_fields(void)
{
	uint8_t out[10];
	size_t len = gps_format_build_position_payload("3404.70N", "07044.39W", "x", out,
							 sizeof(out));

	CHECK(len == 0, "buffer too small even for lat/lon/symbol reports 0, not a partial write");
}

static void test_encode_position_ne(void)
{
	char lat[9], lon[10];

	gps_format_encode_position(34.07840f, -70.73995f, lat, lon);

	CHECK(strcmp(lat, "3404.70N") == 0, "decimal-degrees latitude encodes to DDMM.hhN");
	CHECK(strcmp(lon, "07044.40W") == 0, "decimal-degrees longitude encodes to DDDMM.hhW");
}

static void test_encode_position_sw(void)
{
	char lat[9], lon[10];

	gps_format_encode_position(-33.5000f, 151.2500f, lat, lon);

	CHECK(strcmp(lat, "3330.00S") == 0, "negative latitude encodes with S hemisphere");
	CHECK(strcmp(lon, "15115.00E") == 0, "positive longitude encodes with E hemisphere");
}

static void test_encode_position_zero(void)
{
	char lat[9], lon[10];

	gps_format_encode_position(0.0f, 0.0f, lat, lon);

	CHECK(strcmp(lat, "0000.00N") == 0, "zero latitude encodes with N hemisphere, not a crash");
	CHECK(strcmp(lon, "00000.00E") == 0, "zero longitude encodes with E hemisphere");
}

static void test_encode_position_feeds_payload_builder(void)
{
	char lat[9], lon[10];
	uint8_t out[32];

	gps_format_encode_position(34.07840f, -70.73995f, lat, lon);
	size_t len = gps_format_build_position_payload(lat, lon, NULL, out, sizeof(out));

	CHECK(len == 1 + 8 + 1 + 9 + 1, "encoded fixed position feeds the payload builder unchanged");
	CHECK(memcmp(out, "!3404.70N/07044.40W>", len) == 0,
	      "encoded fixed position produces a valid APRS position report");
}

int main(void)
{
	test_parse_valid_gga();
	test_parse_gngga_variant();
	test_parse_south_west();
	test_parse_rejects_non_gga();
	test_parse_rejects_no_fix_yet();
	test_parse_rejects_truncated_field();
	test_parse_rejects_bad_hemisphere_letter();
	test_build_payload_basic();
	test_build_payload_no_comment();
	test_build_payload_truncates_long_comment();
	test_build_payload_buffer_too_small_for_fixed_fields();
	test_encode_position_ne();
	test_encode_position_sw();
	test_encode_position_zero();
	test_encode_position_feeds_payload_builder();

	if (failures == 0) {
		printf("ALL GPS FORMAT TESTS PASSED\n");
		return 0;
	}
	printf("%d GPS FORMAT TEST(S) FAILED\n", failures);
	return 1;
}
