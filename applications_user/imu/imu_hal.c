/**
 * @file imu_hal.c
 * @brief IMU driver - MPU6886 / SH200Q auto-detect
 *
 * M5StickC Plus2 internal IMU on GPIO 21 (SDA), 22 (SCL), I2C_NUM_1.
 * Self-contained for FAP builds - no firmware component dependency.
 */

#include "imu_hal.h"

#include "furi.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c.h>
#include <esp_log.h>
#include <string.h>

#define TAG "IMU"

#define IMU_I2C_PORT        I2C_NUM_1
#define IMU_I2C_SDA_GPIO    21
#define IMU_I2C_SCL_GPIO    22
#define IMU_I2C_FREQ_HZ     400000
#define IMU_I2C_TIMEOUT_MS  50

#define MPU6886_ADDR        0x68
#define SH200Q_ADDR         0x6C

/* MPU6886 regs */
#define MPU6886_WHO_AM_I        0x75
#define MPU6886_WHO_AM_I_VAL    0x19
#define MPU6886_PWR_MGMT_1      0x6B
#define MPU6886_ACCEL_CONFIG    0x1C
#define MPU6886_GYRO_CONFIG     0x1B
#define MPU6886_CONFIG          0x1A
#define MPU6886_SMPLRT_DIV      0x19
#define MPU6886_ACCEL_XOUT_H    0x3B
#define MPU6886_TEMP_OUT_H      0x41
#define MPU6886_GYRO_XOUT_H     0x43
#define MPU6886_ACCEL_FS_8G     0x10
#define MPU6886_GYRO_FS_1000    0x10
#define MPU6886_PWR1_RESET      (1 << 7)

/* SH200Q regs */
#define SH200Q_WHO_AM_I         0x30
#define SH200Q_WHO_AM_I_VAL     0x31
#define SH200Q_SOFT_RESET       0x7C
#define SH200Q_ACC_CONFIG       0x10
#define SH200Q_GYRO_CONFIG      0x11
#define SH200Q_ACC_RANGE        0x0B
#define SH200Q_GYRO_RANGE       0x0C
#define SH200Q_ACC_DATA         0x02
#define SH200Q_GYRO_DATA        0x08
#define SH200Q_TEMP_DATA        0x20

static ImuType g_type = ImuTypeNone;
static bool g_init = false;

/* ─── I2C helpers ──────────────────────────────────────────────────── */

static esp_err_t imu_i2c_init(void) {
    const i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = IMU_I2C_SDA_GPIO,
        .scl_io_num = IMU_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = IMU_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_driver_install(IMU_I2C_PORT, conf.mode, 0, 0, 0);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = i2c_param_config(IMU_I2C_PORT, &conf);
    if(err != ESP_OK) return err;
    i2c_set_timeout(IMU_I2C_PORT, 0xFFFFF);
    return ESP_OK;
}

static bool i2c_write(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(IMU_I2C_PORT, addr, buf, 2, pdMS_TO_TICKS(IMU_I2C_TIMEOUT_MS)) == ESP_OK;
}

static bool i2c_read(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len) {
    return i2c_master_write_read_device(IMU_I2C_PORT, addr, &reg, 1, buf, len, pdMS_TO_TICKS(IMU_I2C_TIMEOUT_MS)) == ESP_OK;
}

static uint8_t i2c_read_reg(uint8_t addr, uint8_t reg) {
    uint8_t v = 0;
    i2c_read(addr, reg, &v, 1);
    return v;
}

/* ─── MPU6886 ─────────────────────────────────────────────────────── */

static bool mpu6886_probe(void) {
    return i2c_read_reg(MPU6886_ADDR, MPU6886_WHO_AM_I) == MPU6886_WHO_AM_I_VAL;
}

static bool mpu6886_init(void) {
    if(!i2c_write(MPU6886_ADDR, MPU6886_PWR_MGMT_1, MPU6886_PWR1_RESET)) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
    if(!i2c_write(MPU6886_ADDR, MPU6886_PWR_MGMT_1, 0x00)) return false; /* wake */
    vTaskDelay(pdMS_TO_TICKS(20));
    if(!i2c_write(MPU6886_ADDR, MPU6886_ACCEL_CONFIG, MPU6886_ACCEL_FS_8G)) return false;
    if(!i2c_write(MPU6886_ADDR, MPU6886_GYRO_CONFIG, MPU6886_GYRO_FS_1000)) return false;
    if(!i2c_write(MPU6886_ADDR, MPU6886_SMPLRT_DIV, 0)) return false;
    if(!i2c_write(MPU6886_ADDR, MPU6886_CONFIG, 0x01)) return false;
    return true;
}

