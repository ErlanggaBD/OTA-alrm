idf_component_register(
    SRCS
        "main.c"
        "app_state.c"
        "log_buffer.c"
        "storage.c"
        "sensors.c"
        "alarm.c"
        "net_manager.c"
        "provisioning.c"
        "cloud_sync.c"
        "ota_manager.c"
        "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_http_server
        nvs_flash
        esp_wifi
        esp_netif
        esp_timer
        esp_event
        esp_http_client
        esp_https_ota
        app_update
        mbedtls
        spiffs
        json
        driver
        lwip
)

# Embed the local dashboard assets into the "storage" SPIFFS partition.
# Refresh them from shared/web with tools/sync_web.py after UI changes.
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
