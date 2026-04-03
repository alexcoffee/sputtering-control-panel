# LCD + Touch Wiring (Shared SPI Bus)

This project uses a 3.5" SPI LCD module with:
- LCD controller (ILI9488-style interface)
- Resistive touch controller (XPT2046-style interface)

The touch controller exposes 5 pins (`T_CLK`, `T_CS`, `T_DIN`, `T_DO`, `T_IRQ`), but only 3 *additional* GPIO mappings are needed because clock/data lines are shared with the LCD SPI bus.

## Why shared SPI

SPI supports multiple devices on the same bus:
- Shared bus lines: `SCK`, `MOSI`, `MISO`
- Separate chip select per device: LCD `CS`, touch `T_CS`
- Separate touch interrupt line: `T_IRQ`

This saves GPIOs and matches common module wiring.

## Wiring map used by `monitor`

### Shared SPI lines
- `T_CLK` + LCD `SCK` -> Pico `GP2` (`LCD_SPI_SCK`)
- `T_DIN` + LCD `MOSI/SDI` -> Pico `GP3` (`LCD_SDI`)
- `T_DO` + LCD `MISO/SDO` -> Pico `GP7` (`LCD_SDO`)

### Dedicated control lines
- LCD `CS` -> Pico `GP5` (`LCD_SPI_CSN`)
- Touch `T_CS` -> Pico `GP9` (`TOUCH_SPI_CSN`)
- Touch `T_IRQ` -> Pico `GP10` (`TOUCH_IRQ`)
- LCD `DC` -> Pico `GP4` (`LCD_COMMAND`)
- LCD `RST` -> Pico `GP6` (`LCD_RESET`)
- LCD `BL` -> Pico `GP8` (`LCD_BACKLIGHT`)

### Power
- LCD/touch `GND` -> Pico `GND`
- LCD/touch `VCC` -> Pico `3V3` (or module-required supply)

## Important notes

- Do **not** tie touch `T_CS` to LCD `CS`.
- Do **not** tie `T_IRQ` to any other signal.
- If LCD has no `MISO/SDO` pin, connect touch `T_DO` to `GP7` and leave LCD readback unused.
- Shared SPI means devices are connected to the same MCU SPI signals externally; it does not require LCD and touch chips to be internally shorted on the module PCB.
