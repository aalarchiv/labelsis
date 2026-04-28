idf_component_register(
    SRCS "src/pt_app.c"
    INCLUDE_DIRS "include"
    REQUIRES
        pt_session
        pt_transport
        pt_protocol
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        nvs_flash
    EMBED_TXTFILES "spa/index.html"
)
