#ifndef TNC_CONFIG_H_
#define TNC_CONFIG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ax25.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads persisted config (callsign, beacon interval) from flash, or falls
 * back to defaults (N0CALL, 60s) if none was ever saved. Call once at boot.
 */
void tnc_config_init(void);

const ax25_address_t *tnc_config_get_callsign(void);
uint32_t tnc_config_get_beacon_interval_s(void);
const char *tnc_config_get_comment(void);

/* Fixed/static position, for standalone-mode beaconing without a live GPS
 * fix (e.g. indoors, or no GPS module fitted). fixed_pos_configured is
 * false until both SET LAT and SET LON have been received at least once -
 * fixed_pos_enabled alone doesn't imply valid coordinates are stored.
 */
bool tnc_config_get_fixed_pos_enabled(void);
bool tnc_config_get_fixed_pos_configured(void);
float tnc_config_get_fixed_lat(void);
float tnc_config_get_fixed_lon(void);

/* Parses a single-line ASCII command ("SET CALL=N0CALL-9",
 * "SET BEACON=120", "SET COMMENT=...", "SET FIXEDPOS=ON|OFF",
 * "SET LAT=<decimal degrees>", "SET LON=<decimal degrees>", "GET")
 * received over KISS cmd KISS_CMD_SETHARDWARE,
 * applies it (persisting to flash on success), and writes a reply string
 * into resp. Always returns 0; errors are reported in resp, not the
 * return value.
 */
int tnc_config_handle_command(const uint8_t *data, size_t len, char *resp, size_t resp_len);

#ifdef __cplusplus
}
#endif

#endif /* TNC_CONFIG_H_ */
