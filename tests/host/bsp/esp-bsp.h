#pragma once
#include "esp_err.h"
#include "lvgl.h"
#define BSP_SD_MOUNT_POINT "/sdcard"
esp_err_t bsp_sdcard_mount(void);
esp_err_t bsp_sdcard_unmount(void);
