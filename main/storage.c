#include "storage.h"
#include "power_service.h"
#include "bsp/esp-bsp.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "device/usbd_pvt.h"
#include "esp_private/usb_phy.h"
#include "hal/usb_wrap_ll.h"
#include "hal/usb_serial_jtag_ll.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef enum { COMMAND_USB, COMMAND_LOCAL, COMMAND_REMOUNT } storage_command_t;
static storage_gate_t gate;
static storage_status_t status;
static SemaphoreHandle_t lock, drained;
static QueueHandle_t commands;
static sdmmc_card_t card;
static sdmmc_host_t host;
static sd_pwr_ctrl_handle_t ldo;
static bool host_started, driver_started, quick_charge_previous, quick_charge_paused;
static bool serial_pads_previous;
static tinyusb_msc_storage_handle_t medium;
static usb_phy_handle_t device_phy;
static atomic_bool mount_failed;
static atomic_bool io_failed;
static atomic_uint mount_completed, barrier_completed;
static unsigned barrier_ticket;
static bool service_ready;
static esp_err_t (*card_transaction)(int, sdmmc_command_t *);
static bool delete_idle_medium(void);

static esp_err_t monitor_transaction(int slot, sdmmc_command_t *command)
{
    esp_err_t err = card_transaction(slot, command);
    if (err != ESP_OK) atomic_store(&io_failed, true);
    return err;
}

static bool filesystem_ready(void)
{
    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (!dir) return false;
    closedir(dir);
    return true;
}

static void storage_event(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    (void)handle; (void)arg;
    if (event->id == TINYUSB_MSC_EVENT_MOUNT_FAILED || event->id == TINYUSB_MSC_EVENT_FORMAT_REQUIRED ||
        event->id == TINYUSB_MSC_EVENT_FORMAT_FAILED) atomic_store(&mount_failed, true);
    if (event->id == TINYUSB_MSC_EVENT_MOUNT_COMPLETE) {
        atomic_fetch_add(&mount_completed, 1);
        xSemaphoreTake(lock, portMAX_DELAY);
        if (gate.mode == STORAGE_USB && event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            status.media_ejected = true;
            snprintf(status.message, sizeof(status.message), "PC ejected the card. Stop USB drive to resume browsing.");
        }
        xSemaphoreGive(lock);
    }
}

static void device_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    xSemaphoreTake(lock, portMAX_DELAY);
    status.pc_connected = event->id == TINYUSB_EVENT_ATTACHED;
    if (gate.mode == STORAGE_USB && event->id == TINYUSB_EVENT_DETACHED)
        snprintf(status.message, sizeof(status.message), "PC connection lost. Stop USB drive and check copied files.");
    xSemaphoreGive(lock);
}

static bool mount_to(tinyusb_msc_mount_point_t point)
{
    tinyusb_msc_mount_point_t current;
    if (tinyusb_msc_get_storage_mount_point(medium, &current) != ESP_OK) return false;
    if (current == point) return point == TINYUSB_MSC_STORAGE_MOUNT_APP ? filesystem_ready() : !filesystem_ready();
    atomic_store(&mount_failed, false);
    unsigned completed = atomic_load(&mount_completed);
    // The component's setter does not propagate mount errors. Verify both event and VFS.
    if (tinyusb_msc_set_storage_mount_point(medium, point) != ESP_OK || atomic_load(&mount_failed) ||
        atomic_load(&mount_completed) == completed) return false;
    return point == TINYUSB_MSC_STORAGE_MOUNT_APP ? filesystem_ready() : !filesystem_ready();
}

