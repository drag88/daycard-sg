# Waveshare ESP32-S3-RLCD-4.2: hardware as built

Read from the chip with esptool, and from Waveshare's schematic and example code.

| | |
|---|---|
| Chip | ESP32-S3 (QFN56) rev v0.2, dual LX7 240 MHz + LP core, module ESP32-S3-WROOM-1-N16R8 |
| Memory | 16 MB quad flash (3.3 V), 8 MB PSRAM in package (Arduino option `PSRAM=opi`) |
| USB | native USB-Serial/JTAG, no bridge chip; enumerates as `/dev/cu.usbmodem*` (VID 0x303A PID 0x1001) |

| Part | Chip | Pins / address |
|---|---|---|
| Display | 4.2" reflective LCD, 400x300 landscape (300x400 native), 1-bit, ST7305 | SPI SCK 11, MOSI 12, DC 5, CS 40, RST 41, TE 6; driven via U8g2 with U8G2_R1; 24 MHz SPI, ~80 fps max |
| Temp/humidity | SHTC3 | I2C 0x70, SDA 13 / SCL 14 |
| RTC | PCF85063 | I2C 0x51; keeps UTC across restarts; full power-off needs a cell in the RTC BAT header |
| Speaker codec | ES8311 | I2C 0x18; I2S MCLK 16, BCLK 9, WS 45, DOUT 8; amplifier enable GPIO 46 (high only while playing) |
| Mics | ES7210, 2 mics (mic 1 = left channel is used) | I2C 0x40; I2S DIN 10; 30 dB gain |
| microSD | TF slot, FAT32 (cards up to 32 GB work as shipped) | SDMMC 1-bit: CLK 38, CMD 21, D0 39 |
| Buttons | BOOT GPIO 0, KEY GPIO 18 (active low), PWR via a push-button power controller | |
| Battery sense | 200K/100K divider (3:1) from B+ | GPIO 4 (ADC1 ch 3) |

Power: USB-C through a Schottky into the system rail; ETA6098 switching charger (green CHG LED, off when full);
18650 holder with its own S8261 + 8205 protection (an unprotected 65 mm cell fits best); red WRN LED lights when
the cell is reversed; TPS63020 buck-boost to 3.3 V; RT9193 LDO for the analog rail.

Codec register sequences in `audio.h` mirror Espressif's esp_codec_dev es8311.c / es7210.c (slave mode,
16 kHz, 16-bit, MCLK 4.096 MHz = 256 fs). Waveshare's own Arduino audio example uses esp_codec_dev with its own
I2C bus, which would clash with Arduino `Wire`, so this firmware writes the registers over `Wire` instead.

Schematic and docs: https://docs.waveshare.com/ESP32-S3-RLCD-4.2 (Resources page links the schematic PDF).
