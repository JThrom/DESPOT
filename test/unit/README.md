# DESPOT Host Unit Tests

Native (host `gcc`/`g++`) unit tests for the **hardware-independent logic** of
the DESPOT firmware, with gcov/gcovr code-coverage reporting. These run on a
normal Linux PC and require **no ESP-IDF toolchain and no hardware**.

## Why only "hardware-independent logic"?

DESPOT is ESP32-P4/S3 firmware. The vast majority of its code calls
hardware/RTOS APIs (LVGL display, BLE/USB HID, Wi-Fi, NVS, libssh2/mbedTLS,
FreeRTOS) that cannot execute on a host PC. Those paths are validated on-device.

This suite isolates the parts that are pure logic and exercises them against a
set of lightweight mock headers/stubs (`mocks/`) that satisfy the SDK symbols
the sources reference. The covered logic includes:

| Source | What is tested |
|--------|----------------|
| `main/terminal.cpp` | ANSI/VT escape, CSI, SGR, OSC, UTF-8 and DEC-graphics parsing; cursor motion; erase; insert/delete; alt-screen; scroll regions; DSR/CPR/DA replies; 256-color palette + RGB helpers; default-fg persistence |
| `main/hostname_mgr.cpp` | hostname validation/normalization, MAC-derived defaults, NVS persistence, `hostname` shell command |
| `main/usb_hid_host.cpp` | USB HID boot-keyboard keycode → ASCII mapping (shift/ctrl/arrows/keypad), report de-duplication |
| `main/ssh_client.cpp` | transport return-code decoding, FNV-1a trust-key hashing, tailnet IPv4 detection, host-key method-preference ordering, SHA256 fingerprint formatting, DSR fast-query filtering |
| `main/power_mgr.cpp` | idle low-power state machine: timeout boundary, backlight off/restore, brightness save, idempotent steps, unsupported-backlight path, ms-clock wraparound, runtime-configurable timeout, and the disabled (`0`) timeout (clock + backlight injected via hooks) |
| `main/battery_mgr.cpp` | STC8H battery register-block parsing (voltage/level/state byte offsets, level clamp, out-of-range state coercion), charging-state predicate, and the public init/read API driven against a host I2C mock (probe fail, null bus, read-before-init, cache priming) |
| NVS persistence contract | `test_persistence.cpp`: brightness (`devicecfg`/`blpct`) u8 save/load round-trip, missing-key-preserves-default (the brightness-reset bug's failure mode), namespace coexistence with idle-timeout/term-color keys, and overwrite |

Coverage numbers for big files (e.g. `terminal.cpp`, `ssh_client.cpp`) are
intentionally partial: the uncovered lines are the LVGL draw path and the live
SSH connection state machine, which need real hardware/network and are out of
scope for host tests.

## Requirements

- CMake ≥ 3.16, a C++17 compiler, `gcov` (ships with gcc).
- `gcovr` for the coverage report: `pip install --user gcovr`.
- Network access on first configure (GoogleTest is fetched via CMake
  `FetchContent`). Subsequent builds are offline.

## Running

```bash
test/unit/run.sh            # configure + build + test + coverage report
test/unit/run.sh --no-cov   # build + test only
```

Or manually:

```bash
cmake -S test/unit -B build/unit -DCMAKE_BUILD_TYPE=Debug
cmake --build build/unit -j
ctest --test-dir build/unit --output-on-failure
cmake --build build/unit --target coverage   # gcovr: HTML + Cobertura XML + text
```

Reports are written to `build/unit/coverage/`:
- `index.html`     – browsable HTML (gcovr `--html-details`)
- `coverage.xml`   – Cobertura XML (CI-friendly)
- `coverage.txt`   – plain-text summary

## Layout

```
test/unit/
  CMakeLists.txt        # build + coverage wiring
  run.sh                # one-shot build/test/coverage helper
  test_terminal.cpp     # terminal parser + color tests
  test_hostname.cpp     # hostname manager tests
  test_usb_hid.cpp      # USB HID keymap tests (white-box include of source)
  test_ssh_helpers.cpp  # SSH pure-helper tests (white-box include of source)
  test_power_mgr.cpp    # idle low-power state machine tests (injected fakes)
  test_battery_mgr.cpp  # STC8H battery parse + API tests (white-box + I2C mock)
  test_persistence.cpp  # NVS save/load contract (brightness + shared namespace)
  test_support.hpp      # shared test helpers
  mocks/                # mock headers + stub implementations for SDK symbols
    esp_*.h, nvs.h, lvgl.h, libssh2.h, freertos/*, usb/*, lwip/*, mbedtls/*, driver/*
    stubs_esp.cpp       # in-memory NVS, heap_caps, MAC, timer, esp_err
    stubs_freertos.cpp  # mutex/queue/task mocks
    stubs_lvgl.cpp      # LVGL canvas/draw-buffer stubs
    stubs_firmware.cpp  # cross-module + USB driver stubs (shell capture, etc.)
    stubs_ssh.cpp       # libssh2/mbedtls stubs (+ real base64), controllable hooks
```

## Notes for maintainers

- `test_usb_hid.cpp`, `test_ssh_helpers.cpp`, and `test_battery_mgr.cpp`
  `#include` the production `.cpp` directly (white-box) to reach file-local
  `static` functions. Those sources are therefore **not** part of the
  `firmware_under_test` library to avoid duplicate symbols.
- `test_battery_mgr.cpp` uses the host I2C mock (`mocks/driver/i2c_master.h` +
  `i2c_*` stubs in `stubs_esp.cpp`) which emulates the STC8H's single-byte
  register reads via a canned register table (`i2c_mock_set_reg`).
- The mock headers shadow real SDK headers via include-path ordering (the
  `mocks/` dir is first). They model only the symbols the tested sources touch;
  extend them as coverage grows.
- The mocks are deliberately excluded from the coverage report (filter
  `.*/main/.*`) so the numbers reflect firmware code only.
- These tests are independent of the ESP-IDF firmware build under `main/` and
  do not affect it.
```