static bool new_local_medium(void)
{
    const tinyusb_msc_storage_config_t config = {
        .medium.card = &card,
        .fat_fs = {
            .base_path = BSP_SD_MOUNT_POINT,
            .config = { .format_if_mount_failed = false, .max_files = 8, .allocation_unit_size = 16 * 1024 },
            .do_not_format = true,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    if (tinyusb_msc_new_storage_sdmmc(&config, &medium) != ESP_OK) return false;
    return mount_to(TINYUSB_MSC_STORAGE_MOUNT_APP);
}

static void close_card(void)
{
    if (host_started) { host.deinit(); host_started = false; }
    if (ldo) { sd_pwr_ctrl_del_on_chip_ldo(ldo); ldo = NULL; }
    memset(&card, 0, sizeof(card));
}

static bool open_card(void)
{
    bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &host);
    atomic_store(&io_failed, false);
    card_transaction = host.do_transaction;
    host.do_transaction = monitor_transaction;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot;
    bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &slot);
    const sd_pwr_ctrl_ldo_config_t power = { .ldo_chan_id = 4 };
    if (sd_pwr_ctrl_new_on_chip_ldo(&power, &ldo) != ESP_OK) return false;
    host.pwr_ctrl_handle = ldo;
    if (host.init() != ESP_OK) { close_card(); return false; }
    host_started = true;
    if (sdmmc_host_init_slot(host.slot, &slot) != ESP_OK || sdmmc_card_init(&host, &card) != ESP_OK) {
        close_card();
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    status.capacity_bytes = (uint64_t)card.csd.capacity * card.csd.sector_size;
    xSemaphoreGive(lock);
    return new_local_medium();
}

static void restore_phy(void)
{
    if (device_phy) { usb_del_phy(device_phy); device_phy = NULL; }
    // Return USB-C FSLS PHY 0 (GPIO24/25) to programming/console Serial/JTAG.
    usb_serial_jtag_ll_phy_select(0);
    usb_serial_jtag_ll_phy_enable_pad(serial_pads_previous);
}

static void write_barrier(void *arg)
{
    atomic_store(&barrier_completed, (unsigned)(uintptr_t)arg);
    xSemaphoreGive(drained);
}

static bool stop_device(void)
{
    if (!driver_started) return true;
    // Stop accepting PC traffic, then drain writes in the TinyUSB task before teardown.
    tud_disconnect();
    unsigned ticket = ++barrier_ticket;
    usbd_defer_func(write_barrier, (void *)(uintptr_t)ticket, false);
    TickType_t start = xTaskGetTickCount();
    while (atomic_load(&barrier_completed) < ticket) {
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(5000)) return false;
        xSemaphoreTake(drained, pdMS_TO_TICKS(20));
    }
    // Also handles an eject-triggered FAT mount failure without a second invalid unmount.
    // Deleting the medium checks the component's deferred-write count is zero.
    if (!delete_idle_medium()) return false;
    if (tinyusb_driver_uninstall() != ESP_OK) return false;
    driver_started = false;
    restore_phy();
    if (quick_charge_paused) { power_restore_quick_charge(quick_charge_previous); quick_charge_paused = false; }
    return true;
}

static bool start_device(void)
{
    if (power_pause_quick_charge(&quick_charge_previous) != ESP_OK) return false;
    quick_charge_paused = true;
    atomic_store(&io_failed, false);
    if (!mount_to(TINYUSB_MSC_STORAGE_MOUNT_USB)) goto failed;
    serial_pads_previous = usb_serial_jtag_ll_phy_is_pad_enabled();
    usb_serial_jtag_ll_phy_enable_pad(false);
    const usb_phy_config_t phy = {
        .controller = USB_PHY_CTRL_OTG, .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE, .otg_speed = USB_PHY_SPEED_FULL,
    };
    if (usb_new_phy(&phy, &device_phy) != ESP_OK) { restore_phy(); goto failed; }
    // IDF's FS default pins are GPIO26/27 (audio on Tab5). Select the USB-C pads instead.
    usb_wrap_ll_phy_select(&USB_WRAP, 0);
    gpio_set_drive_capability(GPIO_NUM_24, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(GPIO_NUM_25, GPIO_DRIVE_CAP_3);
    tinyusb_config_t config = TINYUSB_CONFIG_FULL_SPEED(device_event, NULL);
    config.phy.skip_setup = true;
    if (tinyusb_driver_install(&config) != ESP_OK) { restore_phy(); goto failed; }
    driver_started = true;
    return true;
failed:
    power_restore_quick_charge(quick_charge_previous);
    quick_charge_paused = false;
    return false;
}

static void finish(storage_mode_t mode, const char *message)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    gate.mode = mode;
    status.mode = mode;
    status.usb_active = driver_started;
    ++status.generation;
    snprintf(status.message, sizeof(status.message), "%s", message);
    xSemaphoreGive(lock);
}

