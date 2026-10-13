idf_component_register(
    SRCS "main.c" "firmware.c" "burner.c" "usb_loader_port.c" "ui.c"
         "storage.c" "storage_gate.c" "power_service.c" "battery_math.c"
    INCLUDE_DIRS "."
    REQUIRES m5stack_tab5 esp-serial-flasher usb_host_cdc_acm usb cjson esp_tinyusb
             esp_timer esp_psram spi_flash nvs_flash sdmmc fatfs esp_hw_support esp_hal_usb)
