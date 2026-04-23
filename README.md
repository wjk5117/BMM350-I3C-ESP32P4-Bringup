# Bosch BMM350 magnetometer over I3C on ESP32-P4

## Overview

This repository provides a practical baseline for bringing up a **single Bosch BMM350 sensor** over **I3C** on **ESP32-P4**.

It currently includes **three single-sensor I3C examples**:

- **SETAASA** — reuse static address as dynamic address
- **SETDASA** — assign a new dynamic address using static address
- **ENTDAA** — automatically discover the device and assign a dynamic address

The current goal is to provide a clean and reproducible starting point for:

- I3C bus initialization on ESP32-P4
- single-sensor BMM350 bring-up
- raw magnetic / temperature readout
---

## Current Status

### Supported

- ESP32-P4 I3C master bus initialization
- Single BMM350 I3C bring-up
- `SETAASA` example
- `SETDASA` example
- `ENTDAA` example
- Raw burst magnetic / temperature readout

### Not Yet Supported

- Multi-sensor BMM350 I3C on one shared bus
- Automatic multi-device identity handling

---

## Hardware

### Controller

- [DFRobot FireBeetle 2 ESP32-P4](https://www.dfrobot.com/product-2915.html)
- More details on the ESP32-P4 can be found in the [ESP32-P4 datasheet](https://documentation.espressif.com/esp32-p4_datasheet_en.pdf).


### Target

- [DFRobot Fermion BMM350 EVB](https://www.dfrobot.com/product-2891.html?srsltid=AfmBOoowFYMnucgk2IW3rAIRciNX7AoznstBkSVFOusGm2l1H5nq4p6-)
- More details on the BMM350 sensor can be found in the [Bosch BMM350 datasheet](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmm350-ds001.pdf).

### Current Wiring

- `SDA` -> `GPIO33`
- `SCL` -> `GPIO32`
- `VCC` -> `3V3`
- `GND` -> `GND`

> The examples in this repository assume the above wiring by default.
> If your hardware uses different pins, update the corresponding macros in the selected example source file.

---

## Software

- **Framework:** [ESP-IDF latest](https://github.com/espressif/esp-idf/tree/master)
- **Reference:** <https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/index.html>

---

## Repository Structure

### Core Driver Files

- **`bmm350_defs.h`** — register and constant definitions
- **`bmm350.h`** — public BMM350 driver interface
- **`bmm350.c`** — core BMM350 driver implementation

### ESP32-P4 I3C Port Layer

- **`bmm350_port_esp32p4_i3c.h`** — ESP32-P4 I3C port-layer header
- **`bmm350_port_esp32p4_i3c.c`** — ESP32-P4 I3C port-layer implementation

### Example Entry Files

- **`single_bmm350_setaasa_main.c`** — single-sensor `SETAASA` example
- **`single_bmm350_setdasa_main.c`** — single-sensor `SETDASA` example
- **`single_bmm350_entdaa_main.c`** — single-sensor `ENTDAA` example

### Build

- **`CMakeLists.txt`** — build configuration

> The default `main.c` file should be replaced with one of the example files above depending on the mode you want to test.

---

## I3C Modes Covered in This Repository

### 1. SETAASA

`SETAASA` tells a target with a valid static address to reuse that static address as its dynamic address.

In this repository:

- BMM350 static address is typically `0x14` or `0x15` depending on `ADSEL`
- In the single-sensor example, the dynamic address after `SETAASA` is the same as the static address

### 2. SETDASA

`SETDASA` assigns a new dynamic address to a target selected by its static address.

In this repository:

- The single-sensor example demonstrates:
  - static address = `0x14`
  - dynamic address = `0x24`

### 3. ENTDAA

`ENTDAA` performs automatic dynamic address assignment through I3C enumeration.

In this repository:

- The host discovers the BMM350 dynamically
- The assigned dynamic address is determined by the controller during enumeration

---

## Quick Start

### 1. Choose One Example

Select one of the following example entry files:

- `single_bmm350_setaasa_main.c`
- `single_bmm350_setdasa_main.c`
- `single_bmm350_entdaa_main.c`

Use it as your active `main` file.

### 2. Set Up ESP-IDF

Make sure ESP-IDF is installed and exported correctly.

### 3. Build and Flash

Typical ESP-IDF workflow:

```bash
idf.py build
idf.py flash monitor
