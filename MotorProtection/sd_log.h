#pragma once

#include <stddef.h>
#include "types.h"

void sdLogBegin();
bool sdLogOk();
void sdLogAppend(const LogEvent *ev);
bool sdLogClear();
bool sdLogReadAll(char *buf, size_t buf_len, size_t *out_len);
