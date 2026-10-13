#pragma once
#include <stdatomic.h>
#include "esp_loader.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "usb/cdc_acm_host.h"

typedef struct {
    esp_loader_port_t base;
    cdc_acm_dev_hdl_t device;
    StreamBufferHandle_t rx;
    atomic_bool disconnected;
    atomic_bool failed;
    int64_t deadline_us;
} tab5_usb_port_t;

extern const esp_loader_port_ops_t tab5_usb_ops;
