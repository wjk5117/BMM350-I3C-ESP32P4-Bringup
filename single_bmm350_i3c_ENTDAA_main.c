/*
 * Single BMM350 EVB + ENTDAA example for ESP32-P4
 *
 * This example demonstrates a practical "ENTDAA-style" bring-up flow for a
 * single BMM350 connected over I3C:
 *
 *   1. Create I3C bus
 *   2. Send an initial broadcast CCC preamble
 *   3. Discover the device using ENTDAA
 *   4. Probe CHIP_ID over raw I3C
 *   5. Bind Bosch SensorAPI
 *   6. Perform minimal sensor initialization
 *   7. Read magnetic field data continuously
 *
 * Notes:
 * - ENTDAA = Enter Dynamic Address Assignment
 * - In this example, the host automatically discovers the device and assigns
 *   a dynamic address during enumeration.
 * - This example is intended for single-device validation. For multi-device
 *   use, ENTDAA requires each device to expose a unique identity / PID.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_rom_sys.h"

#include <math.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>

#include "driver/i3c_master.h"
#include "bmm350.h"
#include "bmm350_port_esp32p4_i3c.h"

static const char *TAG = "BMM350_ENTDAA_1X";

/* ------------------------- User Configuration ------------------------- */
#define BMM350_I3C_SDA_GPIO        33
#define BMM350_I3C_SCL_GPIO        32

#define BMM350_I3C_OD_FREQ_HZ      400000
#define BMM350_I3C_PP_FREQ_HZ      1000000

/* Expected number of ENTDAA-discoverable devices in this example */
#define BMM350_EXPECTED_DEVICE_NUM 1

#define BMM350_READ_PERIOD_MS      20
#define BMM350_PERF_LOG_EVERY_N    50
/* -------------------------------------------------------------------- */

static struct bmm350_dev g_bmm_dev;
static bmm350_i3c_port_t g_bmm_port;
static i3c_master_bus_handle_t g_bus = NULL;

static inline int64_t now_us(void)
{
    return esp_timer_get_time();
}

/**
 * @brief Read CHIP_ID (register 0x00) via raw I3C handle.
 *
 * BMM350 I3C register reads return:
 *   [dummy][dummy][real_data]
 */
