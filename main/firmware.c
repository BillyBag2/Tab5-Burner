#include "firmware.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "cJSON.h"

const char *fw_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool fw_is_bin(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".bin") == 0;
}

bool fw_parse_offset(const char *text, uint32_t *offset)
{
    if (!text || !*text || !offset || *text == '-' || *text == '+' || isspace((unsigned char)*text)) return false;
    unsigned base = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { base = 16; text += 2; }
    if (!*text) return false;
    uint64_t value = 0;
    for (; *text; ++text) {
        unsigned digit;
        if (*text >= '0' && *text <= '9') digit = (unsigned)(*text - '0');
        else if (*text >= 'a' && *text <= 'f') digit = (unsigned)(*text - 'a') + 10;
        else if (*text >= 'A' && *text <= 'F') digit = (unsigned)(*text - 'A') + 10;
        else return false;
        if (digit >= base) return false;
        value = value * base + digit;
        if (value > UINT32_MAX) return false;
    }
    *offset = (uint32_t)value;
    return true;
}

bool fw_join_path(char *out, size_t capacity, const char *directory, const char *relative)
{
    if (!relative || !*relative || *relative == '/' || strchr(relative, '\\') || strchr(relative, ':')) return false;
    const char *p = relative;
    while (*p) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (!n || (n == 1 && *p == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
        if (!end) break;
        p = end + 1;
        if (!*p) return false;
    }
    int n = snprintf(out, capacity, "%s/%s", directory, relative);
    return n >= 0 && (size_t)n < capacity;
}

static bool add_image(fw_plan_t *plan, const char *path, uint32_t offset, char *error, size_t capacity)
{
    struct stat st;
    if (plan->count == FW_MAX_IMAGES) {
        snprintf(error, capacity, "Too many images (maximum %d).", FW_MAX_IMAGES);
        return false;
    }
    if (!fw_is_bin(path) || strlen(path) >= FW_PATH_MAX || stat(path, &st) || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || (uint64_t)st.st_size > FW_TAB5_FLASH_SIZE) {
        snprintf(error, capacity, "Missing, empty or oversized BIN: %.120s", fw_basename(path));
        return false;
    }
    fw_image_t *image = &plan->images[plan->count++];
    snprintf(image->path, sizeof(image->path), "%s", path);
    image->size = (uint32_t)st.st_size;
    image->offset = offset;
    image->selected = true;
    return true;
}

static int compare_images(const void *a, const void *b)
{
    return strcasecmp(((const fw_image_t *)a)->path, ((const fw_image_t *)b)->path);
}

bool fw_load_single(fw_plan_t *plan, const char *path, char *error, size_t capacity)
{
    memset(plan, 0, sizeof(*plan));
    // Merged files and application-only files both need an explicitly chosen base.
    return add_image(plan, path, FW_OFFSET_UNSET, error, capacity);
}

bool fw_load_directory(fw_plan_t *plan, const char *directory, char *error, size_t capacity)
{
    memset(plan, 0, sizeof(*plan));
    char path[FW_PATH_MAX];
    if (!fw_join_path(path, sizeof(path), directory, "flasher_args.json")) return false;
    struct stat st;
    if (stat(path, &st) == 0) {
        if (st.st_size <= 0 || st.st_size > 64 * 1024) {
            snprintf(error, capacity, "Invalid flasher_args.json size.");
            return false;
        }
        FILE *file = fopen(path, "rb");
        char *data = malloc((size_t)st.st_size + 1);
        if (!file || !data) {
            if (file) fclose(file);
            free(data);
            snprintf(error, capacity, "Cannot read flasher_args.json.");
            return false;
        }
        size_t n = fread(data, 1, (size_t)st.st_size, file);
        fclose(file);
        data[n] = 0;
        cJSON *root = n == (size_t)st.st_size ? cJSON_ParseWithOpts(data, NULL, true) : NULL;
        free(data);
        const cJSON *files = cJSON_GetObjectItemCaseSensitive(root, "flash_files");
        const cJSON *chip = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(root, "extra_esptool_args"), "chip");
        bool valid = cJSON_IsObject(files) && (!chip || (cJSON_IsString(chip) && strcmp(chip->valuestring, "esp32p4") == 0));
        const cJSON *entry;
        cJSON_ArrayForEach(entry, files) {
            uint32_t offset;
            if (!valid || !cJSON_IsString(entry) || !fw_parse_offset(entry->string, &offset) ||
                !fw_join_path(path, sizeof(path), directory, entry->valuestring)) {
                valid = false;
                break;
            }
            if (!add_image(plan, path, offset, error, capacity)) { valid = false; break; }
        }
        cJSON_Delete(root);
        if (!valid || !plan->count) {
            memset(plan, 0, sizeof(*plan));
            snprintf(error, capacity, "Invalid manifest, unsafe path, missing BIN, or non-P4 target.");
            return false; // Never fall back to guessed offsets after a broken manifest.
        }
        plan->from_manifest = true;
    } else {
        if (errno != ENOENT) {
            snprintf(error, capacity, "Cannot inspect manifest; remount the SD card.");
            return false;
        }
        DIR *dir = opendir(directory);
        if (!dir) { snprintf(error, capacity, "Cannot open directory."); return false; }
        struct dirent *entry;
        bool valid = true;
        while ((entry = readdir(dir))) {
            if (!fw_is_bin(entry->d_name)) continue;
            if (!fw_join_path(path, sizeof(path), directory, entry->d_name) ||
                !add_image(plan, path, FW_OFFSET_UNSET, error, capacity)) { valid = false; break; }
        }
        closedir(dir);
        if (!valid || !plan->count) {
            memset(plan, 0, sizeof(*plan));
            if (valid) snprintf(error, capacity, "No BIN files in this directory.");
            return false;
        }
    }
    qsort(plan->images, plan->count, sizeof(plan->images[0]), compare_images);
    return true;
}

