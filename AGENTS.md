# Working on Tab5-Burner

## Project purpose

This is standalone ESP-IDF C firmware for an M5Stack Tab5 (ESP32-P4). The host
Tab5 browses firmware on its microSD card and flashes a companion Tab5 through
**host USB-A -> client USB-C programming port**. LVGL 9 provides the touch UI.
Read `README.md` for the user workflow, SD layout examples and recovery steps.
This is not an Arduino project or a Brookesia application package.

## Source map

| Path | Responsibility |
| --- | --- |
| `main/main.c` | NVS, display/touch, SD and USB startup; landscape rotation |
| `main/firmware.c`, `firmware.h` | Portable image/manifest loading, offsets, flash-plan validation and SD preflight |
| `main/burner.c`, `burner.h` | USB host setup, FreeRTOS flashing worker, status, cancellation and MD5 verification |
| `main/usb_loader_port.c`, `usb_loader_port.h` | Custom CDC transport implementing the serial flasher port operations |
| `main/storage.c`, `storage.h` | Raw SDMMC/LDO lifecycle, FAT/MSC ownership, USB-C device mode and write draining |
| `main/storage_gate.c`, `storage_gate.h` | Portable SD ownership/lease policy, serialized by the storage service |
| `main/power_service.c`, `power_service.h` | INA226 sampling, charger IO expander, queued power controls and cached telemetry |
| `main/battery_math.c`, `battery_math.h` | Portable raw sensor conversions and estimated battery percentage |
| `main/ui.c`, `ui.h` | Burner, Battery and USB drive pages, navigation, confirmation and progress |
| `main/idf_component.yml`, `dependencies.lock` | Managed dependency constraints and resolved versions |
| `sdkconfig`, `sdkconfig.defaults` | Full project configuration and reproducible board defaults |
| `partitions.csv` | Host firmware partition table; distinct from the client's flash plan |
| `tests/test_firmware.c`, `tests/run_host_tests.py` | Tests of the production firmware model |
| `tests/test_host_services.c` | Battery conversion and exclusive SD ownership tests; run by the same host-test script |
| `tests/host/`, `tests/render_ui.py` | Production UI rendered with real LVGL and mocked board/worker functions |
| `examples/sdcard/firmware/demo/` | Example client manifest; users must supply actual binaries |

`build/` and `managed_components/` are generated/ignored. Inspect managed source
to verify APIs, but make dependency changes through the manifest and Component
Manager rather than editing downloaded components. Keep downloaded references,
temporary files and test output under `build/`.

## Hardware details that must be preserved

- Target the ESP32-P4, not the onboard ESP32-C6 wireless module.
- Current defaults select P4 silicon below v3, 16 MB DIO flash at 40 MHz and
  PSRAM at 200 MHz. ESP-IDF detects the installed 32 MB PSRAM. Newer silicon
  requires a deliberate revision/configuration change; do not assume it is
  interchangeable with the configured revision.
- Use the managed `espressif/m5stack_tab5` BSP for display/touch detection,
  SDMMC pins and power, and USB power through the IO expander.
- USB-A uses **USB controller 0 and the dedicated high-speed PHY**. The host
  installation explicitly sets `peripheral_map = 1U << 0`.
- USB-C programming/console uses serial/JTAG on GPIO24/25. Do not replace the
  verified USB-A mapping with the BSP header's GPIO19/20 macros or controller 1.
- USB drive mode temporarily routes the full-speed OTG controller to the USB-C
  pads with `usb_wrap_ll_phy_select(&USB_WRAP, 0)`. IDF host controller indices
  and TinyUSB port enums differ: TinyUSB full-speed port 0 is IDF's FS controller
  1; IDF host controller 0 remains the dedicated HS USB-A port. Restore the
  serial/JTAG pad routing on exit; do not put TinyUSB on the host's HS port.
- The panel is 720 x 1280, rotated 90 degrees for a 1280 x 720 interface. Draw
  buffers use PSRAM. SD uses 4-bit SDMMC on GPIO39-44 at `/sdcard`.
