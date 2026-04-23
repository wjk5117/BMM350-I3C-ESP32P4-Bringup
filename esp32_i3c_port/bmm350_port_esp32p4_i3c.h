#ifndef BMM350_PORT_ESP32P4_I3C_H
#define BMM350_PORT_ESP32P4_I3C_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i3c_master.h"
#include "bmm350.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i3c_master_bus_handle_t bus;
    i3c_master_i3c_device_handle_t dev;
    uint8_t static_addr;
    uint8_t dynamic_addr;
    int sda_gpio;
    int scl_gpio;
    uint32_t od_freq_hz;
    uint32_t pp_freq_hz;
    bool inited;
} bmm350_i3c_port_t;

esp_err_t bmm350_i3c_port_init(
    bmm350_i3c_port_t *port,
    int sda_gpio,
    int scl_gpio,
    uint8_t static_addr,
    uint8_t dynamic_addr,
    uint32_t od_freq_hz,
    uint32_t pp_freq_hz);

esp_err_t bmm350_i3c_port_deinit(bmm350_i3c_port_t *port);

void bmm350_i3c_bind_dev(struct bmm350_dev *dev, bmm350_i3c_port_t *port);

BMM350_INTF_RET_TYPE bmm350_i3c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *intfPtr);
BMM350_INTF_RET_TYPE bmm350_i3c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intfPtr);
void bmm350_i3c_delay_us(uint32_t period, void *intfPtr);

#ifdef __cplusplus
}
#endif

#endif