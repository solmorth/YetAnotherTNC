#ifndef GPS_FORMAT_H_
#define GPS_FORMAT_H_

/*
 * Zephyr-free NMEA-to-APRS position formatting, split out of gps.c so it can
 * be exercised by the host test suite (gps.c itself pulls in UART/GPIO/ring
 * buffer APIs that only exist in the Zephyr build).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Extracts the lat/N-S/lon/E-W fields out of a $GPGGA/$GNGGA sentence and
 * writes them out already in APRS's fixed-width uncompressed position
 * format - NMEA already encodes lat/lon as ddmm.mmmm (degrees + decimal
 * minutes), the same scheme APRS uses, just with more decimal digits than
 * APRS's fixed two, so this only needs to extract and truncate, not convert.
 *
 * lat_out must be at least 9 bytes ("DDMM.hhN" + '\0'), lon_out at least
 * 10 bytes ("DDDMM.hhE" + '\0').
 *
 * Returns false (leaving lat_out/lon_out untouched) if line isn't a
 * GGA sentence, or its lat/lon fields aren't populated yet (e.g. no fix).
 */
bool gps_format_parse_gga_position(const char *line, char lat_out[9], char lon_out[10]);

/* Builds an uncompressed APRS position report payload - "!" + lat + '/' +
 * lon + symbol code + comment (APRS101.pdf ch.5) - into out_buf. lat/lon
 * are expected in the format gps_format_parse_gga_position produces.
 * comment may be NULL (treated as empty). Returns the number of bytes
 * written, or 0 if out_buf_size is too small to hold even the position
 * fields (comment is silently truncated to fit instead of failing).
 */
size_t gps_format_build_position_payload(const char *lat, const char *lon, const char *comment,
					  uint8_t *out_buf, size_t out_buf_size);

/* Converts a decimal-degrees coordinate (e.g. from a manually-configured
 * fixed position) into the same APRS DDMM.hh fixed-width format
 * gps_format_parse_gga_position produces from NMEA, so both feed
 * gps_format_build_position_payload identically.
 *
 * lat_out must be at least 9 bytes ("DDMM.hhN" + '\0'), lon_out at least
 * 10 bytes ("DDDMM.hhE" + '\0').
 */
void gps_format_encode_position(float lat, float lon, char lat_out[9], char lon_out[10]);

#endif /* GPS_FORMAT_H_ */