- Failed SD mounting must not format the user's card. Keep
  `CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL` disabled.
- Battery hardware is INA226 `0x41` with a 5 milliohm shunt. Expander `0x44` P7
  enables charging, P5 enables quick charge when low, and P6 is active-high
  charging status. Preserve these polarities and the board charger's limits.

The managed BSP is pinned to 1.3.2 and serial flasher to 2.1.0. The flasher uses
the context-based v2 API; verify signatures in the resolved component before
changing loader calls. The built-in UART and USB flasher ports are disabled
because this application supplies its own USB transport.

## Build

Run from the repository root in an activated ESP-IDF shell. ESP-IDF **6.1.0** was
build-tested; the manifest requires `>=6.1,<6.2` for the P4 full-speed PHY/HAL
support. Use Python from the selected IDF environment and the matching P4
compiler, CMake and Ninja.

```sh
idf.py --version
idf.py -B build/tab5 reconfigure
idf.py -B build/tab5 build size
```

Component Manager fetches dependencies during configuration and updates
`dependencies.lock` when resolution changes. The project already selects
`esp32p4`; do not run `set-target` just to build, since it can replace the
existing configuration. Do not mix IDF versions in an existing CMake cache;
use a separate directory under `build/` when testing another version.

Expected outputs include `build/tab5/tab5_burner.bin`, the bootloader and
partition table BINs, `flasher_args.json`, and `compile_commands.json`. For
editor diagnostics, use the compile commands from the directory actually built.

### Windows environment used in this workspace

The IDE points to `C:\esp\v6.1\esp-idf`. The tools are under
`C:\Espressif\tools`, rather than the usual user `.espressif` directory. If the
normal activated shell is unavailable, this PowerShell setup was used to build:

```powershell
$env:IDF_PATH = 'C:\esp\v6.1\esp-idf'
$env:IDF_TOOLS_PATH = 'C:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v6.1\venv'
$env:ESP_IDF_VERSION = '6.1.0'
$env:PYTHONUTF8 = '1'
$env:ESP_ROM_ELF_DIR = 'C:\Espressif\tools\esp-rom-elfs\20241011'
$env:PATH = "$env:IDF_PYTHON_ENV_PATH\Scripts;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;$env:PATH"
python "$env:IDF_PATH\tools\idf.py" -B build/tab5 build size
```

These are local installation paths, not portable project requirements. Verify
them before use on another machine. `build/run-idf.ps1` may exist as a local
helper, but it is ignored and must not be required for a fresh checkout. If IDF
activation reports missing tools, first check the selected IDF, tools directory
and Python environment instead of immediately reinstalling globally.

## Validation

For firmware changes, run the ESP-IDF build. For changes to image loading,
manifests, offsets or preflight, also run the C model tests after fetching the
managed dependencies:

```sh
python tests/run_host_tests.py --cc gcc
```

That command needs a native POSIX C toolchain. On Windows, use Emscripten/Node;
the installed SDK paths used here are:

```powershell
$env:EM_CONFIG = 'C:/tools/emsdk/.emscripten'
python tests/run_host_tests.py --cc C:/tools/emsdk/upstream/emscripten/emcc.exe --node C:/tools/emsdk/node/24.19.0_64bit/node.exe
```

For UI changes, activate the Emscripten SDK with CMake/Ninja/Node in `PATH` and
run:

```sh
python tests/render_ui.py
```

Alternatively, supply `--emcmake`, `--cmake` and `--node` with executable paths.
The renderer checks layout bounds, offset editing and flash controls and writes
PNGs to `build/ui-render/`. Inspect the relevant screenshots for text contrast,
clipping, modal placement and touch target sizes. It compiles the real `ui.c`;
do not replace it with a separate mock UI to make the checks pass.

For documentation-only changes, check referenced paths/commands and run
`git diff --check`; a firmware rebuild is unnecessary. Report which checks ran
and distinguish simulated behavior from hardware validation. Physical
Tab5-to-Tab5 flashing has not yet been verified in this project.

