/**
 * @file furi_hal_axp192.h
 * AXP192 PMU driver — M5StickC Plus2 power management
 *
 * I2C address: 0x34
 * Bus: I2C_NUM_1 (internal IMU bus, GPIO 21 SDA / 22 SCL)
 *
 * Provides battery voltage, charging status, and USB presence detection
 * so furi_hal_power can report real PMU readings instead of relying on
 * the ESP32's noisy general-purpose ADC.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Init I2C_NUM_1, probe for the AXP192, and prepare for reads. */
bool furi_hal_axp192_init(void);

/** True if the AXP192 was found on the bus. */
bool furi_hal_axp192_is_present(void);

/** Read power-status register bit 4 — battery is actively charging. */
bool furi_hal_axp192_is_charging(void);

/** Read power-status register bit 5 — USB VBUS is present. */
bool furi_hal_axp192_is_vbus_present(void);

/**
 * Battery voltage in millivolts.
 * 12-bit ADC, 1.1 mV/LSB → range 0 – 4504 mV.
 */
uint16_t furi_hal_axp192_get_battery_voltage_mv(void);

/**
 * USB VBUS voltage in millivolts.
 * 10-bit ADC, 1.7 mV/LSB → range 0 – 6963 mV.
 */
uint16_t furi_hal_axp192_get_vbus_voltage_mv(void);

/**
 * Battery charge current in mA (positive = charging, 0 = not charging).
 * 12-bit ADC, 0.5 mA/LSB.
 */
uint16_t furi_hal_axp192_get_charge_current_ma(void);

/**
 * Battery discharge current in mA (positive = discharging).
 * 12-bit ADC, 0.5 mA/LSB.
 */
uint16_t furi_hal_axp192_get_discharge_current_ma(void);

#ifdef __cplusplus
}
#endif
