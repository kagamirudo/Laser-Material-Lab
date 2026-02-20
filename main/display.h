#ifndef DISPLAY_H
#define DISPLAY_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_init(void);
void display_clear(void);
void display_show_status(const char *line1, const char *line2);
void display_show_3lines(const char *line1, const char *line2, const char *line3);
/* Update only the third line (e.g. time) to minimize blink when IP/date unchanged */
void display_update_3rd_line(const char *line3);

#ifdef __cplusplus
}
#endif

#endif  // DISPLAY_H