## Implementation contracts

- Keep the firmware model independent of ESP-IDF/FreeRTOS/LVGL so the same code
  runs in the host tests. Use bounded paths and buffers and explicit errors.
- Separate firmware images belong to one directory. Prefer the original
  `flasher_args.json` and preserve relative subdirectories and exact offsets.
  Reject absolute paths and parent traversal. A broken manifest must fail;
  it must not trigger fallback to guessed addresses.
- Without a manifest, leave offsets unset until the user chooses a layout or
  edits them. Do not infer that an arbitrary BIN is merged. Standard P4
  bootloader/partition/application addresses are `0x2000`, `0x8000`, `0x10000`;
  custom/OTA layouts can differ. A merged image at zero is an explicit choice.
- Validate 4 KB alignment, rounded erase-sector overlap and target capacity
  before erasing. Preflight all selected files before the first erase and
  retain the ESP32-P4 and security checks. Secured clients require a separate
  provisioning workflow.
- Stream files in bounded chunks; do not load whole images into RAM. Preserve
  final-block padding and the matching MD5 length. Report success only after
  every selected image is verified. Do not introduce implicit whole-chip erase.
- The worker owns a copied plan for the entire job. Keep editing controls
  disabled while it runs and prevent another job until teardown completes.
- All SD users must obtain a `storage_claim_local()` lease and release it when
  done. A flash job holds its lease through teardown. Switching to USB or
  remounting requires zero leases. Use the storage service instead of
  `bsp_sdcard_mount()`/`bsp_sdcard_unmount()`; the service retains the raw card
  while TinyUSB manages FAT mounting. Never allow PC and app access concurrently.
- Keep automatic MSC mount-on-connect/disconnect disabled. A PC eject event
  does not grant the application access: explicit Stop USB drive must complete
  before local access resumes. Clear stale plans after ownership changes.
- Disconnect USB and drain deferred writes on the TinyUSB task before removing
  the medium or stopping that task. Mount setters in esp_tinyusb 2.4.0 do not
  propagate all errors; verify completion events and VFS readiness. Fail closed
  and retain SD reservation if teardown fails. Never automatically format.
- Power controls run on their worker and telemetry is published through a short
  cache lock; do not block LVGL on I2C transactions. USB-A power changes share
  the burner's job lock. Pause USB-C quick charge during MSC, and restore it
  only after device teardown. Battery percentage is an explicitly labeled
  voltage estimate, not proof of battery presence or a fuel gauge.
- LVGL callbacks/timers run in the LVGL context. Calls from other tasks require
  `bsp_display_lock()`/`bsp_display_unlock()`. The flashing worker reports status;
  it must not manipulate LVGL objects directly.
- USB CDC callbacks flag disconnect/errors. The worker closes the CDC device before
  freeing RX storage; never delete a stream buffer from a disconnect callback
  while another task may be reading it. Reads must handle fragmented packets
  and finite timeouts.
- Cancellation or disconnect can leave the client partially written. Preserve
  clear recovery messages and do not reset/boot a failed or cancelled client.

## Configuration and device work

Keep intentional hardware changes consistent in `sdkconfig.defaults` and the
full `sdkconfig`; regenerate with ESP-IDF rather than fabricating generated
values. Keep dependency changes and the lock file together. Preserve unrelated
working-tree changes, including existing IDE configuration.

When the user's task authorizes installing firmware on a real host, use its
identified programming port:

```sh
idf.py -B build/tab5 -p COM_PORT flash monitor
```

This flashes the **host burner**, not the companion. Companion flashing occurs
through the application's SD selection and confirmation flow. Do not flash a
physical device merely to validate a code/documentation change. Hardware checks
require two powered Tab5 devices: verify SD/touch, flash a known P4 set and a
merged BIN, confirm booting, and exercise cancellation/unplug/reconnect recovery.
For the new pages also check battery telemetry/controls on real hardware and
copy/checksum files through USB-C, eject, remount and repeat mode transitions.
Host tests and UI mocks do not validate physical USB enumeration or charging.