void fw_apply_standard_layout(fw_plan_t *plan)
{
    size_t unknown = 0, app_index = 0;
    for (size_t i = 0; i < plan->count; ++i) {
        fw_image_t *image = &plan->images[i];
        const char *name = fw_basename(image->path);
        if (!strcasecmp(name, "bootloader.bin")) image->offset = 0x2000;
        else if (!strcasecmp(name, "partition-table.bin") || !strcasecmp(name, "partitions.bin")) image->offset = 0x8000;
        else if (!strcasecmp(name, "ota_data_initial.bin") || !strcasecmp(name, "boot_app0.bin")) image->offset = 0xe000;
        else { image->offset = FW_OFFSET_UNSET; ++unknown; app_index = i; }
    }
    if (unknown == 1) plan->images[app_index].offset = 0x10000;
    plan->from_manifest = false;
}

uint32_t fw_total_bytes(const fw_plan_t *plan)
{
    uint32_t total = 0;
    for (size_t i = 0; i < plan->count; ++i) if (plan->images[i].selected) total += plan->images[i].size;
    return total;
}

bool fw_validate(const fw_plan_t *plan, uint32_t flash_size, char *error, size_t capacity)
{
    size_t selected = 0;
    for (size_t i = 0; i < plan->count; ++i) {
        const fw_image_t *a = &plan->images[i];
        if (!a->selected) continue;
        ++selected;
        if (a->offset == FW_OFFSET_UNSET || (a->offset & 0xfff) || !a->size) {
            snprintf(error, capacity, "Set a 4 KB aligned offset for %.80s.", fw_basename(a->path));
            return false;
        }
        uint64_t end = (uint64_t)a->offset + ((a->size + 4095ULL) & ~4095ULL);
        if (end > flash_size) {
            snprintf(error, capacity, "%.80s exceeds target flash capacity.", fw_basename(a->path));
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            const fw_image_t *b = &plan->images[j];
            uint64_t b_end = (uint64_t)b->offset + ((b->size + 4095ULL) & ~4095ULL);
            if (b->selected && a->offset < b_end && b->offset < end) {
                snprintf(error, capacity, "Erase sectors overlap: %.55s / %.55s.", fw_basename(a->path), fw_basename(b->path));
                return false;
            }
        }
    }
    if (!selected) { snprintf(error, capacity, "Select at least one BIN file."); return false; }
    snprintf(error, capacity, "%u images / %lu bytes ready. Each image will be verified.",
             (unsigned)selected, (unsigned long)fw_total_bytes(plan));
    return true;
}

bool fw_preflight(const fw_plan_t *plan, char *error, size_t capacity)
{
    // Read every selected file before erasing anything. Detect removal and wrong-chip images.
    uint8_t buffer[1024];
    for (size_t i = 0; i < plan->count; ++i) {
        const fw_image_t *image = &plan->images[i];
        if (!image->selected) continue;
        struct stat st;
        FILE *file = fopen(image->path, "rb");
        if (!file || stat(image->path, &st) || (uint64_t)st.st_size != image->size) {
            if (file) fclose(file);
            snprintf(error, capacity, "BIN changed or SD card unavailable: %.70s.", fw_basename(image->path));
            return false;
        }
        size_t n, total = 0;
        bool valid = true;
        while ((n = fread(buffer, 1, sizeof(buffer), file))) {
            if (!total && n >= 24 && buffer[0] == 0xe9 && (buffer[12] != 0x12 || buffer[13] != 0)) valid = false;
            total += n;
        }
        valid = valid && !ferror(file) && total == image->size;
        fclose(file);
        if (!valid) {
            snprintf(error, capacity, "Unreadable or non-ESP32-P4 image: %.80s.", fw_basename(image->path));
            return false;
        }
    }
    return true;
}
