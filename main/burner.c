#include "burner.h"
#include "usb_loader_port.h"
#include "storage.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static QueueHandle_t requests;
static SemaphoreHandle_t status_lock;
static burner_status_t status;
static atomic_bool cancel_requested;
static atomic_bool job_active;
static bool usb_power_enabled;
static fw_plan_t active_plan;
static tab5_usb_port_t port;
static esp_loader_t loader;

static void usb_events(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (err == ESP_OK && (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)) usb_host_device_free_all();
    }
}

static void report(burner_state_t state, const char *message)
{
    xSemaphoreTake(status_lock, portMAX_DELAY);
    status.state = state;
    snprintf(status.message, sizeof(status.message), "%s", message);
    xSemaphoreGive(status_lock);
    ESP_LOGI("burner", "%s", message);
}

static bool loader_ok(esp_loader_error_t err, const char *action)
{
    if (err == ESP_LOADER_SUCCESS) return true;
    char message[192];
    snprintf(message, sizeof(message), "%s failed (loader %d). %s", action, err,
        atomic_load(&port.disconnected) ? "USB disconnected; reconnect and retry the complete plan." :
        err == ESP_LOADER_ERROR_INVALID_MD5 ? "Checksum mismatch; retry the complete plan." :
        "Check USB cable and client download mode; retry the complete plan.");
    report(BURNER_ERROR, message);
    return false;
}

static bool cancelled(void)
{
    if (!atomic_load(&cancel_requested)) return false;
    report(BURNER_CANCELLED, "Cancelled. Client may be partially written; retry the complete plan before booting.");
    return true;
}

static bool flash_image(const fw_image_t *image)
{
    FILE *file = fopen(image->path, "rb");
    if (!file) { report(BURNER_ERROR, "Cannot open BIN on SD card."); return false; }
    uint8_t block[1024]; // Small fixed buffer; never load a complete BIN into RAM.
    esp_loader_flash_cfg_t config = {
        .offset = image->offset,
        .image_size = (image->size + 3U) & ~3U,
        .block_size = sizeof(block),
        .skip_verify = false,
    };
    char message[192];
    snprintf(message, sizeof(message), "Erasing / writing %.100s at 0x%08lx", fw_basename(image->path), (unsigned long)image->offset);
    report(BURNER_BUSY, message);
    bool ok = !cancelled() && loader_ok(esp_loader_flash_start(&loader, &config), "Erase");
    uint32_t remaining = image->size;
    while (ok && remaining) {
        if (cancelled()) { ok = false; break; }
        size_t chunk = remaining > sizeof(block) ? sizeof(block) : remaining;
        memset(block, 0xff, sizeof(block));
        if (fread(block, 1, chunk, file) != chunk) {
            report(BURNER_ERROR, "SD read failed. Client may be partially written; remount SD and retry.");
            ok = false;
            break;
        }
        uint32_t padded = ((uint32_t)chunk + 3U) & ~3U;
        ok = loader_ok(esp_loader_flash_write(&loader, &config, block, padded), "Write");
        if (!ok) break;
        remaining -= chunk;
        xSemaphoreTake(status_lock, portMAX_DELAY);
        status.written += chunk;
        xSemaphoreGive(status_lock);
        vTaskDelay(1);
    }
    fclose(file);
    if (ok && !cancelled()) {
        snprintf(message, sizeof(message), "Verifying %.110s", fw_basename(image->path));
        report(BURNER_BUSY, message);
        ok = loader_ok(esp_loader_flash_finish(&loader, &config), "MD5 verification");
    } else ok = false;
    return ok;
}

