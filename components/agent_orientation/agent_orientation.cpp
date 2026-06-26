#include <math.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "agent_orientation.hpp"

static const char *TAG = "agent_orientation";

#define QMI8658_ADDR_PRIMARY 0x6B
#define QMI8658_ADDR_SECONDARY 0x6A

#define QMI8658_REG_WHO_AM_I 0x00
#define QMI8658_REG_REVISION 0x01
#define QMI8658_REG_CTRL1 0x02
#define QMI8658_REG_CTRL2 0x03
#define QMI8658_REG_CTRL3 0x04
#define QMI8658_REG_CTRL7 0x08
#define QMI8658_REG_RESET 0x60
#define QMI8658_REG_ACC_X_L 0x35
#define QMI8658_REG_ACC_X_H 0x36
#define QMI8658_REG_ACC_Y_L 0x37
#define QMI8658_REG_ACC_Y_H 0x38
#define QMI8658_REG_ACC_Z_L 0x39
#define QMI8658_REG_ACC_Z_H 0x3A

#define QMI8658_WHO_AM_I_VALUE 0x05
#define QMI8658_I2C_FREQ_HZ 400000
#define QMI8658_ACC_2G_SENSITIVITY 16384.0f

#define ORIENTATION_STEP_DEG 90
#define ORIENTATION_SAMPLE_MS 100
#define ORIENTATION_RETRY_MS 2000
#define ORIENTATION_MIN_XY_G 0.20f
#define ORIENTATION_FILTER_ALPHA 0.25f
#define ORIENTATION_DISPLAY_OFFSET_DEG 0
#define ORIENTATION_ROTATION_SIGN (-1)

static i2c_master_dev_handle_t imu_dev = NULL;
static bool imu_ready = false;
static bool filter_ready = false;
static bool probe_warning_logged = false;
static bool orientation_task_started = false;
static uint32_t next_retry_ms = 0;
static int current_angle_deg = 0;
static int locked_angle_deg = 0;
static bool orientation_locked = false;
static float filtered_x_g = 0.0f;
static float filtered_y_g = 0.0f;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static int normalize_degrees(int angle)
{
    angle %= 360;
    if (angle < 0) angle += 360;
    return angle;
}

static int round_degrees(float angle, int step)
{
    return normalize_degrees((int)floorf((angle + (float)step / 2.0f) / (float)step) * step);
}

static esp_err_t qmi_read_reg(uint8_t reg, uint8_t *value)
{
    if (!imu_dev || !value) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(imu_dev, &reg, 1, value, 1, pdMS_TO_TICKS(100));
}

static esp_err_t qmi_write_reg(uint8_t reg, uint8_t value)
{
    if (!imu_dev) return ESP_ERR_INVALID_STATE;
    uint8_t data[2] = { reg, value };
    return i2c_master_transmit(imu_dev, data, sizeof(data), pdMS_TO_TICKS(100));
}

static esp_err_t qmi_read_i16(uint8_t low_reg, uint8_t high_reg, int16_t *value)
{
    uint8_t low = 0;
    uint8_t high = 0;
    esp_err_t ret = qmi_read_reg(low_reg, &low);
    if (ret != ESP_OK) return ret;
    ret = qmi_read_reg(high_reg, &high);
    if (ret != ESP_OK) return ret;

    *value = (int16_t)(((uint16_t)high << 8) | low);
    return ESP_OK;
}

static esp_err_t qmi_read_accel(float *x_g, float *y_g, float *z_g)
{
    int16_t raw_x = 0;
    int16_t raw_y = 0;
    int16_t raw_z = 0;

    esp_err_t ret = qmi_read_i16(QMI8658_REG_ACC_X_L, QMI8658_REG_ACC_X_H, &raw_x);
    if (ret != ESP_OK) return ret;
    ret = qmi_read_i16(QMI8658_REG_ACC_Y_L, QMI8658_REG_ACC_Y_H, &raw_y);
    if (ret != ESP_OK) return ret;
    ret = qmi_read_i16(QMI8658_REG_ACC_Z_L, QMI8658_REG_ACC_Z_H, &raw_z);
    if (ret != ESP_OK) return ret;

    *x_g = (float)raw_x / QMI8658_ACC_2G_SENSITIVITY;
    *y_g = (float)raw_y / QMI8658_ACC_2G_SENSITIVITY;
    *z_g = (float)raw_z / QMI8658_ACC_2G_SENSITIVITY;
    return ESP_OK;
}

