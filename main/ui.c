#include "ui.h"
#include "burner.h"
#include "firmware.h"
#include "storage.h"
#include "power_service.h"
#include "bsp/esp-bsp.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_ENTRIES 128
typedef struct { char name[256]; bool directory; } browser_entry_t;
static browser_entry_t entries[MAX_ENTRIES];
static fw_plan_t plan;
static char directory[FW_PATH_MAX] = BSP_SD_MOUNT_POINT;
static bool sd_ready, usb_ready, busy;
static lv_obj_t *browser_list, *path_label, *image_list, *layout, *validation;
static lv_obj_t *progress, *progress_label, *status_label, *flash_button, *cancel_button, *preset;
static lv_obj_t *dialog, *offset_input;
static lv_obj_t *browser_panel, *setup_panel;
static size_t editing_index;
static burner_state_t previous_state = BURNER_IDLE;
static const uint32_t colors[] = { 0x4dd7b6, 0x6c9dff, 0xf0bb63, 0xca93ff, 0xed8199 };
typedef enum { PAGE_BURNER, PAGE_BATTERY, PAGE_USB_DRIVE } ui_page_t;
static ui_page_t page;
static lv_obj_t *burner_page, *battery_page, *drive_page, *footer_info, *remount_button;
static lv_obj_t *footer_panel, *page_tabs[3];
static lv_obj_t *battery_level, *battery_voltage, *battery_current, *battery_flow, *battery_bar;
static lv_obj_t *battery_charger;
static lv_obj_t *charge_switch, *quick_switch, *usb_power_switch, *brightness_slider, *brightness_label;
static lv_obj_t *drive_state, *drive_capacity, *drive_connection, *drive_message, *drive_start, *drive_stop, *drive_remount;
static unsigned storage_generation;

static void refresh_browser(void);
static void refresh_plan(void);
static void refresh_layout(void);
static void refresh_services(void);

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xe5edfa), 0);
    return obj;
}

static lv_obj_t *box(lv_obj_t *parent, int32_t width, int32_t height)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x172238), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 16, 0);
    lv_obj_set_style_pad_all(obj, 16, 0);
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t callback)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_set_height(obj, 48);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x2d4261), 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_t *caption = label(obj, text, &lv_font_montserrat_20);
    lv_obj_center(caption);
    if (callback) lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, NULL);
    return obj;
}

static void message(const char *text)
{
    lv_label_set_text(status_label, text);
}

static void enable_controls(lv_obj_t *parent, bool enabled)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t *child = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(child, &lv_button_class) || lv_obj_check_type(child, &lv_checkbox_class) ||
            lv_obj_check_type(child, &lv_dropdown_class)) {
            if (enabled) lv_obj_remove_state(child, LV_STATE_DISABLED);
            else lv_obj_add_state(child, LV_STATE_DISABLED);
        }
        enable_controls(child, enabled);
    }
}

static void close_dialog(lv_event_t *event)
{
    (void)event;
    if (dialog) lv_obj_delete(dialog);
    dialog = NULL;
    offset_input = NULL;
}

static lv_obj_t *open_dialog(const char *title, int32_t height)
{
    dialog = lv_obj_create(lv_screen_active());
    lv_obj_add_flag(dialog, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dialog, lv_display_get_horizontal_resolution(NULL), lv_display_get_vertical_resolution(NULL));
    lv_obj_center(dialog);
    lv_obj_set_style_bg_color(dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_70, 0);
    lv_obj_set_style_border_width(dialog, 0, 0);
    lv_obj_t *card = box(dialog, 1000, height);
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    label(card, title, &lv_font_montserrat_24);
    return card;
}

static int compare_entries(const void *a, const void *b)
{
    const browser_entry_t *aa = a, *bb = b;
    if (aa->directory != bb->directory) return aa->directory ? -1 : 1;
    return strcasecmp(aa->name, bb->name);
}

static void browse_entry(lv_event_t *event)
{
    if (busy || !storage_claim_local()) return;
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    char path[FW_PATH_MAX], error[192];
    if (!fw_join_path(path, sizeof(path), directory, entries[index].name)) {
        storage_release_local(); message("Path is too long."); return;
    }
    if (entries[index].directory) {
        snprintf(directory, sizeof(directory), "%s", path);
        refresh_browser();
    } else {
        if (fw_load_single(&plan, path, error, sizeof(error))) {
            lv_dropdown_set_selected(preset, 0);
            refresh_plan();
            message("Choose merged image at 0x0, standard app layout, or enter a custom offset.");
        } else { refresh_plan(); message(error); }
    }
    storage_release_local();
}

