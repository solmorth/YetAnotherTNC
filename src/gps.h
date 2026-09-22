#ifndef GPS_H_
#define GPS_H_

#include <zephyr/kernel.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int gps_init(void);
void gps_enable_set(bool enable);
bool gps_is_enabled(void);

/* Starts the periodic beacon loop using tnc_config's fixed position instead
 * of a live GPS fix - call instead of gps_init()/gps_enable_set() when
 * tnc_config_get_fixed_pos_enabled() is true, so no GPS hardware is touched.
 */
void gps_start_fixed_position_beacon(void);

#ifdef __cplusplus
}
#endif

#endif /* GPS_H_ */
