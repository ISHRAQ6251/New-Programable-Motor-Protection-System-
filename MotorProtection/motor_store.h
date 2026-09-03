#pragma once

#include <stddef.h>
#include "types.h"

void motorStoreBegin();
void motorStoreLoad();
bool motorStoreSave();
void motorStoreGet(MotorRecord *out);
void motorStoreSet(const MotorRecord *in);
int motorStoreFreeCount();
bool motorStoreAlloc(uint8_t phase_count, uint8_t *out_ch);
void motorStoreAuthGet(char *user, char *pass);
bool motorStoreAuthSet(const char *user, const char *pass);
int motorStoreAdd(const MotorRecord *in, char *err, size_t err_len);
bool motorStoreEdit(int idx, const MotorRecord *in, char *err, size_t err_len);
bool motorStoreDelete(int idx, MotorStatus status, char *err, size_t err_len);