static void run_plan(void)
{
    char error[192];
    if (!fw_validate(&active_plan, FW_TAB5_FLASH_SIZE, error, sizeof(error)) ||
        !fw_preflight(&active_plan, error, sizeof(error))) { report(BURNER_ERROR, error); return; }
    if (cancelled()) return;
    memset(&port, 0, sizeof(port));
    port.base.ops = &tab5_usb_ops;
    report(BURNER_BUSY, "Opening client USB-C programming interface (host USB-A)...");
    if (!loader_ok(esp_loader_init_serial(&loader, &port.base), "USB connection")) return;
    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    args.sync_timeout = 300;
    report(BURNER_BUSY, "Entering client download mode and detecting ESP32-P4...");
    bool ok = !cancelled() && loader_ok(esp_loader_connect(&loader, &args), "Bootloader connection");
    if (ok && esp_loader_get_target(&loader) != ESP32P4_CHIP) {
        report(BURNER_ERROR, "Client is not an ESP32-P4. No flash was erased.");
        ok = false;
    }
    esp_loader_target_security_info_t security = {0};
    if (ok) ok = loader_ok(esp_loader_get_security_info(&loader, &security), "Security check");
    if (ok && (security.secure_boot_enabled || security.flash_encryption_enabled || security.secure_download_mode_enabled)) {
        report(BURNER_ERROR, "Secured client requires its own provisioning workflow. No flash was erased.");
        ok = false;
    }
    uint32_t size = 0;
    if (ok) ok = loader_ok(esp_loader_flash_detect_size(&loader, &size), "Flash detection");
    if (ok) {
        xSemaphoreTake(status_lock, portMAX_DELAY);
        status.flash_size = size;
        xSemaphoreGive(status_lock);
        if (!fw_validate(&active_plan, size, error, sizeof(error))) { report(BURNER_ERROR, error); ok = false; }
    }
    for (size_t i = 0; ok && i < active_plan.count; ++i)
        if (active_plan.images[i].selected) ok = flash_image(&active_plan.images[i]);
    if (ok && !cancelled()) {
        esp_loader_reset_target(&loader);
        report(BURNER_DONE, "All selected images written and MD5 verified. Client reset requested.");
    }
    esp_loader_deinit(&loader);
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t request;
        xQueueReceive(requests, &request, portMAX_DELAY);
        run_plan();
        storage_release_local();
        atomic_store(&job_active, false);
    }
}

esp_err_t burner_init(void)
{
    status_lock = xSemaphoreCreateMutex();
    requests = xQueueCreate(1, sizeof(uint8_t));
    if (!status_lock || !requests) return ESP_ERR_NO_MEM;
    esp_err_t err = bsp_feature_enable(BSP_FEATURE_USB, true);
    if (err != ESP_OK) return err;
    usb_power_enabled = true;
    // M5Stack schematic: USB_HOST_DP/DM (USB-A) use the dedicated HS PHY.
    // Peripheral 0 is HS; USB-C programming uses Serial/JTAG on GPIO24/25.
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .peripheral_map = 1U << 0,
    };
    if ((err = usb_host_install(&host_config)) != ESP_OK) {
        bsp_feature_enable(BSP_FEATURE_USB, false);
        usb_power_enabled = false;
        return err;
    }
    if (xTaskCreate(usb_events, "tab5_usb_events", 4096, NULL, 10, NULL) != pdTRUE) return ESP_ERR_NO_MEM;
    if ((err = cdc_acm_host_install(NULL)) != ESP_OK) return err;
    if (xTaskCreate(worker, "tab5_flasher", 8192, NULL, 5, NULL) != pdTRUE) return ESP_ERR_NO_MEM;
    report(BURNER_IDLE, "Connect host USB-A to client USB-C with a data cable.");
    return ESP_OK;
}

bool burner_start(const fw_plan_t *plan)
{
    if (!requests || !status_lock) return false;
    if (!storage_claim_local()) return false;
    xSemaphoreTake(status_lock, portMAX_DELAY);
    if (atomic_load(&job_active) || !usb_power_enabled) {
        xSemaphoreGive(status_lock);
        storage_release_local();
        return false;
    }
    atomic_store(&job_active, true);
    active_plan = *plan; // Immutable worker snapshot; UI edits cannot affect this job.
    status.state = BURNER_BUSY;
    status.written = 0;
    status.total = fw_total_bytes(plan);
    status.flash_size = 0;
    atomic_store(&cancel_requested, false);
    snprintf(status.message, sizeof(status.message), "Checking all selected SD card files...");
    xSemaphoreGive(status_lock);
    uint8_t request = 1;
    if (xQueueSend(requests, &request, 0) != pdTRUE) {
        atomic_store(&job_active, false);
        storage_release_local();
        report(BURNER_ERROR, "Worker queue unavailable.");
        return false;
    }
    return true;
}

void burner_cancel(void) { atomic_store(&cancel_requested, true); }

bool burner_is_active(void) { return atomic_load(&job_active); }
bool burner_usb_power_enabled(void)
{
    if (!status_lock) return false;
    xSemaphoreTake(status_lock, portMAX_DELAY);
    bool enabled = usb_power_enabled;
    xSemaphoreGive(status_lock);
    return enabled;
}
esp_err_t burner_set_usb_power(bool enabled)
{
    if (!status_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(status_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!atomic_load(&job_active)) {
        err = bsp_feature_enable(BSP_FEATURE_USB, enabled);
        if (err == ESP_OK) usb_power_enabled = enabled;
    }
    xSemaphoreGive(status_lock);
    return err;
}

void burner_get_status(burner_status_t *out)
{
    if (!status_lock) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(status_lock, portMAX_DELAY);
    *out = status;
    xSemaphoreGive(status_lock);
}