static void refresh_browser(void)
{
    lv_label_set_text(path_label, directory);
    lv_obj_clean(browser_list);
    if (!sd_ready || !storage_claim_local()) {
        storage_status_t storage;
        storage_get_status(&storage);
        label(browser_list, storage.mode == STORAGE_USB || storage.mode == STORAGE_SWITCHING ?
            "SD card reserved for USB transfer.\nStop USB drive to browse files." :
            "Insert a FAT32 SD card,\nthen tap Remount SD.", &lv_font_montserrat_20);
        return;
    }
    DIR *dir = opendir(directory);
    if (!dir) { storage_release_local(); message("Cannot read SD directory. Remount the card."); return; }
    size_t count = 0;
    struct dirent *entry;
    bool truncated = false;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char path[FW_PATH_MAX];
        struct stat st;
        if (!fw_join_path(path, sizeof(path), directory, entry->d_name) || stat(path, &st)) continue;
        if (!S_ISDIR(st.st_mode) && (!S_ISREG(st.st_mode) || !fw_is_bin(entry->d_name))) continue;
        if (count == MAX_ENTRIES) { truncated = true; break; }
        snprintf(entries[count].name, sizeof(entries[count].name), "%s", entry->d_name);
        entries[count++].directory = S_ISDIR(st.st_mode);
    }
    closedir(dir);
    storage_release_local();
    qsort(entries, count, sizeof(entries[0]), compare_entries);
    for (size_t i = 0; i < count; ++i) {
        lv_obj_t *row = button(browser_list, "", NULL);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_t *caption = lv_obj_get_child(row, 0);
        lv_label_set_text_fmt(caption, "%s  %s", entries[i].directory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE, entries[i].name);
        lv_obj_set_width(caption, LV_PCT(100));
        lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
        lv_obj_add_event_cb(row, browse_entry, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    if (!count) label(browser_list, "No directories or BIN files here.", &lv_font_montserrat_20);
    if (truncated) message("Showing the first 128 entries. Organise firmware into smaller directories.");
}

static void up_directory(lv_event_t *event)
{
    (void)event;
    if (busy || !strcmp(directory, BSP_SD_MOUNT_POINT)) return;
    char *slash = strrchr(directory, '/');
    if (slash) *slash = 0;
    refresh_browser();
}

static void remount(lv_event_t *event)
{
    (void)event;
    if (busy) return;
    if (!storage_request_remount()) { message("SD card is in use. Stop flashing or USB drive first."); return; }
    sd_ready = false;
    memset(&plan, 0, sizeof(plan));
    snprintf(directory, sizeof(directory), "%s", BSP_SD_MOUNT_POINT);
    refresh_browser();
    refresh_plan();
    message("Remounting SD card...");
}

static void load_directory(lv_event_t *event)
{
    (void)event;
    if (busy || !sd_ready || !storage_claim_local()) return;
    char error[192];
    bool ok = fw_load_directory(&plan, directory, error, sizeof(error));
    storage_release_local();
    lv_dropdown_set_selected(preset, 0);
    refresh_plan();
    message(ok ? (plan.from_manifest ? "Offsets loaded from flasher_args.json. Review the selected images." :
            "Directory loaded. Choose a layout or set each offset manually.") : error);
}

static void selection_changed(lv_event_t *event)
{
    if (busy) return;
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    plan.images[index].selected = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    refresh_layout();
}

static void apply_offset(lv_event_t *event)
{
    (void)event;
    uint32_t offset;
    if (!fw_parse_offset(lv_textarea_get_text(offset_input), &offset) || (offset & 0xfff)) {
        message("Enter a 4 KB aligned address, for example 0x10000.");
        return;
    }
    plan.images[editing_index].offset = offset;
    lv_dropdown_set_selected(preset, 0);
    close_dialog(NULL);
    refresh_plan();
}

static void edit_offset(lv_event_t *event)
{
    if (busy || dialog) return;
    editing_index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    lv_obj_t *card = open_dialog("Flash address (hexadecimal or decimal)", 600);
    label(card, fw_basename(plan.images[editing_index].path), &lv_font_montserrat_20);
    offset_input = lv_textarea_create(card);
    lv_obj_set_style_bg_color(offset_input, lv_color_hex(0x20304a), 0);
    lv_obj_set_style_text_color(offset_input, lv_color_hex(0xe5edfa), 0);
    lv_obj_set_width(offset_input, LV_PCT(100));
    lv_textarea_set_one_line(offset_input, true);
    lv_textarea_set_accepted_chars(offset_input, "0123456789abcdefABCDEFxX");
    lv_textarea_set_max_length(offset_input, 10);
    if (plan.images[editing_index].offset != FW_OFFSET_UNSET) {
        char text[16];
        snprintf(text, sizeof(text), "0x%08lx", (unsigned long)plan.images[editing_index].offset);
        lv_textarea_set_text(offset_input, text);
    }
    lv_obj_t *keyboard = lv_keyboard_create(card);
    lv_obj_set_size(keyboard, LV_PCT(100), 260);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x172238), 0);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x2d4261), LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(0xe5edfa), LV_PART_ITEMS);
    lv_keyboard_set_textarea(keyboard, offset_input);
    lv_obj_t *actions = box(card, LV_PCT(100), 68);
    lv_obj_set_style_pad_all(actions, 10, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    button(actions, "Apply address", apply_offset);
    button(actions, "Cancel", close_dialog);
}

