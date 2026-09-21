#pragma once

#include "types.h"

void protectionBegin();
void protectionTask(void *arg);
bool protectionPost(CmdType type, uint8_t motor_idx);
void protectionSnapshot(StatusSnapshot *out);
bool protectionPopLog(LogEvent *out);
int protectionCopyLog(LogEvent *out, int max);
bool protectionCanStart(const StatusSnapshot &snap, int idx, const char **reason);
ToneId protectionTakeTone();
bool protectionAllIdleForCal();
MotorStatus protectionMotorStatus(int idx);
void protectionSetSdOk(uint8_t ok);
void protectionCopyVcal(uint8_t out[MAX_CHANNELS]);
