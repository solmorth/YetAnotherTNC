#ifndef TNC_CONFIG_H_
#define TNC_CONFIG_H_

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

/* Parses a single-line ASCII command ("SET CALL=N0CALL-9",
 * "SET BEACON=120", "SET COMMENT=...", "GET") received over KISS cmd
 * KISS_CMD_SETHARDWARE,
 * applies it (persisting to flash on success), and writes a reply string
 * into resp. Always returns 0; errors are reported in resp, not the
 * return value.
 */
int tnc_config_handle_command(const uint8_t *data, size_t len, char *resp, size_t resp_len);

#ifdef __cplusplus
}
#endif

#endif /* TNC_CONFIG_H_ */
