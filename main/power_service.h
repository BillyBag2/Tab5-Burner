#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool available;
    bool telemetry_valid;
    bool controls_valid;
    bool charger_status_valid;
    bool charging;
    bool charge_enabled;
    bool quick_charge_enabled;
    bool usb_power_enabled;
    float voltage;
    float current; // Amperes, positive discharging, negative charging.
    int voltage_percent; // Estimate only; -1 means unavailable.
    unsigned brightness;
    char message[160];
} power_status_t;
typedef enum { POWER_CHARGING, POWER_QUICK_CHARGE, POWER_USB_A, POWER_BRIGHTNESS } power_action_t;

esp_err_t power_service_init(void);
void power_get_status(power_status_t *out);
bool power_request(power_action_t action, unsigned value);
esp_err_t power_pause_quick_charge(bool *previous);
void power_restore_quick_charge(bool previous);
