#pragma once
#include "firmware.h"
#include "esp_err.h"

typedef enum { BURNER_IDLE, BURNER_BUSY, BURNER_DONE, BURNER_ERROR, BURNER_CANCELLED } burner_state_t;
typedef struct {
    burner_state_t state;
    uint32_t written;
    uint32_t total;
    uint32_t flash_size;
    char message[192];
} burner_status_t;

esp_err_t burner_init(void);
bool burner_start(const fw_plan_t *plan);
void burner_cancel(void);
void burner_get_status(burner_status_t *status);
bool burner_is_active(void);
bool burner_usb_power_enabled(void);
esp_err_t burner_set_usb_power(bool enabled);
