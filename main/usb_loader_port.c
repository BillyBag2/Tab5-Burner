#include "usb_loader_port.h"
#include "esp_timer.h"
#include "freertos/task.h"

static tab5_usb_port_t *self(esp_loader_port_t *base)
{
    return (tab5_usb_port_t *)base; // Embedded base is the first member.
}

static bool data_cb(const uint8_t *data, size_t length, void *arg)
{
    tab5_usb_port_t *port = arg;
    if (xStreamBufferSend(port->rx, data, length, 0) != length) atomic_store(&port->failed, true);
    return true;
}

static void event_cb(const cdc_acm_host_dev_event_data_t *event, void *arg)
{
    tab5_usb_port_t *port = arg;
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) atomic_store(&port->disconnected, true);
    if (event->type == CDC_ACM_HOST_ERROR) atomic_store(&port->failed, true);
    // The worker owns teardown. Never delete a stream buffer while a read is in flight.
}

static esp_loader_error_t port_init(esp_loader_port_t *base)
{
    tab5_usb_port_t *port = self(base);
    atomic_store(&port->disconnected, false);
    atomic_store(&port->failed, false);
    port->rx = xStreamBufferCreate(8192, 1);
    if (!port->rx) return ESP_LOADER_ERROR_FAIL;
    const cdc_acm_host_device_config_t config = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 4096,
        .in_buffer_size = 512,
        .event_cb = event_cb,
        .data_cb = data_cb,
        .user_arg = port,
    };
    // Only the native Espressif programming interface; never claim arbitrary USB serial devices.
    if (cdc_acm_host_open(0x303a, 0x1001, 0, &config, &port->device) != ESP_OK) {
        vStreamBufferDelete(port->rx);
        port->rx = NULL;
        return ESP_LOADER_ERROR_TIMEOUT;
    }
    return ESP_LOADER_SUCCESS;
}

static void port_deinit(esp_loader_port_t *base)
{
    tab5_usb_port_t *port = self(base);
    if (port->device) {
        cdc_acm_host_close(port->device); // Stops transfers/callbacks before freeing RX storage.
        port->device = NULL;
    }
    if (port->rx) { vStreamBufferDelete(port->rx); port->rx = NULL; }
}

static bool available(tab5_usb_port_t *port)
{
    return port->device && !atomic_load(&port->disconnected) && !atomic_load(&port->failed);
}

static void delay_ms(esp_loader_port_t *base, uint32_t ms)
{
    (void)base;
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}

static void control(tab5_usb_port_t *port, bool dtr, bool rts)
{
    if (available(port) && cdc_acm_host_set_control_line_state(port->device, dtr, rts) != ESP_OK)
        atomic_store(&port->failed, true);
}

static void reset(esp_loader_port_t *base)
{
    tab5_usb_port_t *port = self(base);
    if (!available(port)) return;
    xStreamBufferReset(port->rx);
    control(port, false, true);
    delay_ms(base, 100);
    control(port, false, false);
}

static void enter_bootloader(esp_loader_port_t *base)
{
    tab5_usb_port_t *port = self(base);
    control(port, true, false);
    delay_ms(base, 50);
    control(port, true, true);
    reset(base);
}

static void start_timer(esp_loader_port_t *base, uint32_t ms)
{
    self(base)->deadline_us = esp_timer_get_time() + (int64_t)ms * 1000;
}

static uint32_t remaining(esp_loader_port_t *base)
{
    int64_t us = self(base)->deadline_us - esp_timer_get_time();
    return us > 0 ? (uint32_t)((us + 999) / 1000) : 0;
}

static esp_loader_error_t write_bytes(esp_loader_port_t *base, const uint8_t *data, uint16_t size, uint32_t timeout)
{
    tab5_usb_port_t *port = self(base);
    if (!available(port)) return ESP_LOADER_ERROR_FAIL;
    esp_err_t err = cdc_acm_host_data_tx_blocking(port->device, data, size, timeout);
    return err == ESP_OK ? ESP_LOADER_SUCCESS : (err == ESP_ERR_TIMEOUT ? ESP_LOADER_ERROR_TIMEOUT : ESP_LOADER_ERROR_FAIL);
}

static esp_loader_error_t read_bytes(esp_loader_port_t *base, uint8_t *data, uint16_t size, uint32_t timeout)
{
    tab5_usb_port_t *port = self(base);
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout * 1000;
    size_t received = 0;
    while (received < size) {
        if (!available(port)) return ESP_LOADER_ERROR_FAIL;
        int64_t left = deadline - esp_timer_get_time();
        if (left <= 0) return ESP_LOADER_ERROR_TIMEOUT;
        uint32_t slice = (uint32_t)((left + 999) / 1000);
        if (slice > 20) slice = 20;
        received += xStreamBufferReceive(port->rx, data + received, size - received, pdMS_TO_TICKS(slice) + 1);
    }
    return ESP_LOADER_SUCCESS;
}

const esp_loader_port_ops_t tab5_usb_ops = {
    .init = port_init, .deinit = port_deinit,
    .enter_bootloader = enter_bootloader, .reset_target = reset,
    .start_timer = start_timer, .remaining_time = remaining, .delay_ms = delay_ms,
    .write = write_bytes, .read = read_bytes,
};