static bool mpu6886_read(ImuData* d) {
    uint8_t raw[14];
    if(!i2c_read(MPU6886_ADDR, MPU6886_ACCEL_XOUT_H, raw, 14)) return false;
    int16_t ax = (int16_t)((raw[0] << 8) | raw[1]);
    int16_t ay = (int16_t)((raw[2] << 8) | raw[3]);
    int16_t az = (int16_t)((raw[4] << 8) | raw[5]);
    int16_t tmp = (int16_t)((raw[6] << 8) | raw[7]);
    int16_t gx = (int16_t)((raw[8] << 8) | raw[9]);
    int16_t gy = (int16_t)((raw[10] << 8) | raw[11]);
    int16_t gz = (int16_t)((raw[12] << 8) | raw[13]);
    d->accel_x = (float)ax / 4096.0f;
    d->accel_y = (float)ay / 4096.0f;
    d->accel_z = (float)az / 4096.0f;
    d->gyro_x = (float)gx / 32.8f;
    d->gyro_y = (float)gy / 32.8f;
    d->gyro_z = (float)gz / 32.8f;
    d->temperature = (float)tmp / 334.0f + 21.0f;
    return true;
}

/* ─── SH200Q ──────────────────────────────────────────────────────── */

static bool sh200q_probe(void) {
    uint8_t who = i2c_read_reg(SH200Q_ADDR, SH200Q_WHO_AM_I);
    return who == SH200Q_WHO_AM_I_VAL || who == 0x3D;
}

static bool sh200q_init(void) {
    if(!i2c_write(SH200Q_ADDR, SH200Q_SOFT_RESET, 0x00)) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    if(!i2c_write(SH200Q_ADDR, SH200Q_ACC_RANGE, 0x02)) return false;  /* ±8g */
    if(!i2c_write(SH200Q_ADDR, SH200Q_ACC_CONFIG, 0x02)) return false;
    if(!i2c_write(SH200Q_ADDR, SH200Q_GYRO_RANGE, 0x03)) return false; /* ±1000dps */
    if(!i2c_write(SH200Q_ADDR, SH200Q_GYRO_CONFIG, 0x02)) return false;
    if(!i2c_write(SH200Q_ADDR, 0x0D, 0x00)) return false; /* enable all */
    vTaskDelay(pdMS_TO_TICKS(10));
    return true;
}

static bool sh200q_read(ImuData* d) {
    uint8_t raw[6];
    if(!i2c_read(SH200Q_ADDR, SH200Q_ACC_DATA, raw, 6)) return false;
    int16_t ax = (int16_t)((raw[0] << 8) | raw[1]);
    int16_t ay = (int16_t)((raw[2] << 8) | raw[3]);
    int16_t az = (int16_t)((raw[4] << 8) | raw[5]);
    if(!i2c_read(SH200Q_ADDR, SH200Q_GYRO_DATA, raw, 6)) return false;
    int16_t gx = (int16_t)((raw[0] << 8) | raw[1]);
    int16_t gy = (int16_t)((raw[2] << 8) | raw[3]);
    int16_t gz = (int16_t)((raw[4] << 8) | raw[5]);
    uint8_t tr[2];
    int16_t tmp = 0;
    if(i2c_read(SH200Q_ADDR, SH200Q_TEMP_DATA, tr, 2))
        tmp = (int16_t)((tr[0] << 8) | tr[1]);
    d->accel_x = (float)ax / 4096.0f;
    d->accel_y = (float)ay / 4096.0f;
    d->accel_z = (float)az / 4096.0f;
    d->gyro_x = (float)gx / 32.8f;
    d->gyro_y = (float)gy / 32.8f;
    d->gyro_z = (float)gz / 32.8f;
    d->temperature = (float)tmp / 256.0f + 25.0f;
    return true;
}

/* ─── Public API ───────────────────────────────────────────────────── */

bool imu_init(void) {
    if(g_init) return true;
    if(imu_i2c_init() != ESP_OK) { FURI_LOG_E(TAG, "I2C init failed"); return false; }
    vTaskDelay(pdMS_TO_TICKS(10));
    if(mpu6886_probe()) {
        FURI_LOG_I(TAG, "Detected MPU6886");
        if(mpu6886_init()) { g_type = ImuTypeMPU6886; g_init = true; return true; }
    }
    if(sh200q_probe()) {
        FURI_LOG_I(TAG, "Detected SH200Q");
        if(sh200q_init()) { g_type = ImuTypeSH200Q; g_init = true; return true; }
    }
    i2c_driver_delete(IMU_I2C_PORT);
    FURI_LOG_W(TAG, "No IMU detected");
    return false;
}

bool imu_read(ImuData* data) {
    if(!g_init || !data) return false;
    memset(data, 0, sizeof(*data));
    switch(g_type) {
    case ImuTypeMPU6886: return mpu6886_read(data);
    case ImuTypeSH200Q:  return sh200q_read(data);
    default: return false;
    }
}

ImuType imu_get_type(void) { return g_type; }

const char* imu_get_type_name(void) {
    switch(g_type) {
    case ImuTypeMPU6886: return "MPU6886";
    case ImuTypeSH200Q:  return "SH200Q";
    default:             return "None";
    }
}

void imu_deinit(void) {
    if(!g_init) return;
    g_init = false; g_type = ImuTypeNone;
    i2c_driver_delete(IMU_I2C_PORT);
}
