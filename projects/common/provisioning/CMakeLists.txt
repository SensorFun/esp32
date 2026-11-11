idf_component_register(
    SRCS "provisioning.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_http_server nvs_flash esp_driver_gpio esp_system
    EMBED_TXTFILES "index.html"
)