static bool delete_idle_medium(void)
{
    if (!medium) return true;
    bool ready = filesystem_ready();
    bool unmounted = mount_to(TINYUSB_MSC_STORAGE_MOUNT_USB);
    if (ready && !unmounted) return false;
    // A failed FAT mount is already cleaned up by the component; USB ownership avoids a second unmount.
    if (tinyusb_msc_delete_storage(medium) != ESP_OK) return false;
    medium = NULL;
    return true;
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        storage_command_t command;
        if (xQueueReceive(commands, &command, pdMS_TO_TICKS(200)) != pdTRUE) {
            if (driver_started && atomic_load(&io_failed)) {
                xSemaphoreTake(lock, portMAX_DELAY);
                status.io_error = true;
                snprintf(status.message, sizeof(status.message), "SD I/O error during USB transfer. Eject, stop USB drive and verify the copied files.");
                xSemaphoreGive(lock);
            }
            continue;
        }
        if (command == COMMAND_USB) {
            if (start_device()) finish(STORAGE_USB, "SD card shared over USB-C. Eject on the PC before stopping USB drive.");
            else {
                bool local = medium && mount_to(TINYUSB_MSC_STORAGE_MOUNT_APP);
                finish(local ? STORAGE_LOCAL : STORAGE_ERROR, "USB drive could not start. Check SD card and power controls; retry.");
            }
        } else if (command == COMMAND_LOCAL) {
            if (!stop_device()) {
                finish(STORAGE_USB, "USB teardown not complete. Keep SD reserved; retry Stop USB drive.");
            } else {
                bool local = new_local_medium();
                xSemaphoreTake(lock, portMAX_DELAY);
                status.pc_connected = false;
                status.media_ejected = false;
                xSemaphoreGive(lock);
                finish(local ? STORAGE_LOCAL : STORAGE_ERROR, local ? (atomic_load(&io_failed) ?
                    "USB stopped after an SD I/O error. Verify copied files before flashing." :
                    "USB drive stopped. SD card remounted; copied files are ready to browse.") :
                    "USB stopped, but FAT mount failed. Check the card and tap Remount SD.");
            }
        } else {
            bool cleaned = delete_idle_medium();
            bool ready = false;
            if (cleaned) { close_card(); ready = open_card(); }
            finish(ready ? STORAGE_LOCAL : STORAGE_ERROR, ready ? "SD card mounted." : "Cannot mount SD. Insert a FAT32 card and retry.");
        }
    }
}

esp_err_t storage_init(void)
{
    lock = xSemaphoreCreateMutex();
    drained = xSemaphoreCreateBinary();
    commands = xQueueCreate(1, sizeof(storage_command_t));
    if (!lock || !drained || !commands) return ESP_ERR_NO_MEM;
    const tinyusb_msc_driver_config_t config = { .user_flags.auto_mount_off = 1, .callback = storage_event };
    esp_err_t err = tinyusb_msc_install_driver(&config);
    if (err != ESP_OK) { finish(STORAGE_ERROR, "USB storage driver initialization failed."); return err; }
    bool ready = open_card();
    finish(ready ? STORAGE_LOCAL : STORAGE_UNAVAILABLE, ready ? "SD card mounted locally." : "Insert a FAT32 SD card, then tap Remount SD.");
    service_ready = xTaskCreate(worker, "tab5_storage", 6144, NULL, 4, NULL) == pdTRUE;
    if (!service_ready) finish(STORAGE_ERROR, "Storage worker could not start.");
    return service_ready ? ESP_OK : ESP_ERR_NO_MEM;
}

void storage_get_status(storage_status_t *out)
{
    if (!lock) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = status;
    out->mode = gate.mode;
    xSemaphoreGive(lock);
}

static bool request(storage_command_t command)
{
    if (!lock || !commands || !service_ready) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    storage_mode_t previous = gate.mode;
    bool allowed = command == COMMAND_REMOUNT ? (!status.usb_active && storage_gate_remount(&gate)) :
        storage_gate_switch(&gate, command == COMMAND_USB);
    if (allowed) {
        status.io_error = false;
        snprintf(status.message, sizeof(status.message), "Switching SD card ownership; please wait...");
        status.media_ejected = false;
        if (xQueueSend(commands, &command, 0) != pdTRUE) { gate.mode = previous; allowed = false; }
    }
    xSemaphoreGive(lock);
    return allowed;
}

bool storage_request_usb(bool enabled) { return request(enabled ? COMMAND_USB : COMMAND_LOCAL); }
bool storage_request_remount(void) { return request(COMMAND_REMOUNT); }
bool storage_claim_local(void)
{
    if (!lock || !service_ready) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool claimed = storage_gate_claim(&gate);
    xSemaphoreGive(lock);
    return claimed;
}
void storage_release_local(void)
{
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    storage_gate_release(&gate);
    xSemaphoreGive(lock);
}
