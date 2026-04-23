#include "bmm350_port_esp32p4_i3c.h"

#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "bmm350_i3c";

esp_err_t bmm350_i3c_port_init(
    bmm350_i3c_port_t *port,
    int sda_gpio,
    int scl_gpio,
    uint8_t static_addr,
    uint8_t dynamic_addr,
    uint32_t od_freq_hz,
    uint32_t pp_freq_hz)
{
    if (!port) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(port, 0, sizeof(*port));
    port->sda_gpio = sda_gpio;
    port->scl_gpio = scl_gpio;
    port->static_addr = static_addr;
    port->dynamic_addr = dynamic_addr;
    port->od_freq_hz = od_freq_hz;
    port->pp_freq_hz = pp_freq_hz;

    i3c_master_bus_config_t bus_cfg = {
    .sda_io_num = sda_gpio,
    .scl_io_num = scl_gpio,
    .clock_source = I3C_MASTER_CLK_SRC_DEFAULT,
    .internal_pullup_resistor_val = I3C_MASTER_INTERNAL_PULLUP_RESISTOR_2_4K,
    .trans_queue_depth = 4,
    .intr_priority = 0,

    .i3c_scl_freq_hz_od = od_freq_hz,
    .i3c_scl_freq_hz_pp = pp_freq_hz,
    .i3c_scl_pp_duty_cycle = 0.5f,
    .i3c_scl_od_duty_cycle = 0.5f,
    .i3c_sda_od_hold_time_ns = 25,
    .i3c_sda_pp_hold_time_ns = 0,
    .entdaa_device_num = 1,

    .flags = {
        .enable_async_trans = 0,
        .ibi_rstart_trans_en = 0,
        .ibi_silent_sir_rejected = 0,
        .ibi_no_auto_disable = 0,
    },
};

    esp_err_t err = i3c_new_master_bus(&bus_cfg, &port->bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i3c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    i3c_device_i3c_config_t dev_cfg = {
        .dynamic_addr = dynamic_addr,
        .static_addr = static_addr,
    };

    err = i3c_master_bus_add_i3c_static_device(port->bus, &dev_cfg, &port->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_i3c_static_device failed: %s", esp_err_to_name(err));
        i3c_del_master_bus(port->bus);
        port->bus = NULL;
        return err;
    }

    port->inited = true;
    ESP_LOGI(TAG,
             "BMM350 on I3C: static=0x%02X dynamic=0x%02X SDA=%d SCL=%d OD=%lu PP=%lu",
             static_addr,
             dynamic_addr,
             sda_gpio,
             scl_gpio,
             (unsigned long)od_freq_hz,
             (unsigned long)pp_freq_hz);

    return ESP_OK;
}

esp_err_t bmm350_i3c_port_deinit(bmm350_i3c_port_t *port)
{
    if (!port || !port->inited) {
        return ESP_OK;
    }

    if (port->dev) {
        i3c_master_bus_rm_i3c_device(port->dev);
        port->dev = NULL;
    }

    if (port->bus) {
        i3c_del_master_bus(port->bus);
        port->bus = NULL;
    }

    port->inited = false;
    return ESP_OK;
}

void bmm350_i3c_bind_dev(struct bmm350_dev *dev, bmm350_i3c_port_t *port)
{
    if (!dev || !port) {
        return;
    }

    memset(dev, 0, sizeof(*dev));
    dev->intfPtr = port;
    dev->read = bmm350_i3c_read;
    dev->write = bmm350_i3c_write;
    dev->delayUs = bmm350_i3c_delay_us;
}

BMM350_INTF_RET_TYPE bmm350_i3c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *intfPtr)
{
    if (!reg_data || !intfPtr || len == 0) {
        return -1;
    }

    bmm350_i3c_port_t *port = (bmm350_i3c_port_t *)intfPtr;
    if (!port->inited || !port->dev) {
        return -1;
    }

    esp_err_t err = i3c_master_i3c_device_transmit_receive(
        port->dev,
        &reg_addr,
        1,
        reg_data,
        len,
        -1
    );

    return (err == ESP_OK) ? 0 : -1;
}

BMM350_INTF_RET_TYPE bmm350_i3c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intfPtr)
{
    if (!reg_data || !intfPtr || len == 0) {
        return -1;
    }

    bmm350_i3c_port_t *port = (bmm350_i3c_port_t *)intfPtr;
    if (!port->inited || !port->dev) {
        return -1;
    }

    uint8_t buf[1 + 32];
    if (len > 32) {
        return -1;
    }

    buf[0] = reg_addr;
    memcpy(&buf[1], reg_data, len);

    esp_err_t err = i3c_master_i3c_device_transmit(
        port->dev,
        buf,
        len + 1,
        -1
    );

    return (err == ESP_OK) ? 0 : -1;
}

void bmm350_i3c_delay_us(uint32_t period, void *intfPtr)
{
    (void)intfPtr;
    esp_rom_delay_us(period);
}