/**
 * @file furi_hal_axp192.c
 * AXP192 PMU driver — ESP-IDF I2C (legacy driver API)
 *
 * The AXP192 is the power-management IC (PMU) on the M5StickC Plus2.
 * It sits on the same internal I2C bus (I2C_NUM_1, GPIO 21 SDA,
 * GPIO 22 SCL) as the MPU6886 / SH200Q IMU.
 *
 * Key registers (all 8-bit):
 *   0x00  — Power status (R):
 *           bit 4 = battery charging
 *           bit 5 = VBUS present
 *           bit 6 = battery present
 *   0x78/0x79 — Battery voltage ADC (12-bit, 1.1 mV/LSB)
 *   0x56/0x57 — VBUS voltage ADC (10-bit, 1.7 mV/LSB, pseudo 12-bit)
 *   0x7A/0x7B — Battery charge current (12-bit, 0.5 mA/LSB)
 *   0x7C/0x7D — Battery discharge current (12-bit, 0.5 mA/LSB)
 */

#include "furi_hal_axp192.h"
#include "boards/board.h"

#include <driver/i2c.h>
#include <esp_log.h>
#include <string.h>

static const char* TAG = "AXP192";

#define AXP_I2C_ADDR        0x34
#define AXP_I2C_TIMEOUT_MS  50

/* ─── Register map ─────────────────────────────────────────────────── */

#define AXP192_POWER_STATUS         0x00
#define AXP192_POWER_STATUS_CHARGING   (1 << 4)
#define AXP192_POWER_STATUS_VBUS       (1 << 5)
#define AXP192_POWER_STATUS_BATT_PRESENT (1 << 6)

#define AXP192_BATT_VOLTAGE_H       0x78
#define AXP192_BATT_VOLTAGE_L       0x79
#define AXP192_VBUS_VOLTAGE_H       0x56
#define AXP192_VBUS_VOLTAGE_L       0x57
#define AXP192_BATT_CHG_CURR_H      0x7A
#define AXP192_BATT_CHG_CURR_L      0x7B
#define AXP192_BATT_DISCHG_CURR_H   0x7C
#define AXP192_BATT_DISCHG_CURR_L   0x7D

#define AXP192_LDOIO_CTRL           0x92
#define AXP192_LDOIO_ENABLE         (1 << 2)   /* bit 2: LDOio output enable */

/* ─── State ────────────────────────────────────────────────────────── */

static bool axp192_present = false;
static bool axp192_init_done = false;

/* ─── I2C helpers ──────────────────────────────────────────────────── */

