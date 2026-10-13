// Render production ui.c with real LVGL; only hardware and worker are substituted.
#include <assert.h>
#include <emscripten.h>
#include <sys/stat.h>
#include "../../main/ui.c"

static burner_status_t mock_status;
static storage_gate_t mock_gate = { .mode = STORAGE_LOCAL };
static storage_status_t mock_storage = { .mode = STORAGE_LOCAL, .capacity_bytes = 32ULL * 1024 * 1024 * 1024,
    .message = "SD card mounted locally." };
static power_status_t mock_power = { .available = true, .telemetry_valid = true, .controls_valid = true, .charger_status_valid = true,
    .charge_enabled = true, .usb_power_enabled = true, .voltage = 7.4f, .current = 0.35f, .voltage_percent = 58,
    .brightness = 70, .message = "Charging enabled. Battery percentage is a voltage estimate." };
static uint8_t pixels[1280 * 720 * 3];
static uint8_t draw_buffer[1280 * 720 * 3];
static lv_display_t *display;

esp_err_t bsp_sdcard_mount(void) { return ESP_OK; }
esp_err_t bsp_sdcard_unmount(void) { return ESP_OK; }
void burner_get_status(burner_status_t *out) { *out = mock_status; }
bool burner_start(const fw_plan_t *p) {
    if (!mock_power.usb_power_enabled || !storage_gate_claim(&mock_gate)) return false;
    mock_status.state = BURNER_BUSY; mock_status.total = fw_total_bytes(p); return true;
}
void burner_cancel(void) { mock_status.state = BURNER_CANCELLED; }
bool burner_is_active(void) { return mock_status.state == BURNER_BUSY; }
bool burner_usb_power_enabled(void) { return mock_power.usb_power_enabled; }
void storage_get_status(storage_status_t *out) { *out = mock_storage; out->mode = mock_gate.mode; }
bool storage_claim_local(void) { return storage_gate_claim(&mock_gate); }
void storage_release_local(void) { storage_gate_release(&mock_gate); }
bool storage_request_usb(bool enabled) { return storage_gate_switch(&mock_gate, enabled); }
bool storage_request_remount(void) { return storage_gate_remount(&mock_gate); }
void power_get_status(power_status_t *out) { *out = mock_power; }
bool power_request(power_action_t action, unsigned value) {
    if (action == POWER_CHARGING) mock_power.charge_enabled = value != 0;
    else if (action == POWER_BRIGHTNESS) mock_power.brightness = value;
    else if (action == POWER_USB_A && !burner_is_active()) mock_power.usb_power_enabled = value != 0;
    else if (action == POWER_QUICK_CHARGE && mock_gate.mode == STORAGE_LOCAL) mock_power.quick_charge_enabled = value != 0;
    else return false;
    snprintf(mock_power.message, sizeof(mock_power.message), "Power setting applied.");
    return true;
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *data)
{
    int w = lv_area_get_width(area);
    for (int y = area->y1; y <= area->y2; ++y) {
        memcpy(pixels + (y * 1280 + area->x1) * 3, data, w * 3);
        data += w * 3;
    }
    lv_display_flush_ready(disp);
}

static void screenshot(const char *name)
{
    lv_tick_inc(250);
    lv_timer_handler(); // Let state transitions and the production poll timer settle.
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(display);
    // RGB888 is stored in BGR byte order by LVGL.
    EM_ASM({
        const fs = require('node:fs');
        fs.writeFileSync(UTF8ToString($0) + '.bgr', HEAPU8.slice($1, $1 + 1280 * 720 * 3));
    }, name, pixels);
}

static void assert_inside(lv_obj_t *child, lv_obj_t *parent)
{
    lv_area_t a, b;
    lv_obj_get_coords(child, &a);
    lv_obj_get_coords(parent, &b);
    if (!(a.x1 >= b.x1 && a.x2 <= b.x2 && a.y1 >= b.y1 && a.y2 <= b.y2)) {
        printf("OUTSIDE child (%ld,%ld)-(%ld,%ld), parent (%ld,%ld)-(%ld,%ld)\n",
            (long)a.x1, (long)a.y1, (long)a.x2, (long)a.y2, (long)b.x1, (long)b.y1, (long)b.x2, (long)b.y2);
        assert(false);
    }
}

static void check_actions(void)
{
    lv_obj_update_layout(lv_screen_active());
    assert_inside(flash_button, lv_obj_get_parent(flash_button));
    assert_inside(cancel_button, lv_obj_get_parent(cancel_button));
    assert_inside(lv_obj_get_parent(flash_button), lv_screen_active());
    assert_inside(browser_list, lv_obj_get_parent(browser_list));
    assert_inside(image_list, lv_obj_get_parent(image_list));
    assert_inside(validation, lv_obj_get_parent(validation));
}

