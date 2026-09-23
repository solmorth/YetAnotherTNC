#include "gps_format.h"
#include <stdio.h>
#include <string.h>

/* Walks n commas into line, returning a pointer at the comma just before
 * field n (so field[1] is the field's first character), or NULL if line
 * doesn't have n commas.
 */
static const char *nth_field(const char *line, int n)
{
	const char *field = line;

	for (int i = 0; i < n && field != NULL; i++) {
		field = strchr(field + 1, ',');
	}
	return field;
}

/* True if none of field[0..len-1] is the field terminator (',' or '\0') -
 * i.e. the field has at least len characters of actual content.
 */
static bool field_has_min_len(const char *field, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (field[i] == '\0' || field[i] == ',') {
			return false;
		}
	}
	return true;
}

bool gps_format_parse_gga_position(const char *line, char lat_out[9], char lon_out[10])
{
	if (strncmp(line, "$GPGGA", 6) != 0 && strncmp(line, "$GNGGA", 6) != 0) {
		return false;
	}

	/* $GPGGA,time,lat,N/S,lon,E/W,fix_quality,... */
	const char *lat_field = nth_field(line, 2);
	const char *ns_field = nth_field(line, 3);
	const char *lon_field = nth_field(line, 4);
	const char *ew_field = nth_field(line, 5);

	if (lat_field == NULL || ns_field == NULL || lon_field == NULL || ew_field == NULL) {
		return false;
	}
	lat_field++; /* skip the comma itself to land on the field's content */
	ns_field++;
	lon_field++;
	ew_field++;

	/* "ddmm.hh" = 7 chars, "dddmm.hh" = 8 chars; anything shorter means
	 * the receiver hasn't filled this field in yet (no fix).
	 */
	if (!field_has_min_len(lat_field, 7) || (ns_field[0] != 'N' && ns_field[0] != 'S') ||
	    !field_has_min_len(lon_field, 8) || (ew_field[0] != 'E' && ew_field[0] != 'W')) {
		return false;
	}

	memcpy(lat_out, lat_field, 7);
	lat_out[7] = ns_field[0];
	lat_out[8] = '\0';

	memcpy(lon_out, lon_field, 8);
	lon_out[8] = ew_field[0];
	lon_out[9] = '\0';

	return true;
}

size_t gps_format_build_position_payload(const char *lat, const char *lon, const char *comment,
					  uint8_t *out_buf, size_t out_buf_size)
{
	/* '!' + "DDMM.hhN" (8) + '/' + "DDDMM.hhE" (9) + symbol code (1) */
	const size_t fixed_len = 1 + 8 + 1 + 9 + 1;

	if (out_buf_size < fixed_len) {
		return 0;
	}

	size_t off = 0;

	/* '!' = APRS position report, no messaging capability - this is an
	 * unattended standalone tracker with no return path, so it can never
	 * receive or ack a message (APRS101.pdf ch.5, '!' vs '=').
	 */
	out_buf[off++] = '!';

	memcpy(&out_buf[off], lat, 8);
	off += 8;

	out_buf[off++] = '/'; /* primary symbol table */

	memcpy(&out_buf[off], lon, 9);
	off += 9;

	out_buf[off++] = '>'; /* symbol code: car (default tracker icon) */

	size_t comment_len = comment != NULL ? strlen(comment) : 0;

	if (comment_len > out_buf_size - off) {
		comment_len = out_buf_size - off;
	}
	memcpy(&out_buf[off], comment, comment_len);
	off += comment_len;

	return off;
}

void gps_format_encode_position(float lat, float lon, char lat_out[9], char lon_out[10])
{
	char lat_hemi = lat >= 0 ? 'N' : 'S';
	char lon_hemi = lon >= 0 ? 'E' : 'W';
	float lat_abs = lat < 0 ? -lat : lat;
	float lon_abs = lon < 0 ? -lon : lon;
	int lat_deg = (int)lat_abs;
	int lon_deg = (int)lon_abs;
	float lat_min = (lat_abs - lat_deg) * 60.0f;
	float lon_min = (lon_abs - lon_deg) * 60.0f;

	snprintf(lat_out, 9, "%02d%05.2f%c", lat_deg, (double)lat_min, lat_hemi);
	snprintf(lon_out, 10, "%03d%05.2f%c", lon_deg, (double)lon_min, lon_hemi);
}
