#pragma once

#define BOARD_NAME        "M5StickC Plus2"
#define BOARD_TARGET      "esp32"

#define BOARD_PIN_HOLD          4

#define BOARD_PIN_QWIIC_SDA     26
#define BOARD_PIN_QWIIC_SCL     25

/* Internal I2C bus for IMU (MPU6886 / SH200Q) and AXP192 */
#define BOARD_PIN_IMU_SDA       21
#define BOARD_PIN_IMU_SCL       22

#define BOARD_PIN_BUTTON_A      37
#define BOARD_PIN_BUTTON_B      39
#define BOARD_PIN_BUTTON_BOOT   35
#define BOARD_PIN_BUTTON_PWR    35
#define BOARD_PIN_BATTERY_ADC   38

#define BOARD_PIN_LCD_MOSI      15
#define BOARD_PIN_LCD_SCLK      13
#define BOARD_PIN_LCD_DC        14
#define BOARD_PIN_LCD_CS        5
#define BOARD_PIN_LCD_RST       12
#define BOARD_PIN_LCD_BL        27

#define BOARD_LCD_H_RES         240
#define BOARD_LCD_V_RES         135
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ   (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_SWAP_XY       true
#define BOARD_LCD_MIRROR_X      false
#define BOARD_LCD_MIRROR_Y      true
#define BOARD_LCD_INVERT_COLOR  true
#define BOARD_LCD_GAP_X         40
#define BOARD_LCD_GAP_Y         52
#define BOARD_LCD_BL_ACTIVE_LOW false

#define BOARD_LCD_FG_COLOR      0x20FD
#define BOARD_LCD_BG_COLOR      0x0000

#define BOARD_PIN_SD_CS         -1
#define BOARD_PIN_SD_MISO       33      /* Unused (no SD card); SPI bus needs a valid GPIO for MISO */

/* Touch controller pins (not present on M5Stick, but touch HAL compiles) */
#define BOARD_PIN_TOUCH_SCL     -1
#define BOARD_PIN_TOUCH_SDA     -1
#define BOARD_PIN_TOUCH_RST     -1
#define BOARD_PIN_TOUCH_INT     -1
#define BOARD_TOUCH_I2C_ADDR    0x00
#define BOARD_TOUCH_I2C_PORT    0
#define BOARD_TOUCH_I2C_FREQ_HZ 0
#define BOARD_TOUCH_I2C_TIMEOUT 0

#define BOARD_PIN_BUZZER        2

/* CC1101 / SubGHz pins (not present, but resources.c references unconditionally) */
#define BOARD_PIN_CC1101_SCK    -1
#define BOARD_PIN_CC1101_CSN    -1
#define BOARD_PIN_CC1101_MISO   -1
#define BOARD_PIN_CC1101_MOSI   -1
#define BOARD_PIN_CC1101_GDO0   -1

/* nRF24 pin (not present, but nrf24_hw.c references unconditionally) */
#define BOARD_PIN_NRF24_CE      -1

/* Bruce firmware pinout for SPI modules */
#define BOARD_PIN_SPI_BRUCE_MOSI  32
#define BOARD_PIN_SPI_BRUCE_MISO  33
#define BOARD_PIN_SPI_BRUCE_SCK   0
#define BOARD_PIN_SPI_BRUCE_CS    26
#define BOARD_PIN_SPI_BRUCE_GDO0  25

/* IR pins */
#define BOARD_PIN_IR_TX         19
#define BOARD_PIN_IR_RX         -1

#define BOARD_HAS_TOUCH         0
#define BOARD_HAS_SD_CARD       0
#define BOARD_HAS_BLE           1
#define BOARD_HAS_IMU           1
#define BOARD_HAS_RGB_LED       0
#define BOARD_HAS_VIBRO         0
#define BOARD_HAS_SPEAKER       0
#define BOARD_HAS_IR            1
#define BOARD_HAS_IBUTTON       0
#define BOARD_HAS_RFID          0
#define BOARD_HAS_NFC           1
#define BOARD_PIN_NFC_SDA       26
#define BOARD_PIN_NFC_SCL       25
#define BOARD_PIN_NFC_IRQ       -1
#define BOARD_PIN_NFC_RST       -1
#define BOARD_NFC_I2C_PORT      I2C_NUM_0
#define BOARD_HAS_SUBGHZ        0
#define BOARD_HAS_USB           1

#define BOARD_HAS_AXP192        1
#define BOARD_BATTERY_ADC_RATIO 2.0f

#define BQ27220_ADDR                    0x55
#define BQ25896_CHARGE_LIMIT            1280
#define FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH (1300U)
