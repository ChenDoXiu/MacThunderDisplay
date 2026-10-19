#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
const char *td_status(void);
const char *td_start(void);
void td_stop(void);
void td_disconnect_client(void);
int td_client_connected(void);
int td_screen_permission(int request);
int td_input_permission(int request);
uint32_t td_display_id(void);
#ifdef __cplusplus
}
#endif
