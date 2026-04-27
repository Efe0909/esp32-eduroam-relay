idf_component_register(SRCS "relay_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES console esp_wifi esp_netif esp_event nvs_flash lwip wpa_supplicant)
target_compile_options(__idf_main PRIVATE -Wno-format)
