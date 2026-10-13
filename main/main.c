#include "bsp/esp-bsp.h"
#include "burner.h"
#include "ui.h"
#include "power_service.h"
#include "storage.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "nvs_flash.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));
    ESP_LOGI("tab5", "Host flash: %lu MB; PSRAM: %u MB", (unsigned long)(flash_size / 1048576),
        (unsigned)(esp_psram_get_size() / 1048576));
    bsp_display_cfg_t config = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = true,
        .flags = { .buff_dma = false, .buff_spiram = true, .sw_rotate = true },
    };
    lv_display_t *display = bsp_display_start_with_config(&config);
    if (!display) { ESP_LOGE("tab5", "Display/touch initialisation failed"); return; }
    esp_err_t usb_error = burner_init();
    if (usb_error != ESP_OK) ESP_LOGE("tab5", "USB host: %s", esp_err_to_name(usb_error));
    esp_err_t power_error = power_service_init();
    if (power_error != ESP_OK) ESP_LOGE("tab5", "Power service: %s", esp_err_to_name(power_error));
    esp_err_t storage_error = storage_init();
    if (storage_error != ESP_OK) ESP_LOGE("tab5", "Storage service: %s", esp_err_to_name(storage_error));
    storage_status_t storage;
    storage_get_status(&storage);
    if (bsp_display_lock(0)) {
        bsp_display_rotate(display, LV_DISPLAY_ROTATION_90);
        ui_init(storage.mode == STORAGE_LOCAL, usb_error == ESP_OK);
        bsp_display_unlock();
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(70));
}
