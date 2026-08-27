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

#ifdef __cplusplus
}
#endif

#endif /* GPS_H_ */
