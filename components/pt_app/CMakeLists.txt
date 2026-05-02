idf_component_register(
    SRCS "src/pt_app.c" "src/pt_led.c" "src/pt_dns.c"
    INCLUDE_DIRS "include"
    REQUIRES
        pt_session
        pt_transport
        pt_protocol
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_app_format
        nvs_flash
        mdns
        esp_driver_gpio
        lwip
        led_strip
    EMBED_TXTFILES "spa/index.html" "spa/setup.html" "spa/qrcode.min.js"
    # Bootstrap Icons v1.13.1 (MIT, https://icons.getbootstrap.com/) --
    # binary woff2 (~130 KB) + name->codepoint JSON for the picker.
    # favicon.ico (multi-resolution, ~34 KB) is original artwork by the
    # project author, no third-party attribution required.
    EMBED_FILES    "spa/fonts/bootstrap-icons.woff2"
                   "spa/fonts/bootstrap-icons.json"
                   "spa/favicon.ico"
)
