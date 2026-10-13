This is a layout example, not flashable firmware. Add your ESP32-P4 binaries:

- `bootloader/bootloader.bin`
- `partition_table/partition-table.bin`
- `application.bin`

Update `flasher_args.json` to match the original firmware build. Copy the entire
`demo` directory onto an SD card, open it on the host, and tap **Load this firmware
folder**. Missing binaries cause an error before any erase operation.