static void preset_changed(lv_event_t *event)
{
    (void)event;
    if (busy) return;
    uint32_t selected = lv_dropdown_get_selected(preset);
    if (selected == 1) {
        fw_apply_standard_layout(&plan);
        message("Standard P4 offsets applied. Check against the firmware's original partition layout.");
    } else if (selected == 2) {
        if (plan.count != 1) {
            message("Merged layout requires selecting one merged BIN in the browser.");
            lv_dropdown_set_selected(preset, 0);
            return;
        }
        plan.images[0].offset = 0;
        message("Merged image will be written from address 0x0.");
    }
    refresh_plan();
}

static void refresh_layout(void)
{
    lv_obj_clean(layout);
    int32_t width = lv_obj_get_content_width(layout);
    if (width < 1) width = 600;
    for (size_t i = 0; i < plan.count; ++i) {
        fw_image_t *image = &plan.images[i];
        if (!image->selected || image->offset == FW_OFFSET_UNSET || image->offset >= FW_TAB5_FLASH_SIZE) continue;
        uint32_t length = image->size > FW_TAB5_FLASH_SIZE - image->offset ? FW_TAB5_FLASH_SIZE - image->offset : image->size;
        int32_t x = (int64_t)image->offset * width / FW_TAB5_FLASH_SIZE;
        int32_t w = (int64_t)length * width / FW_TAB5_FLASH_SIZE;
        if (w < 4) w = 4; // Minimum visible marker; exact ranges remain in the image rows.
        if (x + w > width) w = width - x;
        lv_obj_t *segment = lv_obj_create(layout);
        lv_obj_set_pos(segment, x, 0);
        lv_obj_set_size(segment, w, 24);
        lv_obj_set_style_pad_all(segment, 0, 0);
        lv_obj_set_style_border_width(segment, 0, 0);
        lv_obj_set_style_radius(segment, 2, 0);
        lv_obj_set_style_bg_color(segment, lv_color_hex(colors[i % 5]), 0);
    }
    char error[192];
    bool valid = fw_validate(&plan, FW_TAB5_FLASH_SIZE, error, sizeof(error));
    lv_label_set_text(validation, error);
    lv_obj_set_style_text_color(validation, lv_color_hex(valid ? 0x4dd7b6 : 0xf0bb63), 0);
    if (valid && usb_ready && sd_ready && !busy && burner_usb_power_enabled()) lv_obj_remove_state(flash_button, LV_STATE_DISABLED);
    else lv_obj_add_state(flash_button, LV_STATE_DISABLED);
}