static esp_err_t qmi_configure(void)
{
    esp_err_t ret = qmi_write_reg(QMI8658_REG_RESET, 0xB0);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));

    ret = qmi_write_reg(QMI8658_REG_CTRL7, 0x00);
    if (ret != ESP_OK) return ret;

    ret = qmi_write_reg(QMI8658_REG_CTRL1, 0x40); // Little-endian data, address auto-increment enabled.
    if (ret != ESP_OK) return ret;

    ret = qmi_write_reg(QMI8658_REG_CTRL2, 0x07); // Accelerometer: +/-2g, 62.5Hz ODR.
    if (ret != ESP_OK) return ret;

    ret = qmi_write_reg(QMI8658_REG_CTRL3, 0x00); // Gyro disabled, default scale/ODR.
    if (ret != ESP_OK) return ret;

    ret = qmi_write_reg(QMI8658_REG_CTRL7, 0x01); // Enable accelerometer only.
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static void agent_orientation_sample(void)
{
    uint32_t now = now_ms();
    if (!imu_ready) {
        if ((int32_t)(now - next_retry_ms) >= 0) {
            agent_orientation_init();
        }
        return;
    }

    float x_g = 0.0f;
    float y_g = 0.0f;
    float z_g = 0.0f;
    esp_err_t ret = qmi_read_accel(&x_g, &y_g, &z_g);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 read failed: %s", esp_err_to_name(ret));
        imu_ready = false;
        filter_ready = false;
        if (imu_dev) {
            i2c_master_bus_rm_device(imu_dev);
            imu_dev = NULL;
        }
        next_retry_ms = now + ORIENTATION_RETRY_MS;
        return;
    }

    if (!filter_ready) {
        filtered_x_g = x_g;
        filtered_y_g = y_g;
        filter_ready = true;
    } else {
        filtered_x_g += (x_g - filtered_x_g) * ORIENTATION_FILTER_ALPHA;
        filtered_y_g += (y_g - filtered_y_g) * ORIENTATION_FILTER_ALPHA;
    }

    float xy_magnitude = sqrtf(filtered_x_g * filtered_x_g + filtered_y_g * filtered_y_g);
    if (xy_magnitude < ORIENTATION_MIN_XY_G) return;

    float gravity_angle = atan2f(filtered_y_g, filtered_x_g) * (180.0f / 3.14159265f);
    float display_angle = (float)ORIENTATION_DISPLAY_OFFSET_DEG +
                          (float)ORIENTATION_ROTATION_SIGN * gravity_angle;
    current_angle_deg = round_degrees(display_angle, ORIENTATION_STEP_DEG);
}

static void agent_orientation_task(void *arg)
{
    (void)arg;
    while (true) {
        agent_orientation_sample();
        vTaskDelay(pdMS_TO_TICKS(ORIENTATION_SAMPLE_MS));
    }
}

static bool qmi_try_address(i2c_master_bus_handle_t bus, uint8_t address)
{
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = address;
    dev_cfg.scl_speed_hz = QMI8658_I2C_FREQ_HZ;

    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &imu_dev);
    if (ret != ESP_OK) return false;

    uint8_t who_am_i = 0;
    ret = qmi_read_reg(QMI8658_REG_WHO_AM_I, &who_am_i);
    if (ret != ESP_OK || who_am_i != QMI8658_WHO_AM_I_VALUE) {
        i2c_master_bus_rm_device(imu_dev);
        imu_dev = NULL;
        return false;
    }

    ret = qmi_configure();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 setup failed at 0x%02X: %s", address, esp_err_to_name(ret));
        i2c_master_bus_rm_device(imu_dev);
        imu_dev = NULL;
        return false;
    }

    uint8_t revision = 0;
    qmi_read_reg(QMI8658_REG_REVISION, &revision);
    ESP_LOGI(TAG, "QMI8658 ready at 0x%02X, revision 0x%02X", address, revision);
    return true;
}

bool agent_orientation_init(void)
{
    if (imu_ready) return true;

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        if (!probe_warning_logged) ESP_LOGW(TAG, "I2C bus is not available for QMI8658");
        probe_warning_logged = true;
        return false;
    }

    imu_ready = qmi_try_address(bus, QMI8658_ADDR_PRIMARY) ||
                qmi_try_address(bus, QMI8658_ADDR_SECONDARY);

    if (!imu_ready) {
        if (!probe_warning_logged) {
            ESP_LOGW(TAG, "QMI8658 accelerometer not found");
            probe_warning_logged = true;
        }
        next_retry_ms = now_ms() + ORIENTATION_RETRY_MS;
        return false;
    }

    probe_warning_logged = false;
    filter_ready = false;

    if (!orientation_task_started) {
        BaseType_t ok = xTaskCreate(agent_orientation_task, "agent_orientation", 4096, NULL, 4, NULL);
        orientation_task_started = ok == pdPASS;
        if (!orientation_task_started) {
            ESP_LOGW(TAG, "Failed to start orientation task");
        }
    }

    return true;
}

void agent_orientation_timer_update(void)
{
}

int agent_orientation_get_angle_deg(void)
{
    return orientation_locked ? locked_angle_deg : current_angle_deg;
}

bool agent_orientation_is_locked(void)
{
    return orientation_locked;
}

void agent_orientation_set_locked(bool locked)
{
    if (locked && !orientation_locked) {
        locked_angle_deg = current_angle_deg;
    }
    orientation_locked = locked;
}
