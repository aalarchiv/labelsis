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
        mdns
        esp_driver_gpio
    EMBED_TXTFILES "spa/index.html" "spa/setup.html" "spa/qrcode.min.js"
    # Bootstrap Icons v1.13.1 (MIT, https://icons.getbootstrap.com/) —
    # binary woff2 (~130 KB) + name->codepoint JSON for the picker.
    EMBED_FILES    "spa/fonts/bootstrap-icons.woff2"
                   "spa/fonts/bootstrap-icons.json"
)