static bool axp192_i2c_read(i2c_port_t port, uint8_t reg, uint8_t* val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (AXP_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (AXP_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(AXP_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

static bool axp192_i2c_read_buf(i2c_port_t port, uint8_t reg, uint8_t* buf, size_t len) {
    if(len == 0) return false;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (AXP_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (AXP_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    for(size_t i = 0; i < len - 1; i++) {
        i2c_master_read_byte(cmd, &buf[i], I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, &buf[len - 1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(AXP_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

static bool axp192_i2c_write(i2c_port_t port, uint8_t reg, uint8_t val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (AXP_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(AXP_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

/**
 * Install I2C on a port/pin combo and probe for the AXP192.
 * Returns the port number on success, -1 on failure.
 */
static int axp192_try_bus(i2c_port_t port, int sda, int scl) {
    const i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    esp_err_t err = i2c_driver_install(port, conf.mode, 0, 0, 0);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return -1;
    err = i2c_param_config(port, &conf);
    if(err != ESP_OK) return -1;
    i2c_set_timeout(port, 0xFFFFF);

    /* Probe at 0x34 (AXP192/2101) */
    uint8_t status = 0;
    if(axp192_i2c_read(port, AXP192_POWER_STATUS, &status)) {
        ESP_LOGI(TAG, "AXP192 detected on I2C_NUM_%d (power_status=0x%02X)", (int)port, status);
        return (int)port;
    }

    /* Quick check for AXP2101 (address 0x35) and other PMUs */
    static const uint8_t pmu_addrs[] = {0x35, 0x36, 0x3C, 0x3D, 0x08, 0x09, 0x0A, 0x15, 0x75};
    for(size_t i = 0; i < sizeof(pmu_addrs); i++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (uint8_t)(pmu_addrs[i] << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t e = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(5));
        i2c_cmd_link_delete(cmd);
        if(e == ESP_OK) {
            ESP_LOGI(TAG, "  I2C_NUM_%d: device at 0x%02X", (int)port, pmu_addrs[i]);
        }
    }
    return -1;
}

/**
 * Read a 12-bit ADC value stored in two consecutive registers.
 *
 * Reg layout (AXP192 convention):
 *   reg_H : ADC[11:4]  (bits 7:0 → upper 8 bits)
 *   reg_L : ADC[3:0]   (bits 7:4 → lower 4 bits)
 */
static uint16_t axp192_read_12bit(uint8_t reg_h) {
    uint8_t buf[2];
    if(!axp192_i2c_read_buf(I2C_NUM_1, reg_h, buf, 2) &&
       !axp192_i2c_read_buf(I2C_NUM_0, reg_h, buf, 2)) {
        return 0;
    }
    return (uint16_t)(((uint16_t)buf[0] << 4) | (buf[1] >> 4));
}

/* ─── Public API ───────────────────────────────────────────────────── */

bool furi_hal_axp192_init(void) {
    if(axp192_init_done) return axp192_present;
    axp192_init_done = true;

#if BOARD_HAS_AXP192
    /* The AXP192 should be on the internal IMU bus (I2C_NUM_1, GPIO 21/22).
     * It shares this bus with the MPU6886/SH200Q IMU. If not found at the
     * standard address (0x34), the PMU may be wired differently on this
     * board revision, or may run in standalone mode without I2C access.
     * The ADC fallback in furi_hal_power will still detect USB presence. */
    if(axp192_try_bus(I2C_NUM_1, 21, 22) >= 0) {
        axp192_present = true;
    } else {
        ESP_LOGW(TAG, "AXP192 not found at 0x%02X on I2C_NUM_1 (21/22) — PMU may be standalone; ADC fallback active", AXP_I2C_ADDR);
    }
#else
    axp192_present = false;
#endif

    return axp192_present;
}

bool furi_hal_axp192_is_present(void) {
    return axp192_present;
}

bool furi_hal_axp192_is_charging(void) {
    if(!axp192_present) return false;
    uint8_t status = 0;
    if(!axp192_i2c_read(I2C_NUM_1, AXP192_POWER_STATUS, &status) &&
       !axp192_i2c_read(I2C_NUM_0, AXP192_POWER_STATUS, &status)) {
        return false;
    }
    return (status & AXP192_POWER_STATUS_CHARGING) != 0;
}

bool furi_hal_axp192_is_vbus_present(void) {
    if(!axp192_present) return false;
    uint8_t status = 0;
    if(!axp192_i2c_read(I2C_NUM_1, AXP192_POWER_STATUS, &status) &&
       !axp192_i2c_read(I2C_NUM_0, AXP192_POWER_STATUS, &status)) {
        return false;
    }
    return (status & AXP192_POWER_STATUS_VBUS) != 0;
}

uint16_t furi_hal_axp192_get_battery_voltage_mv(void) {
    if(!axp192_present) return 0;
    uint16_t raw = axp192_read_12bit(AXP192_BATT_VOLTAGE_H);
    /* 1.1 mV per LSB */
    return (uint16_t)((uint32_t)raw * 11u / 10u);
}

uint16_t furi_hal_axp192_get_vbus_voltage_mv(void) {
    if(!axp192_present) return 0;
    uint16_t raw = axp192_read_12bit(AXP192_VBUS_VOLTAGE_H);
    /* 1.7 mV per LSB → multiply by 17/10 */
    return (uint16_t)((uint32_t)raw * 17u / 10u);
}

uint16_t furi_hal_axp192_get_charge_current_ma(void) {
    if(!axp192_present) return 0;
    uint16_t raw = axp192_read_12bit(AXP192_BATT_CHG_CURR_H);
    /* 0.5 mA per LSB */
    return raw / 2u;
}

uint16_t furi_hal_axp192_get_discharge_current_ma(void) {
    if(!axp192_present) return 0;
    uint16_t raw = axp192_read_12bit(AXP192_BATT_DISCHG_CURR_H);
    /* 0.5 mA per LSB */
    return raw / 2u;
}

void furi_hal_axp192_enable_ldoio(bool enable) {
    if(!axp192_present) return;
    uint8_t val = 0;
    /* Read-modify-write: preserve other LDOio control bits */
    if(axp192_i2c_read(I2C_NUM_1, AXP192_LDOIO_CTRL, &val) ||
       axp192_i2c_read(I2C_NUM_0, AXP192_LDOIO_CTRL, &val)) {
        if(enable) {
            val |= AXP192_LDOIO_ENABLE;
        } else {
            val &= ~AXP192_LDOIO_ENABLE;
        }
        axp192_i2c_write(I2C_NUM_1, AXP192_LDOIO_CTRL, val);
    }
}
