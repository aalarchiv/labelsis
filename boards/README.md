# Board profiles

Each subdirectory here is a board profile -- a bundle of board-specific
configuration the build picks at configure time. Pass a profile name on
the `idf.py` command line:

```sh
idf.py -D BOARD=lolin_s2_mini set-target esp32s2
idf.py build flash monitor
```

The default is `devkitc_s3`.

## What a profile contains

```
boards/<name>/
  board.h              C macros consumed by main/main.c (LED type/pin,
                       reset GPIO, friendly board name).
  sdkconfig.defaults   IDF Kconfig overlay -- flash size, partition
                       table path, PSRAM, Bluetooth.
  partitions.csv       Flash layout sized for the board's flash chip.
                       (esp-idf reads this via
                       CONFIG_PARTITION_TABLE_CUSTOM_FILENAME, which
                       the per-board sdkconfig.defaults sets.)
  target               IDF target name on a single line (e.g.
                       "esp32s2" / "esp32s3"). Read by
                       scripts/build-release.sh to drive set-target
                       unattended; manual builds infer the same from
                       the user's `idf.py set-target` invocation.
```

## Switching profiles after first config

`idf.py` only consumes `SDKCONFIG_DEFAULTS` on initial configure. Once
`sdkconfig` exists, the file is the source of truth. To switch board:

```sh
rm -rf build sdkconfig
idf.py -D BOARD=<new> set-target <s2|s3>
idf.py build
```

(Same gotcha as `idf.py set-target`.)

## Adding a new board

1. Copy an existing profile that's closest to your hardware.
2. Edit `board.h`: friendly name, LED type/pin, reset GPIO.
3. Edit `sdkconfig.defaults`: flash size, partition path, PSRAM,
   Bluetooth -- whatever differs from the closest sibling.
4. Edit `partitions.csv`: factory app size to fit the flash. We don't
   ship a SPIFFS / second-NVS partition; SPA assets are baked into
   `.rodata` via `EMBED_FILES` and the runtime only opens the
   default `nvs` partition for Wi-Fi credentials.
5. Build with `idf.py -D BOARD=<your_name> set-target <target>`.

## Currently shipped profiles

| Name             | SoC        | Flash  | PSRAM | LED                    |
|------------------|------------|--------|-------|------------------------|
| `devkitc_s3`     | ESP32-S3   | 16 MB  | 8 MB octal | WS2812 RGB on GPIO 48 |
| `lolin_s2_mini`  | ESP32-S2FH4 | 4 MB  | none  | plain on GPIO 15       |
