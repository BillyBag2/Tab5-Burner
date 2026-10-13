#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FW_PATH_MAX 512
#define FW_MAX_IMAGES 32
#define FW_TAB5_FLASH_SIZE (16U * 1024U * 1024U)
#define FW_OFFSET_UNSET UINT32_MAX

typedef struct {
    char path[FW_PATH_MAX];
    uint32_t size;
    uint32_t offset;
    bool selected;
} fw_image_t;

typedef struct {
    fw_image_t images[FW_MAX_IMAGES];
    size_t count;
    bool from_manifest;
} fw_plan_t;

bool fw_is_bin(const char *name);
bool fw_parse_offset(const char *text, uint32_t *offset);
bool fw_join_path(char *out, size_t capacity, const char *directory, const char *relative);
bool fw_load_directory(fw_plan_t *plan, const char *directory, char *error, size_t capacity);
bool fw_load_single(fw_plan_t *plan, const char *path, char *error, size_t capacity);
void fw_apply_standard_layout(fw_plan_t *plan);
bool fw_validate(const fw_plan_t *plan, uint32_t flash_size, char *error, size_t capacity);
bool fw_preflight(const fw_plan_t *plan, char *error, size_t capacity);
uint32_t fw_total_bytes(const fw_plan_t *plan);
const char *fw_basename(const char *path);