static esp_err_t probe_bmm350_chipid_i3c_handle(i3c_master_i3c_device_handle_t dev)
{
    uint8_t reg = 0x00;   /* CHIP_ID */
    uint8_t rx[3] = {0};  /* 2 dummy bytes + 1 real byte */

    esp_err_t err = i3c_master_i3c_device_transmit_receive(
        dev, &reg, 1, rx, sizeof(rx), -1
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CHIP_ID read transaction failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Raw I3C CHIP_ID bytes: %02X %02X %02X", rx[0], rx[1], rx[2]);

    if (rx[2] != BMM350_CHIP_ID) {
        ESP_LOGE(TAG, "Unexpected CHIP_ID: 0x%02X (expected 0x%02X)", rx[2], BMM350_CHIP_ID);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Sign-extend a 21-bit signed value packed into 24 bits.
 */
static int32_t sign_extend_21(uint32_t v)
{
    v &= 0x1FFFFF;
    if (v & 0x100000) {
        v |= 0xFFE00000;
    }
    return (int32_t)v;
}

/**
 * @brief Read raw magnetic field and temperature registers directly.
 *
 * Starting at 0x31, the BMM350 returns:
 *   [dummy][dummy][12 bytes payload]
 *
 * Payload order:
 *   X(3 bytes), Y(3 bytes), Z(3 bytes), T(3 bytes)
 */
static esp_err_t read_bmm350_raw_direct(i3c_master_i3c_device_handle_t dev,
                                        int32_t *x, int32_t *y, int32_t *z, int32_t *t)
{
    uint8_t reg = 0x31;      /* MAG_X_XLSB */
    uint8_t rx[14] = {0};    /* 2 dummy bytes + 12 data bytes */

    esp_err_t err = i3c_master_i3c_device_transmit_receive(
        dev, &reg, 1, rx, sizeof(rx), -1
    );
    if (err != ESP_OK) {
        return err;
    }

    uint32_t raw_x = ((uint32_t)rx[2])  | ((uint32_t)rx[3]  << 8) | ((uint32_t)rx[4]  << 16);
    uint32_t raw_y = ((uint32_t)rx[5])  | ((uint32_t)rx[6]  << 8) | ((uint32_t)rx[7]  << 16);
    uint32_t raw_z = ((uint32_t)rx[8])  | ((uint32_t)rx[9]  << 8) | ((uint32_t)rx[10] << 16);
    uint32_t raw_t = ((uint32_t)rx[11]) | ((uint32_t)rx[12] << 8) | ((uint32_t)rx[13] << 16);

    *x = sign_extend_21(raw_x);
    *y = sign_extend_21(raw_y);
    *z = sign_extend_21(raw_z);
    *t = sign_extend_21(raw_t);

    return ESP_OK;
}

/**
 * @brief Convert raw values into approximate engineering units.
 *
 * These are useful for bring-up and debug. They are not intended to replace
 * the fully compensated output path that depends on OTP/calibration handling.
 */
static void convert_raw_to_approx_units(int32_t x, int32_t y, int32_t z, int32_t t,
                                        float *mx_uT, float *my_uT, float *mz_uT, float *temp_c)
{
    *mx_uT = x * 0.007070f;
    *my_uT = y * 0.007070f;
    *mz_uT = z * 0.007175f;

    *temp_c = t * 0.000981f;
    if (*temp_c > 0.0f) {
        *temp_c -= 25.49f;
    } else if (*temp_c < 0.0f) {
        *temp_c += 25.49f;
    }
}

/**
 * @brief Create I3C master bus configured for ENTDAA discovery.
 */
static esp_err_t create_i3c_bus_for_entdaa(void)
{
    i3c_master_bus_config_t bus_cfg = {
        .sda_io_num = BMM350_I3C_SDA_GPIO,
        .scl_io_num = BMM350_I3C_SCL_GPIO,
        .clock_source = I3C_MASTER_CLK_SRC_DEFAULT,
        .internal_pullup_resistor_val = I3C_MASTER_INTERNAL_PULLUP_RESISTOR_2_4K,
        .trans_queue_depth = 4,
        .intr_priority = 0,

        .i3c_scl_freq_hz_od = BMM350_I3C_OD_FREQ_HZ,
        .i3c_scl_freq_hz_pp = BMM350_I3C_PP_FREQ_HZ,
        .i3c_scl_pp_duty_cycle = 0.5f,
        .i3c_scl_od_duty_cycle = 0.5f,
        .i3c_sda_od_hold_time_ns = 25,
        .i3c_sda_pp_hold_time_ns = 0,

        .entdaa_device_num = BMM350_EXPECTED_DEVICE_NUM,

        .flags = {
            .enable_async_trans = 0,
            .ibi_rstart_trans_en = 0,
            .ibi_silent_sir_rejected = 0,
            .ibi_no_auto_disable = 0,
        },
    };

    return i3c_new_master_bus(&bus_cfg, &g_bus);
}

/**
 * @brief Send an initial broadcast CCC frame to wake/shift target into
 *        I3C-aware state.
 *
 * We use broadcast RSTDAA as a harmless preamble before ENTDAA.
 */
static esp_err_t send_i3c_enable_preamble(void)
{
    i3c_master_ccc_transfer_config_t ccc = {
        .ccc_command = I3C_CCC_RSTDAA,
        .direction = I3C_MASTER_TRANSFER_DIRECTION_WRITE,
        .device_address = 0,   /* broadcast */
        .data = NULL,
        .data_size = 0,
    };

    int64_t t0 = now_us();
    esp_err_t err = i3c_master_transfer_ccc(g_bus, &ccc);
    int64_t t1 = now_us();

    ESP_LOGI(TAG, "Broadcast CCC RSTDAA cost = %lld us", (long long)(t1 - t0));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Broadcast CCC RSTDAA failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_rom_delay_us(2000);
    return ESP_OK;
}

/**
 * @brief Discover a single I3C device using ENTDAA and return its handle.
 */
static esp_err_t discover_single_device_by_entdaa(i3c_master_i3c_device_handle_t *out_dev)
{
    if (!out_dev || !g_bus) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_dev = NULL;

    i3c_master_i3c_device_table_handle_t table_handle = NULL;

    int64_t t0 = now_us();
    esp_err_t err = i3c_master_scan_devices_by_entdaa(g_bus, &table_handle);
    int64_t t1 = now_us();

    ESP_LOGI(TAG, "ENTDAA scan cost = %lld us", (long long)(t1 - t0));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ENTDAA scan failed: %s", esp_err_to_name(err));
        return err;
    }

    if (table_handle == NULL) {
        ESP_LOGW(TAG, "ENTDAA completed but no valid device table returned");
        return ESP_ERR_NOT_FOUND;
    }

    size_t device_count = 0;
    err = i3c_master_get_device_count(table_handle, &device_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_device_count failed: %s", esp_err_to_name(err));
        i3c_master_free_device_handle_table(table_handle);
        return err;
    }

    ESP_LOGI(TAG, "ENTDAA found %u device(s)", (unsigned)device_count);

    if (device_count == 0) {
        i3c_master_free_device_handle_table(table_handle);
        return ESP_ERR_NOT_FOUND;
    }

    err = i3c_master_get_device_handle(table_handle, 0, out_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_device_handle[0] failed: %s", esp_err_to_name(err));
        i3c_master_free_device_handle_table(table_handle);
        return err;
    }

    i3c_device_information_t info;
    memset(&info, 0, sizeof(info));

    err = i3c_master_i3c_device_get_info(*out_dev, &info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i3c_device_get_info failed: %s", esp_err_to_name(err));
        i3c_master_free_device_handle_table(table_handle);
        return err;
    }

    ESP_LOGI(TAG,
             "Found device: dyn=0x%02X BCR=0x%02X DCR=0x%02X PID=0x%012llX",
             info.dynamic_addr,
             info.bcr,
             info.dcr,
             (unsigned long long)(info.pid & 0xFFFFFFFFFFFFULL));

    err = i3c_master_free_device_handle_table(table_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "free_device_handle_table failed: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

/**
 * @brief Bind a discovered ENTDAA handle into the local BMM350 I3C port object.
 */
static void bind_port_from_entdaa_handle(bmm350_i3c_port_t *port,
                                         i3c_master_i3c_device_handle_t dev)
{
    memset(port, 0, sizeof(*port));
    port->bus = g_bus;
    port->dev = dev;
    port->static_addr = 0;
    port->dynamic_addr = 0;  /* dynamic address is already handled by ENTDAA */
    port->sda_gpio = BMM350_I3C_SDA_GPIO;
    port->scl_gpio = BMM350_I3C_SCL_GPIO;
    port->od_freq_hz = BMM350_I3C_OD_FREQ_HZ;
    port->pp_freq_hz = BMM350_I3C_PP_FREQ_HZ;
    port->inited = true;
}

/**
 * @brief Perform a minimal but stable sensor initialization sequence.
 */
static esp_err_t bmm350_minimal_init(struct bmm350_dev *dev,
                                     bmm350_i3c_port_t *port)
{
    if (!dev || !port) {
        return ESP_ERR_INVALID_ARG;
    }

    bmm350_i3c_bind_dev(dev, port);
    dev->chipId = BMM350_CHIP_ID;

    uint8_t otp_cmd = BMM350_OTP_CMD_PWR_OFF_OTP;
    int8_t rslt = bmm350SetRegs(BMM350_REG_OTP_CMD_REG, &otp_cmd, 1, dev);
    if (rslt != BMM350_OK) {
        ESP_LOGE(TAG, "Terminate boot failed: %d", rslt);
        return ESP_FAIL;
    }

    dev->delayUs(1000, dev->intfPtr);

    rslt = bmm350_enable_axes(BMM350_X_EN, BMM350_Y_EN, BMM350_Z_EN, dev);
    if (rslt != BMM350_OK) {
        ESP_LOGE(TAG, "Enable axes failed: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bmm350SetPowerMode(eBmm350NormalMode, dev);
    if (rslt != BMM350_OK) {
        ESP_LOGE(TAG, "Set normal mode failed: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bmm350SetOdrPerformance(BMM350_DATA_RATE_100HZ, BMM350_AVERAGING_2, dev);
    if (rslt != BMM350_OK) {
        ESP_LOGE(TAG, "Set ODR/performance failed: %d", rslt);
        return ESP_FAIL;
    }

    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Step 1: Create I3C bus for single-sensor ENTDAA");

    esp_err_t err = create_i3c_bus_for_entdaa();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create_i3c_bus_for_entdaa failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Step 2: Send initial I3C broadcast preamble");
    err = send_i3c_enable_preamble();
    if (err != ESP_OK) {
        return;
    }

    ESP_LOGI(TAG, "Step 3: ENTDAA discovery");
    i3c_master_i3c_device_handle_t entdaa_dev = NULL;
    err = discover_single_device_by_entdaa(&entdaa_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ENTDAA did not find a valid device: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Step 4: Raw I3C probe via ENTDAA handle");

    int64_t t0 = now_us();
    esp_err_t probe_err = probe_bmm350_chipid_i3c_handle(entdaa_dev);
    int64_t t1 = now_us();

    ESP_LOGI(TAG, "I3C CHIP_ID probe cost = %lld us", (long long)(t1 - t0));

    if (probe_err != ESP_OK) {
        ESP_LOGE(TAG, "Raw I3C probe failed");
        return;
    }

    ESP_LOGI(TAG, "Raw CHIP_ID probe success");

    ESP_LOGI(TAG, "Step 5: Bind Bosch SensorAPI");
    bind_port_from_entdaa_handle(&g_bmm_port, entdaa_dev);
    bmm350_i3c_bind_dev(&g_bmm_dev, &g_bmm_port);

    ESP_LOGI(TAG, "Step 6: Minimal sensor initialization");
    err = bmm350_minimal_init(&g_bmm_dev, &g_bmm_port);
    if (err != ESP_OK) {
        return;
    }

    ESP_LOGI(TAG, "Minimal init success");
    ESP_LOGI(TAG, "Step 7: Read magnetic field data");

    uint32_t sample_count = 0;
    int64_t total_read_us = 0;

    while (1) {
        int32_t x = 0, y = 0, z = 0, t = 0;

        int64_t tr0 = now_us();
        err = read_bmm350_raw_direct(g_bmm_port.dev, &x, &y, &z, &t);
        int64_t tr1 = now_us();

        if (err == ESP_OK) {
            float mx_uT = 0.0f, my_uT = 0.0f, mz_uT = 0.0f, temp_c = 0.0f;
            convert_raw_to_approx_units(x, y, z, t, &mx_uT, &my_uT, &mz_uT, &temp_c);
            float norm_uT = sqrtf(mx_uT * mx_uT + my_uT * my_uT + mz_uT * mz_uT);

            int64_t read_cost_us = tr1 - tr0;
            sample_count++;
            total_read_us += read_cost_us;

            ESP_LOGI(TAG,
                     "X=%.2f uT Y=%.2f uT Z=%.2f uT | |B|=%.2f uT | Temp=%.2f C | read=%lld us",
                     mx_uT, my_uT, mz_uT, norm_uT, temp_c,
                     (long long)read_cost_us);

            if ((sample_count % BMM350_PERF_LOG_EVERY_N) == 0) {
                double avg_us = (double)total_read_us / (double)sample_count;
                double est_hz = 1000000.0 / avg_us;
                ESP_LOGI(TAG,
                         "Average raw-read over %u samples = %.2f us (theoretical %.2f Hz)",
                         sample_count, avg_us, est_hz);
            }
        } else {
            ESP_LOGE(TAG, "Raw read failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(BMM350_READ_PERIOD_MS));
    }
}