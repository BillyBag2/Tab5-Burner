// Host tests for the same parser and flash plan used by the device.
#include "firmware.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static fw_plan_t plan;
static char error[192];

static void write_file(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(data, 1, size, file) == size);
    assert(fclose(file) == 0);
}

static void manifest(const char *json)
{
    write_file("fixtures/flasher_args.json", json, strlen(json));
}

int main(void)
{
    uint32_t offset;
    char path[FW_PATH_MAX];
    assert(fw_parse_offset("0x10000", &offset) && offset == 65536);
    assert(fw_parse_offset("65536", &offset) && offset == 65536);
    assert(!fw_parse_offset("-4096", &offset));
    assert(!fw_parse_offset("+4096", &offset));
    assert(!fw_parse_offset(" 4096", &offset));
    assert(!fw_parse_offset("0x", &offset));
    assert(!fw_parse_offset("0x1000junk", &offset));
    assert(!fw_parse_offset("4294967296", &offset));
    assert(fw_is_bin("APP.BIN") && !fw_is_bin("app.bin.txt"));
    assert(fw_join_path(path, sizeof(path), "fixtures", "bootloader/bootloader.bin"));
    assert(!fw_join_path(path, sizeof(path), "fixtures", "../outside.bin"));
    assert(!fw_join_path(path, sizeof(path), "fixtures", "x/../outside.bin"));
    assert(!fw_join_path(path, sizeof(path), "fixtures", "/outside.bin"));
    assert(!fw_join_path(path, sizeof(path), "fixtures", "C:\\outside.bin"));
    assert(!fw_join_path(path, 4, "fixtures", "app.bin"));
    assert(!fw_join_path(path, sizeof(path), "fixtures", "x//app.bin"));

    mkdir("fixtures", 0777);
    mkdir("fixtures/bootloader", 0777);
    uint8_t image[4097] = {0};
    image[0] = 0xe9;
    image[12] = 0x12;
    write_file("fixtures/app.bin", image, sizeof(image));
    write_file("fixtures/bootloader/bootloader.bin", image, 24);
    write_file("fixtures/partition-table.bin", "abcd", 4);
    manifest("{\"extra_esptool_args\":{\"chip\":\"esp32p4\"},\"flash_files\":{\"0x2000\":\"bootloader/bootloader.bin\",\"0x8000\":\"partition-table.bin\",\"0x10000\":\"app.bin\"}}");
    assert(fw_load_directory(&plan, "fixtures", error, sizeof(error)));
    assert(plan.from_manifest && plan.count == 3);
    assert(fw_validate(&plan, FW_TAB5_FLASH_SIZE, error, sizeof(error)));
    assert(fw_preflight(&plan, error, sizeof(error)));
    assert(fw_total_bytes(&plan) == 4125);

    plan.images[0].offset = 0x8000;
    assert(!fw_validate(&plan, FW_TAB5_FLASH_SIZE, error, sizeof(error))); // Sector overlap.
    plan.images[0].offset = 0x10001;
    assert(!fw_validate(&plan, FW_TAB5_FLASH_SIZE, error, sizeof(error))); // Alignment.
    plan.images[0].offset = FW_TAB5_FLASH_SIZE - 4096;
    assert(!fw_validate(&plan, FW_TAB5_FLASH_SIZE, error, sizeof(error))); // Rounded erase end.
    plan.images[0].offset = 0xfffff000;
    assert(!fw_validate(&plan, FW_TAB5_FLASH_SIZE, error, sizeof(error))); // Integer wrap.
    plan.images[0].offset = 0x10000;
    assert(!fw_validate(&plan, 65536, error, sizeof(error))); // Actual target smaller.
    for (size_t i = 0; i < plan.count; ++i) plan.images[i].selected = false;
    assert(!fw_validate(&plan, FW_TAB5_FLASH_SIZE, error, sizeof(error)));

    manifest("{\"flash_files\":{\"0x0\":\"../outside.bin\"}}");
    assert(!fw_load_directory(&plan, "fixtures", error, sizeof(error)) && plan.count == 0);
    manifest("{\"extra_esptool_args\":{\"chip\":\"esp32s3\"},\"flash_files\":{\"0x10000\":\"app.bin\"}}");
    assert(!fw_load_directory(&plan, "fixtures", error, sizeof(error)));
    manifest("{\"flash_files\":{\"0x10000\":\"missing.bin\"}}");
    assert(!fw_load_directory(&plan, "fixtures", error, sizeof(error)));
    manifest("{broken");
    assert(!fw_load_directory(&plan, "fixtures", error, sizeof(error))); // No guessing fallback.
    manifest("{\"flash_files\":{}}");
    assert(!fw_load_directory(&plan, "fixtures", error, sizeof(error)));
    manifest("{\"flash_files\":{\"0x10000\":\"app.bin\"}} trailing");
    assert(!fw_load_directory(&plan, "fixtures", error, sizeof(error)));
    unlink("fixtures/flasher_args.json");
    assert(fw_load_directory(&plan, "fixtures", error, sizeof(error)) && !plan.from_manifest);
    assert(!fw_validate(&plan, FW_TAB5_FLASH_SIZE, error, sizeof(error)));
    fw_apply_standard_layout(&plan);
    assert(fw_validate(&plan, FW_TAB5_FLASH_SIZE, error, sizeof(error)));

    assert(fw_load_single(&plan, "fixtures/app.bin", error, sizeof(error)));
    assert(plan.images[0].offset == FW_OFFSET_UNSET);
    plan.images[0].offset = 0;
    assert(fw_validate(&plan, FW_TAB5_FLASH_SIZE, error, sizeof(error)));
    image[12] = 9; // ESP32-S3 image on a P4 client.
    write_file("fixtures/app.bin", image, sizeof(image));
    assert(!fw_preflight(&plan, error, sizeof(error)));
    image[12] = 0x12;
    write_file("fixtures/app.bin", image, 24); // Changed after review.
    assert(!fw_preflight(&plan, error, sizeof(error)));
    assert(fw_load_single(&plan, "fixtures/app.bin", error, sizeof(error)));
    unlink("fixtures/app.bin");
    assert(!fw_preflight(&plan, error, sizeof(error))); // Removed SD/file.
    write_file("fixtures/empty.bin", "", 0);
    assert(!fw_load_single(&plan, "fixtures/empty.bin", error, sizeof(error)));
    unlink("fixtures/empty.bin");
    for (unsigned i = 0; i < FW_MAX_IMAGES + 1; ++i) {
        snprintf(path, sizeof(path), "fixtures/image-%u.bin", i);
        write_file(path, "data", 4);
    }
    assert(!fw_load_directory(&plan, "fixtures", error, sizeof(error)) && plan.count == 0);
    puts("PASS: offsets, paths, manifests, presets, sector overlap, capacity, chip and SD preflight");
    return 0;
}
