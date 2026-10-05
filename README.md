# ESP32 Matter Light Controller

## Build
```
. $IDF_PATH/export.sh && . $ESP_MATTER_PATH/export.sh   # ESP-IDF v5.2/5.4 + esp-matter release/v1.4
idf.py set-target esp32
idf.py build flash monitor
```
Delete any old `sdkconfig` after changing `sdkconfig.defaults`.

## Flash layout (4 MB, partitions.csv)
| Name | Offset | Size | Use |
|---|---|---|---|
| bootloader / table | 0x1000 / 0xC000 | | |
| nvs | 0x10000 | 64 KB | Matter + our keys (namespace `light_ctrl`) |
| otadata, phy_init | 0x20000, 0x22000 | | |
| ota_0 / ota_1 | 0x30000 / 0x200000 | 1.8 MB each | firmware + Matter OTA |
| fctry | 0x3D0000 | 24 KB | Matter factory data (DAC, discriminator, passcode) |

Production: `esp-matter-mfg-tool` -> `esptool.py write_flash 0x3D0000 <fctry.bin>`, then enable the
"Production identity" block in sdkconfig.defaults.

## Commissioning
Serial log prints the QR/manual code. Google Home -> Add device -> Matter. Dev builds use test VID 0xFFF1;
add the test VID/PID in the Google Home Developer Console (or use your CSA VID in production).
Hold the button 10 s to factory-reset the Matter fabric.