int main(void)
{
    lv_init();
    display = lv_display_create(1280, 720);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    mkdir("/sdcard", 0777);
    mkdir("/sdcard/firmware", 0777);
    mkdir("/sdcard/firmware/demo", 0777);
    FILE *f = fopen("/sdcard/merged-tab5.bin", "wb");
    assert(f); fputs("demo", f); fclose(f);
    ui_init(true, true);
    screenshot("initial");
    check_actions();
    snprintf(directory, sizeof(directory), "/sdcard/firmware/demo");
    plan.count = 3;
    const char *names[] = { "bootloader.bin", "partition-table.bin", "tab5-application.bin" };
    const uint32_t offsets[] = { 0x2000, 0x8000, 0x10000 };
    const uint32_t sizes[] = { 24576, 3072, 1835008 };
    for (int i = 0; i < 3; ++i) {
        snprintf(plan.images[i].path, sizeof(plan.images[i].path), "%s/%s", directory, names[i]);
        plan.images[i].offset = offsets[i]; plan.images[i].size = sizes[i]; plan.images[i].selected = true;
        f = fopen(plan.images[i].path, "wb"); assert(f); fputs("demo", f); fclose(f);
    }
    refresh_browser(); refresh_plan();
    message("Offsets loaded from flasher_args.json. Review the selected images.");
    screenshot("setup"); check_actions();
    assert(!lv_obj_has_state(flash_button, LV_STATE_DISABLED));
    start_flash(NULL); screenshot("confirmation");
    assert_inside(lv_obj_get_child(lv_obj_get_child(dialog, 0), 3), lv_obj_get_child(dialog, 0));
    close_dialog(NULL);
    lv_obj_t *edit = lv_obj_get_child(lv_obj_get_child(image_list, 2), 3);
    lv_obj_send_event(edit, LV_EVENT_CLICKED, NULL);
    assert(dialog && offset_input);
    screenshot("offset");
    assert_inside(lv_obj_get_child(lv_obj_get_child(dialog, 0), 4), lv_obj_get_child(dialog, 0));
    lv_textarea_set_text(offset_input, "0x20000");
    apply_offset(NULL);
    assert(plan.images[2].offset == 0x20000);
    start_flash(NULL); confirm_flash(NULL);
    assert(busy && lv_obj_has_state(preset, LV_STATE_DISABLED));
    mock_status.written = 700000;
    snprintf(mock_status.message, sizeof(mock_status.message), "Writing tab5-application.bin at 0x00020000");
    busy = true; lv_obj_remove_state(cancel_button, LV_STATE_DISABLED);
    refresh_layout(); poll(NULL); screenshot("progress");
    show_page(PAGE_BATTERY); screenshot("battery-flashing");
    assert(lv_obj_has_state(usb_power_switch, LV_STATE_DISABLED));
    assert(!lv_obj_has_flag(cancel_button, LV_OBJ_FLAG_HIDDEN));
    assert(!storage_request_usb(true));
    show_page(PAGE_USB_DRIVE);
    assert(lv_obj_has_state(drive_start, LV_STATE_DISABLED));
    start_drive(NULL);
    assert(!dialog);
    show_page(PAGE_BURNER);
    mock_status.state = BURNER_DONE; mock_status.written = mock_status.total;
    storage_gate_release(&mock_gate);
    snprintf(mock_status.message, sizeof(mock_status.message), "All selected images written and MD5 verified. Client reset requested.");
    poll(NULL); screenshot("complete");
    assert(lv_bar_get_value(progress) == 100);
    assert(!lv_obj_has_state(preset, LV_STATE_DISABLED));
    show_page(PAGE_BATTERY); screenshot("battery");
    assert_inside(brightness_slider, lv_obj_get_parent(brightness_slider));
    assert_inside(lv_obj_get_parent(battery_level), battery_page);
    lv_slider_set_value(brightness_slider, 35, LV_ANIM_OFF);
    lv_obj_send_event(brightness_slider, LV_EVENT_RELEASED, NULL);
    assert(mock_power.brightness == 35);
    lv_obj_remove_state(charge_switch, LV_STATE_CHECKED);
    lv_obj_send_event(charge_switch, LV_EVENT_VALUE_CHANGED, NULL);
    assert(!mock_power.charge_enabled);
    show_page(PAGE_USB_DRIVE); screenshot("usb-drive-local");
    assert_inside(drive_remount, lv_obj_get_parent(drive_remount));
    start_drive(NULL); screenshot("usb-drive-confirmation");
    assert_inside(lv_obj_get_child(lv_obj_get_child(dialog, 0), 3), lv_obj_get_child(dialog, 0));
    confirm_drive_start(NULL);
    assert(mock_gate.mode == STORAGE_SWITCHING && !storage_claim_local());
    mock_gate.mode = STORAGE_USB; mock_storage.usb_active = true; mock_storage.pc_connected = true;
    snprintf(mock_storage.message, sizeof(mock_storage.message), "SD card shared over USB-C. Eject on the PC before stopping USB drive.");
    ++mock_storage.generation;
    poll(NULL); screenshot("usb-drive-active");
    assert(lv_obj_has_state(drive_start, LV_STATE_DISABLED));
    assert(!lv_obj_has_state(drive_stop, LV_STATE_DISABLED));
    assert(plan.count == 0 && !sd_ready && lv_obj_has_state(flash_button, LV_STATE_DISABLED));
    assert(!storage_request_remount());
    assert(!burner_start(&plan));
    show_page(PAGE_BATTERY); screenshot("battery-usb-active");
    assert(lv_obj_has_state(quick_switch, LV_STATE_DISABLED));
    show_page(PAGE_USB_DRIVE);
    stop_drive(NULL); screenshot("usb-drive-stop"); confirm_drive_stop(NULL);
    mock_gate.mode = STORAGE_LOCAL; mock_storage.usb_active = false; mock_storage.pc_connected = false;
    ++mock_storage.generation;
    poll(NULL); show_page(PAGE_BURNER);
    assert(sd_ready && !lv_obj_has_state(preset, LV_STATE_DISABLED));
    mock_power.telemetry_valid = false; mock_power.voltage_percent = -1;
    show_page(PAGE_BATTERY); screenshot("battery-unavailable");
    assert(!strcmp(lv_label_get_text(battery_level), "Level unavailable"));
    show_page(PAGE_BURNER);
    mock_gate.mode = STORAGE_UNAVAILABLE;
    sd_ready = false; memset(&plan, 0, sizeof(plan));
    refresh_browser(); refresh_plan(); screenshot("no-sd");
    assert(mock_gate.readers == 0);
    puts("PASS: LVGL pages, power controls, exclusive USB SD access, offset editing and progress");
    return 0;
}
