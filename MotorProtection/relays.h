#pragma once

#include <stdint.h>
#include <stdbool.h>

void relaysBegin();
void relaysLogPolarity(const uint8_t *active_high_by_ch);
void relaysDeenergizeAll(const uint8_t *active_high_by_ch);
void relaysSetChannel(int ch, bool energized, uint8_t active_high);
void relaysMotorOff(const uint8_t *channels, const uint8_t *active_high, uint8_t phase_count);
void relaysMotorOn(const uint8_t *channels, const uint8_t *active_high, uint8_t phase_count);
