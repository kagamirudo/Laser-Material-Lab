#ifndef DISPLAY_H
#define DISPLAY_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_init(void);
void display_clear(void);
void display_show_status(const char *line1, const char *line2);

#ifdef __cplusplus
}
#endif

#endif  // DISPLAY_H