static void refresh_plan(void)
{
    lv_obj_clean(image_list);
    for (size_t i = 0; i < plan.count; ++i) {
        fw_image_t *image = &plan.images[i];
        lv_obj_t *row = box(image_list, LV_PCT(100), 64);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x20304a), 0);
        lv_obj_t *check = lv_checkbox_create(row);
        lv_checkbox_set_text(check, "");
        lv_obj_align(check, LV_ALIGN_LEFT_MID, 0, 0);
        if (image->selected) lv_obj_add_state(check, LV_STATE_CHECKED);
        lv_obj_add_event_cb(check, selection_changed, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)i);
        lv_obj_t *name = label(row, fw_basename(image->path), &lv_font_montserrat_20);
        lv_obj_set_width(name, LV_PCT(57));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(name, 42, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(colors[i % 5]), 0);
        lv_obj_t *detail = label(row, "", &lv_font_montserrat_20);
        lv_label_set_text_fmt(detail, "%lu bytes", (unsigned long)image->size);
        lv_obj_set_pos(detail, 42, 24);
        lv_obj_t *edit = button(row, "", NULL);
        lv_obj_set_width(edit, 168);
        lv_obj_align(edit, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_t *caption = lv_obj_get_child(edit, 0);
        if (image->offset == FW_OFFSET_UNSET) lv_label_set_text(caption, "Set offset");
        else lv_label_set_text_fmt(caption, "0x%08lx", (unsigned long)image->offset);
        lv_obj_add_event_cb(edit, edit_offset, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    if (!plan.count) label(image_list, "Load a firmware folder or select a BIN.\nSeparate images stay together in one folder.", &lv_font_montserrat_20);
    refresh_layout();
}

static void confirm_flash(lv_event_t *event)
{
    (void)event;
    char error[192];
    if (busy || !fw_validate(&plan, FW_TAB5_FLASH_SIZE, error, sizeof(error))) return;
    close_dialog(NULL);
    if (burner_start(&plan)) {
        busy = true;
        enable_controls(browser_panel, false);
        enable_controls(setup_panel, false);
        lv_obj_add_state(flash_button, LV_STATE_DISABLED);
        lv_obj_remove_state(cancel_button, LV_STATE_DISABLED);
    } else message("Flasher is still finishing the previous request. Try again.");
}

static void start_flash(lv_event_t *event)
{
    (void)event;
    if (busy || dialog || !usb_ready || !sd_ready) return;
    lv_obj_t *card = open_dialog("Flash the companion Tab5?", 580);
    lv_obj_t *notice = label(card, "This replaces the selected flash sectors on the client.\nHost USB-A -> client USB-C. Keep both devices powered.\nIf connection fails, hold the client's reset button for about\n2 seconds until its green LED flashes, then release.", &lv_font_montserrat_20);
    lv_obj_set_width(notice, LV_PCT(100));
    lv_obj_t *list = box(card, LV_PCT(100), 180);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    for (size_t i = 0; i < plan.count; ++i) {
        if (!plan.images[i].selected) continue;
        lv_obj_t *item = label(list, "", &lv_font_montserrat_20);
        lv_obj_set_width(item, LV_PCT(100));
        lv_label_set_long_mode(item, LV_LABEL_LONG_WRAP);
        lv_label_set_text_fmt(item, "0x%08lx   %lu bytes   %s", (unsigned long)plan.images[i].offset,
            (unsigned long)plan.images[i].size, fw_basename(plan.images[i].path));
    }
    lv_obj_t *actions = box(card, LV_PCT(100), 68);
    lv_obj_set_style_pad_all(actions, 10, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    button(actions, "Write and verify", confirm_flash);
    button(actions, "Back", close_dialog);
}

static void cancel_flash(lv_event_t *event)
{
    (void)event;
    if (!busy) return;
    burner_cancel();
    message("Cancellation requested; waiting for the current USB command to finish.");
    lv_obj_add_state(cancel_button, LV_STATE_DISABLED);
}

static void poll(lv_timer_t *timer)
{
    (void)timer;
    burner_status_t status = {0};
    if (usb_ready) burner_get_status(&status);
    bool was_busy = busy;
    busy = status.state == BURNER_BUSY;
    int percent = status.total ? (int)((uint64_t)status.written * 100 / status.total) : 0;
    // Transfer completion isn't success until every MD5 has passed.
    if (status.state != BURNER_DONE && percent == 100) percent = 99;
    lv_bar_set_value(progress, percent, LV_ANIM_OFF);
    lv_label_set_text_fmt(progress_label, "%d%%  |  %lu / %lu bytes", percent,
        (unsigned long)status.written, (unsigned long)status.total);
    if (busy || status.state != previous_state) message(status.message);
    previous_state = status.state;
    if (was_busy && !busy) {
        enable_controls(browser_panel, true);
        enable_controls(setup_panel, true);
        lv_obj_add_state(cancel_button, LV_STATE_DISABLED);
        refresh_layout();
    }
    refresh_services();
}

static void set_enabled(lv_obj_t *obj, bool enabled)
{
    if (enabled) lv_obj_remove_state(obj, LV_STATE_DISABLED);
    else lv_obj_add_state(obj, LV_STATE_DISABLED);
}

static void show_page(ui_page_t selected)
{
    if (dialog) return;
    page = selected;
    for (unsigned i = 0; i < 3; ++i)
        lv_obj_set_style_bg_color(page_tabs[i], lv_color_hex(i == (unsigned)selected ? 0x197c70 : 0x2d4261), 0);
    lv_obj_add_flag(burner_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(battery_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(drive_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(selected == PAGE_BURNER ? burner_page : selected == PAGE_BATTERY ? battery_page : drive_page, LV_OBJ_FLAG_HIDDEN);
    if (selected == PAGE_BURNER) lv_obj_remove_flag(flash_button, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(flash_button, LV_OBJ_FLAG_HIDDEN);
    if (selected == PAGE_BURNER || busy) lv_obj_remove_flag(cancel_button, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(cancel_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(footer_info, selected == PAGE_BURNER ? LV_PCT(72) : busy ? LV_PCT(87) : LV_PCT(100));
    refresh_services();
}

static void navigate(lv_event_t *event)
{
    show_page((ui_page_t)(uintptr_t)lv_event_get_user_data(event));
}

static void power_changed(lv_event_t *event)
{
    power_action_t action = (power_action_t)(uintptr_t)lv_event_get_user_data(event);
    bool enabled = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    if (!power_request(action, enabled)) message("Power command queue is full; retry.");
}

static void brightness_changed(lv_event_t *event)
{
    unsigned value = lv_slider_get_value(brightness_slider);
    lv_label_set_text_fmt(brightness_label, "Screen brightness: %u%%", value);
    if (lv_event_get_code(event) == LV_EVENT_RELEASED && !power_request(POWER_BRIGHTNESS, value))
        message("Brightness change could not be queued.");
}

static lv_obj_t *power_toggle(lv_obj_t *parent, const char *text, power_action_t action)
{
    lv_obj_t *row = box(parent, LV_PCT(100), 58);
    lv_obj_set_style_pad_all(row, 6, 0);
    lv_obj_t *caption = label(row, text, &lv_font_montserrat_20);
    lv_obj_align(caption, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *toggle = lv_switch_create(row);
    lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_add_event_cb(toggle, power_changed, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)action);
    return toggle;
}

static void confirm_drive_start(lv_event_t *event)
{
    (void)event;
    close_dialog(NULL);
    if (!storage_request_usb(true)) { message("SD is unavailable or in use. Finish flashing and retry."); return; }
    sd_ready = false;
    memset(&plan, 0, sizeof(plan));
    refresh_browser(); refresh_plan(); refresh_services();
}

static void confirm_drive_stop(lv_event_t *event)
{
    (void)event;
    close_dialog(NULL);
    if (!storage_request_usb(false)) message("USB drive is not ready to stop; retry shortly.");
    refresh_services();
}

static void start_drive(lv_event_t *event)
{
    (void)event;
    if (busy || dialog) return;
    lv_obj_t *card = open_dialog("Share the host SD card with a computer?", 330);
    lv_obj_t *notice = label(card,
        "Connect this Tab5's USB-C port to the computer.\nBrowsing and companion flashing pause during file transfer.\nQuick charge pauses and the USB console disconnects.\nEject the drive on the computer before stopping this mode.", &lv_font_montserrat_20);
    lv_obj_set_width(notice, LV_PCT(100));
    button(card, "Enable USB drive", confirm_drive_start);
    button(card, "Back", close_dialog);
}

static void stop_drive(lv_event_t *event)
{
    (void)event;
    if (dialog) return;
    lv_obj_t *card = open_dialog("Eject the drive on your computer first", 300);
    lv_obj_t *notice = label(card,
        "Finish copying and use Safely Remove / Eject on the computer.\nStopping disconnects the USB drive, drains accepted writes,\nand remounts the SD card for local browsing.", &lv_font_montserrat_20);
    lv_obj_set_width(notice, LV_PCT(100));
    button(card, "Stop USB drive", confirm_drive_stop);
    button(card, "Back", close_dialog);
}

static void sync_switch(lv_obj_t *toggle, bool value)
{
    if (lv_obj_has_state(toggle, LV_STATE_PRESSED)) return;
    if (value) lv_obj_add_state(toggle, LV_STATE_CHECKED);
    else lv_obj_remove_state(toggle, LV_STATE_CHECKED);
}

static void refresh_services(void)
{
    storage_status_t storage;
    storage_get_status(&storage);
    bool was_ready = sd_ready;
    sd_ready = storage.mode == STORAGE_LOCAL;
    bool changed = storage_generation != storage.generation;
    if (changed) {
        storage_generation = storage.generation;
        memset(&plan, 0, sizeof(plan));
        snprintf(directory, sizeof(directory), "%s", BSP_SD_MOUNT_POINT);
        lv_dropdown_set_selected(preset, 0);
        refresh_plan();
    }
    if (changed || was_ready != sd_ready) refresh_browser();
    enable_controls(browser_panel, sd_ready && !busy);
    enable_controls(setup_panel, sd_ready && !busy);
    bool storage_idle = !storage.usb_active && storage.mode != STORAGE_SWITCHING && storage.mode != STORAGE_USB && !busy;
    set_enabled(remount_button, storage_idle);
    set_enabled(drive_remount, storage_idle);
    set_enabled(drive_start, sd_ready && !busy);
    set_enabled(drive_stop, storage.mode == STORAGE_USB);
    lv_label_set_text(drive_state, storage.mode == STORAGE_USB ? "SD CARD SHARED" :
        storage.mode == STORAGE_LOCAL ? "SD CARD LOCAL" : storage.mode == STORAGE_SWITCHING ? "SWITCHING MODE" : "SD CARD UNAVAILABLE");
    lv_label_set_text_fmt(drive_capacity, "Card capacity: %.1f GiB", (double)storage.capacity_bytes / (1024.0 * 1024.0 * 1024.0));
    lv_label_set_text(drive_connection, storage.media_ejected ? "PC ejected the card" : storage.pc_connected ? "Computer connected" : "Waiting for computer / USB-C");
    lv_label_set_text(drive_message, storage.message);

    power_status_t power;
    power_get_status(&power);
    if (power.telemetry_valid) {
        lv_label_set_text_fmt(battery_voltage, "%.2f V", (double)power.voltage);
        lv_label_set_text_fmt(battery_current, "%+.3f A", (double)power.current);
        lv_label_set_text_fmt(battery_flow, "%.2f W  |  %s", (double)(power.voltage * (power.current < 0 ? -power.current : power.current)),
            power.current > 0.02f ? "Discharging" : power.current < -0.02f ? "Charging current" : "Near zero current");
    } else {
        lv_label_set_text(battery_voltage, "Voltage unavailable");
        lv_label_set_text(battery_current, "Current unavailable");
        lv_label_set_text(battery_flow, "INA226 monitor unavailable");
    }
    if (power.voltage_percent >= 0 && power.telemetry_valid) {
        lv_label_set_text_fmt(battery_level, "%d%% estimated", power.voltage_percent);
        lv_bar_set_value(battery_bar, power.voltage_percent, LV_ANIM_OFF);
    } else { lv_label_set_text(battery_level, "Level unavailable"); lv_bar_set_value(battery_bar, 0, LV_ANIM_OFF); }
    lv_label_set_text(battery_charger, !power.charger_status_valid ? "Charger status unavailable" :
        power.charging ? "Charger status: charging" : "Charger status: idle");
    sync_switch(charge_switch, power.charge_enabled);
    sync_switch(quick_switch, power.quick_charge_enabled);
    sync_switch(usb_power_switch, power.usb_power_enabled);
    set_enabled(charge_switch, power.controls_valid);
    set_enabled(quick_switch, power.controls_valid && storage_idle);
    set_enabled(usb_power_switch, power.available && usb_ready && !busy && !burner_is_active());
    set_enabled(brightness_slider, power.available);
    if (!lv_obj_has_state(brightness_slider, LV_STATE_PRESSED)) {
        lv_slider_set_value(brightness_slider, power.brightness, LV_ANIM_OFF);
        lv_label_set_text_fmt(brightness_label, "Screen brightness: %u%%", power.brightness);
    }
    refresh_layout();
    if (!busy && page == PAGE_BATTERY) message(power.message);
    else if (!busy && (page == PAGE_USB_DRIVE || changed || was_ready != sd_ready)) message(storage.message);
    if (page != PAGE_BURNER) {
        if (busy) lv_obj_remove_flag(cancel_button, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(cancel_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(footer_info, busy ? LV_PCT(87) : LV_PCT(100));
    }
    bool show_progress = page == PAGE_BURNER || busy;
    if (show_progress) {
        lv_obj_remove_flag(progress, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(progress_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(progress, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(progress_label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_height(footer_panel, show_progress ? 128 : 80);
}

static void create_service_pages(lv_obj_t *screen)
{
    battery_page = box(screen, LV_PCT(100), 450);
    lv_obj_set_flex_grow(battery_page, 1);
    lv_obj_set_flex_flow(battery_page, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(battery_page, 0, 0);
    lv_obj_set_style_pad_column(battery_page, 16, 0);
    lv_obj_set_style_bg_opa(battery_page, LV_OPA_TRANSP, 0);
    lv_obj_t *readings = box(battery_page, LV_PCT(42), LV_PCT(100));
    lv_obj_set_style_pad_row(readings, 8, 0);
    lv_obj_set_flex_flow(readings, LV_FLEX_FLOW_COLUMN);
    label(readings, "HOST BATTERY", &lv_font_montserrat_24);
    battery_level = label(readings, "Level unavailable", &lv_font_montserrat_32);
    battery_bar = lv_bar_create(readings);
    lv_obj_set_size(battery_bar, LV_PCT(100), 16);
    lv_obj_set_style_bg_color(battery_bar, lv_color_hex(0x4dd7b6), LV_PART_INDICATOR);
    label(readings, "Battery rail voltage", &lv_font_montserrat_20);
    battery_voltage = label(readings, "-- V", &lv_font_montserrat_32);
    label(readings, "Current (+ discharge / - charge)", &lv_font_montserrat_20);
    battery_current = label(readings, "-- A", &lv_font_montserrat_32);
    battery_flow = label(readings, "", &lv_font_montserrat_20);
    battery_charger = label(readings, "", &lv_font_montserrat_20);
    lv_obj_t *estimate = label(readings, "Voltage estimate only. A charger can raise\nthe rail even without a battery fitted.", &lv_font_montserrat_20);
    lv_obj_set_width(estimate, LV_PCT(100));
    lv_obj_t *controls = box(battery_page, LV_PCT(56), LV_PCT(100));
    lv_obj_set_flex_grow(controls, 1);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_COLUMN);
    label(controls, "POWER CONTROLS", &lv_font_montserrat_24);
    charge_switch = power_toggle(controls, "Enable battery charging", POWER_CHARGING);
    quick_switch = power_toggle(controls, "Quick charge (USB-C)", POWER_QUICK_CHARGE);
    usb_power_switch = power_toggle(controls, "USB-A power to companion", POWER_USB_A);
    brightness_label = label(controls, "Screen brightness: 70%", &lv_font_montserrat_20);
    brightness_slider = lv_slider_create(controls);
    lv_obj_set_size(brightness_slider, LV_PCT(92), 18);
    lv_slider_set_range(brightness_slider, 10, 100);
    lv_slider_set_value(brightness_slider, 70, LV_ANIM_OFF);
    lv_obj_add_event_cb(brightness_slider, brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(brightness_slider, brightness_changed, LV_EVENT_RELEASED, NULL);
    lv_obj_t *note = label(controls, "USB-A power is locked while flashing.\nQuick charge pauses while USB drive is active.\nCharging limits stay under the board charger's control.", &lv_font_montserrat_20);
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_add_flag(battery_page, LV_OBJ_FLAG_HIDDEN);

    drive_page = box(screen, LV_PCT(100), 450);
    lv_obj_set_flex_grow(drive_page, 1);
    lv_obj_set_flex_flow(drive_page, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(drive_page, 0, 0);
    lv_obj_set_style_pad_column(drive_page, 16, 0);
    lv_obj_set_style_bg_opa(drive_page, LV_OPA_TRANSP, 0);
    lv_obj_t *instructions = box(drive_page, LV_PCT(62), LV_PCT(100));
    lv_obj_set_flex_flow(instructions, LV_FLEX_FLOW_COLUMN);
    label(instructions, "USB SD CARD DRIVE", &lv_font_montserrat_32);
    label(instructions, "HOST TAB5 USB-C  <->  COMPUTER", &lv_font_montserrat_24);
    lv_obj_t *steps = label(instructions,
        "1. Enable USB drive and connect a USB data cable.\n\n2. Open the SD card drive on the computer and copy files.\n   Keep each non-merged firmware set in its own directory.\n\n3. Safely eject the drive on the computer.\n\n4. Stop USB drive to return to browsing and flashing.", &lv_font_montserrat_20);
    lv_obj_set_width(steps, LV_PCT(100));
    lv_obj_t *note2 = label(instructions, "File transfer uses USB-C. USB-A stays the companion port.\nThe USB programming console is unavailable in drive mode.", &lv_font_montserrat_20);
    lv_obj_set_width(note2, LV_PCT(100));
    lv_obj_t *actions = box(drive_page, LV_PCT(36), LV_PCT(100));
    lv_obj_set_flex_grow(actions, 1);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_COLUMN);
    drive_state = label(actions, "SD CARD LOCAL", &lv_font_montserrat_24);
    drive_capacity = label(actions, "", &lv_font_montserrat_20);
    drive_connection = label(actions, "", &lv_font_montserrat_20);
    lv_obj_set_width(drive_connection, LV_PCT(100));
    drive_message = label(actions, "", &lv_font_montserrat_20);
    lv_obj_set_width(drive_message, LV_PCT(100));
    lv_obj_set_height(drive_message, 98);
    drive_start = button(actions, "Enable USB drive", start_drive);
    drive_stop = button(actions, "Stop USB drive", stop_drive);
    drive_remount = button(actions, "Remount SD", remount);
    lv_obj_add_flag(drive_page, LV_OBJ_FLAG_HIDDEN);
}

void ui_init(bool mounted, bool ready)
{
    sd_ready = mounted;
    usb_ready = ready;
    storage_status_t initial_storage;
    storage_get_status(&initial_storage);
    storage_generation = initial_storage.generation;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0c1424), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xe5edfa), 0);
    lv_obj_set_style_pad_all(screen, 20, 0);
    lv_obj_set_style_pad_row(screen, 14, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *header = box(screen, LV_PCT(100), 78);
    lv_obj_set_style_pad_ver(header, 8, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(header, "TAB5  /  FIRMWARE BURNER", &lv_font_montserrat_24);
    lv_obj_t *navigation = box(header, 490, 56);
    lv_obj_set_style_pad_all(navigation, 4, 0);
    lv_obj_set_flex_flow(navigation, LV_FLEX_FLOW_ROW);
    const char *pages[] = { "Burner", "Battery", "USB drive" };
    for (size_t i = 0; i < 3; ++i) {
        lv_obj_t *tab = button(navigation, pages[i], NULL);
        page_tabs[i] = tab;
        lv_obj_set_width(tab, 150);
        lv_obj_add_event_cb(tab, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }

    lv_obj_t *body = box(screen, LV_PCT(100), 450);
    burner_page = body;
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_column(body, 16, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_t *browser = box(body, LV_PCT(38), LV_PCT(100));
    browser_panel = browser;
    lv_obj_set_flex_flow(browser, LV_FLEX_FLOW_COLUMN);
    label(browser, LV_SYMBOL_SD_CARD "  SD CARD", &lv_font_montserrat_24);
    path_label = label(browser, directory, &lv_font_montserrat_20);
    lv_obj_set_width(path_label, LV_PCT(100));
    lv_label_set_long_mode(path_label, LV_LABEL_LONG_DOT);
    lv_obj_t *nav = box(browser, LV_PCT(100), 68);
    lv_obj_set_style_pad_all(nav, 4, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    button(nav, LV_SYMBOL_UP "  Up", up_directory);
    remount_button = button(nav, "Remount SD", remount);
    browser_list = box(browser, LV_PCT(100), 180);
    lv_obj_set_flex_grow(browser_list, 1);
    lv_obj_set_style_pad_all(browser_list, 0, 0);
    lv_obj_set_flex_flow(browser_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *load = button(browser, "Load this firmware folder", load_directory);
    lv_obj_set_width(load, LV_PCT(100));

    lv_obj_t *setup = box(body, LV_PCT(60), LV_PCT(100));
    setup_panel = setup;
    lv_obj_set_style_pad_row(setup, 6, 0);
    lv_obj_set_flex_grow(setup, 1);
    lv_obj_set_flex_flow(setup, LV_FLEX_FLOW_COLUMN);
    label(setup, "FLASH SETUP", &lv_font_montserrat_24);
    preset = lv_dropdown_create(setup);
    lv_obj_set_style_bg_color(preset, lv_color_hex(0x20304a), 0);
    lv_obj_set_style_text_color(preset, lv_color_hex(0xe5edfa), 0);
    lv_obj_set_width(preset, LV_PCT(100));
    lv_dropdown_set_options(preset, "Manifest / custom addresses\nStandard ESP32-P4 layout\nMerged image at 0x0");
    lv_obj_add_event_cb(preset, preset_changed, LV_EVENT_VALUE_CHANGED, NULL);
    image_list = box(setup, LV_PCT(100), 180);
    lv_obj_set_flex_grow(image_list, 1);
    lv_obj_set_style_pad_all(image_list, 0, 0);
    lv_obj_set_style_pad_row(image_list, 6, 0);
    lv_obj_set_flex_flow(image_list, LV_FLEX_FLOW_COLUMN);
    label(setup, "CLIENT FLASH   0x0 ------------------------------------ 16 MB", &lv_font_montserrat_20);
    layout = box(setup, LV_PCT(100), 26);
    lv_obj_set_style_pad_all(layout, 0, 0);
    lv_obj_set_style_bg_color(layout, lv_color_hex(0x2d4261), 0);
    lv_obj_remove_flag(layout, LV_OBJ_FLAG_SCROLLABLE);
    validation = label(setup, "", &lv_font_montserrat_20);
    lv_obj_set_width(validation, LV_PCT(100));
    lv_obj_set_height(validation, 52);
    lv_label_set_long_mode(validation, LV_LABEL_LONG_WRAP);

    create_service_pages(screen);
    lv_obj_t *footer = box(screen, LV_PCT(100), 128);
    footer_panel = footer;
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_t *info = box(footer, LV_PCT(72), LV_PCT(100));
    footer_info = info;
    lv_obj_set_style_pad_all(info, 0, 0);
    lv_obj_set_style_pad_row(info, 6, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    status_label = label(info, "", &lv_font_montserrat_20);
    lv_obj_set_width(status_label, LV_PCT(100));
    lv_obj_set_height(status_label, 48);
    progress = lv_bar_create(info);
    lv_obj_set_size(progress, LV_PCT(100), 8);
    lv_obj_set_style_bg_color(progress, lv_color_hex(0x4dd7b6), LV_PART_INDICATOR);
    progress_label = label(info, "0%  |  Ready", &lv_font_montserrat_20);
    flash_button = button(footer, "Flash client", start_flash);
    lv_obj_set_style_bg_color(flash_button, lv_color_hex(0x15886f), 0);
    cancel_button = button(footer, "Cancel", cancel_flash);
    lv_obj_add_state(cancel_button, LV_STATE_DISABLED);
    lv_obj_update_layout(screen);
    refresh_browser();
    refresh_plan();
    show_page(PAGE_BURNER);
    message(!usb_ready ? "USB host initialisation failed. Restart the host and check power." :
        sd_ready ? "Connect host USB-A to client USB-C. Load firmware from the SD card." : "Insert a FAT32 SD card, then tap Remount SD.");
    lv_timer_create(poll, 200, NULL);
}
