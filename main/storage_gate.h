#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { STORAGE_UNAVAILABLE, STORAGE_LOCAL, STORAGE_SWITCHING, STORAGE_USB, STORAGE_ERROR } storage_mode_t;
// All operations on a gate must be serialized by its owner.
typedef struct { storage_mode_t mode; unsigned readers; } storage_gate_t;
bool storage_gate_claim(storage_gate_t *gate);
void storage_gate_release(storage_gate_t *gate);
bool storage_gate_switch(storage_gate_t *gate, bool to_usb);
bool storage_gate_remount(storage_gate_t *gate);
