#ifndef BUZZER_H
#define BUZZER_H

#include <stdint.h>

void buzzer_init(void);

void update_buzzer(uint16_t freq_hz,
                   uint8_t volume_percent);

void buzzer_off(void);

void buzzer_beep(uint16_t freq_hz,
                 uint8_t volume_percent,
                 uint32_t duration_ms);

void buzzer_alert(void);

void buzzer_error(void);

void buzzer_success(void);

#endif