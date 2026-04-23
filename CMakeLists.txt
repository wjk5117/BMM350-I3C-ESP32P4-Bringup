idf_component_register(SRCS "single_bmm350_i3c_ENTDAA_main.c"
                        "bmm350.c"
                        "bmm350_port_esp32p4_i3c.c"
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS "."
                        REQUIRES
                        esp_driver_i3c
                        esp_timer
)

