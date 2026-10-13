#include "power_service.h"
#include "battery_math.h"
#include "burner.h"
#include "storage.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define CHARGE_EN (1U << 7)
#define QUICK_CHARGE_N (1U << 5)
#define CHARGE_STATUS (1U << 6)
typedef struct { power_action_t action; unsigned value; } power_command_t;
static power_status_t status;
static power_status_t published;
static SemaphoreHandle_t lock, cache_lock;
static QueueHandle_t commands;
static esp_io_expander_handle_t expander;
static i2c_master_dev_handle_t monitor;

static void publish(void)
{
    xSemaphoreTake(cache_lock, portMAX_DELAY);
    published = status;
    xSemaphoreGive(cache_lock);
}

static esp_err_t read_register(uint8_t reg, uint16_t *value)
{
    uint8_t data[2];
    esp_err_t err = i2c_master_transmit_receive(monitor, &reg, 1, data, sizeof(data), 50);
    if (err == ESP_OK) *value = (uint16_t)data[0] << 8 | data[1];
    return err;
}

static esp_err_t quick_charge(bool enabled)
{
    if (!status.controls_valid) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_io_expander_set_level(expander, QUICK_CHARGE_N, !enabled);
    if (err == ESP_OK) status.quick_charge_enabled = enabled;
    return err;
}

static void sample(void)
{
    uint16_t bus, shunt;
    status.telemetry_valid = monitor && read_register(2, &bus) == ESP_OK && read_register(1, &shunt) == ESP_OK;
    if (status.telemetry_valid) {
        status.voltage = battery_bus_voltage(bus);
        status.current = battery_shunt_current(shunt);
        status.voltage_percent = battery_voltage_percent(status.voltage);
    } else status.voltage_percent = -1;
    uint32_t pins;
    status.charger_status_valid = expander && esp_io_expander_get_level(expander, CHARGE_STATUS, &pins) == ESP_OK;
    if (status.charger_status_valid) status.charging = (pins & CHARGE_STATUS) != 0;
}

static void apply(const power_command_t *command)
{
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (command->action == POWER_BRIGHTNESS && command->value >= 10 && command->value <= 100) {
        err = bsp_display_brightness_set(command->value);
        if (err == ESP_OK) status.brightness = command->value;
    } else if (command->action == POWER_USB_A) {
        err = burner_set_usb_power(command->value != 0);
        if (err == ESP_OK) status.usb_power_enabled = command->value != 0;
    } else if (command->action == POWER_CHARGING && status.controls_valid) {
        err = esp_io_expander_set_level(expander, CHARGE_EN, command->value != 0);
        if (err == ESP_OK) status.charge_enabled = command->value != 0;
    } else if (command->action == POWER_QUICK_CHARGE) {
        // QC negotiation also uses USB-C data lines. Keep it off in USB drive mode.
        storage_status_t storage;
        storage_get_status(&storage);
        err = (storage.mode == STORAGE_USB || storage.mode == STORAGE_SWITCHING || storage.usb_active || burner_is_active()) ?
            ESP_ERR_INVALID_STATE : quick_charge(command->value != 0);
    }
    snprintf(status.message, sizeof(status.message), "%s", err == ESP_OK ? "Power setting applied." :
        err == ESP_ERR_INVALID_STATE ? "Setting is locked during flashing or USB file transfer." : "Power control failed; check hardware.");
}

static void worker(void *arg)
{
    (void)arg;
    TickType_t sampled = 0;
    for (;;) {
        power_command_t command;
        bool received = xQueueReceive(commands, &command, pdMS_TO_TICKS(250)) == pdTRUE;
        xSemaphoreTake(lock, portMAX_DELAY);
        if (received) apply(&command);
        TickType_t now = xTaskGetTickCount();
        if (now - sampled >= pdMS_TO_TICKS(1000)) { sample(); sampled = now; }
        publish();
        xSemaphoreGive(lock);
    }
}

esp_err_t power_service_init(void)
{
    lock = xSemaphoreCreateMutex();
    cache_lock = xSemaphoreCreateMutex();
    commands = xQueueCreate(8, sizeof(power_command_t));
    if (!lock || !cache_lock || !commands) return ESP_ERR_NO_MEM;
    status.brightness = 70;
    status.voltage_percent = -1;
    status.usb_power_enabled = burner_usb_power_enabled();
    expander = bsp_io_expander1_init();
    if (expander && esp_io_expander_set_dir(expander, CHARGE_EN | QUICK_CHARGE_N, IO_EXPANDER_OUTPUT) == ESP_OK &&
        esp_io_expander_set_level(expander, CHARGE_EN | QUICK_CHARGE_N, true) == ESP_OK &&
        esp_io_expander_set_output_mode(expander, CHARGE_EN | QUICK_CHARGE_N, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL) == ESP_OK &&
        esp_io_expander_set_dir(expander, CHARGE_STATUS, IO_EXPANDER_INPUT) == ESP_OK &&
        esp_io_expander_set_pullupdown(expander, CHARGE_STATUS, IO_EXPANDER_PULL_DOWN) == ESP_OK) {
        status.controls_valid = true;
        status.charge_enabled = true;
    }
    i2c_device_config_t config = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x41, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &monitor) == ESP_OK) {
        uint16_t id;
        uint8_t setup[] = { 0, 0x45, 0x27 }; // 16 averages, 1.1 ms bus/shunt conversions, continuous.
        if (read_register(0xff, &id) != ESP_OK || id != 0x2260 ||
            i2c_master_transmit(monitor, setup, sizeof(setup), 50) != ESP_OK) {
            i2c_master_bus_rm_device(monitor);
            monitor = NULL;
        }
    }
    sample();
    snprintf(status.message, sizeof(status.message), "Charging %s. Battery percentage is a voltage estimate.",
        status.controls_valid ? "enabled" : "controls unavailable");
    status.available = true;
    publish();
    if (xTaskCreate(worker, "tab5_power", 4096, NULL, 3, NULL) != pdTRUE) {
        status.available = false;
        status.controls_valid = false;
        snprintf(status.message, sizeof(status.message), "Power worker could not start.");
        publish();
        vQueueDelete(commands);
        commands = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void power_get_status(power_status_t *out)
{
    if (!cache_lock) { memset(out, 0, sizeof(*out)); out->voltage_percent = -1; return; }
    xSemaphoreTake(cache_lock, portMAX_DELAY);
    *out = published;
    xSemaphoreGive(cache_lock);
}

bool power_request(power_action_t action, unsigned value)
{
    power_command_t command = { action, value };
    return commands && xQueueSend(commands, &command, 0) == pdTRUE;
}

esp_err_t power_pause_quick_charge(bool *previous)
{
    if (!lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock, portMAX_DELAY);
    *previous = status.quick_charge_enabled;
    esp_err_t err = quick_charge(false);
    publish();
    xSemaphoreGive(lock);
    return err;
}

void power_restore_quick_charge(bool previous)
{
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (quick_charge(previous) != ESP_OK) snprintf(status.message, sizeof(status.message), "Quick charge restore failed.");
    publish();
    xSemaphoreGive(lock);
}
