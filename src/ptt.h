#ifndef PTT_H_
#define PTT_H_

#include <zephyr/kernel.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int ptt_init(void);
void ptt_set(bool active);
bool ptt_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* PTT_H_ */
