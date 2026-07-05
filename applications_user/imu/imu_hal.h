#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ImuTypeNone = 0,
    ImuTypeMPU6886,
    ImuTypeSH200Q,
} ImuType;

typedef struct {
    float accel_x, accel_y, accel_z; /**< g */
    float gyro_x, gyro_y, gyro_z;   /**< deg/s */
    float temperature;               /**< °C */
} ImuData;

bool imu_init(void);
bool imu_read(ImuData* data);
ImuType imu_get_type(void);
const char* imu_get_type_name(void);
void imu_deinit(void);

#ifdef __cplusplus
}
#endif
