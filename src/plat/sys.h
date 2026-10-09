/* System integration: lid/headphones, backlights, battery, Wi-Fi, APT hooks. */
#ifndef SHELF3DS_SYS_H
#define SHELF3DS_SYS_H
#include <stdbool.h>

void sys_init(void (*on_suspend)(void));
void sys_exit(void);
void sys_poll(void);           /* call every frame; cheap */
bool sys_lid_closed(void);
bool sys_headphones(void);
int sys_battery(void);         /* 0..5 */
bool sys_charging(void);
int sys_wifi(void);            /* 0..3 bars, 0 when disconnected */
bool sys_wifi_connected(void);
void sys_backlight(bool on);
bool sys_backlight_on(void);
void sys_allow_sleep(bool allow);
bool sys_suspended(void);      /* between the APT suspend/sleep and restore/wakeup hooks */
bool sys_is_new3ds(void);
const char *sys_model(void);   /* "New 2DS XL", "3DS XL", ... */
unsigned long long sys_now_ms(void); /* unix epoch ms */

#endif
