#pragma once
#include "storage_gate.h"
#include "esp_err.h"
typedef struct {
    storage_mode_t mode;
    bool usb_active;
    bool pc_connected;
    bool media_ejected;
    bool io_error;
    uint64_t capacity_bytes;
    unsigned generation;
    char message[192];
} storage_status_t;

esp_err_t storage_init(void);
void storage_get_status(storage_status_t *status);
bool storage_request_usb(bool enabled);
bool storage_request_remount(void);
// A lease blocks USB handover/remount until released. Used for browsing and flashing.
bool storage_claim_local(void);
void storage_release_local(void);
