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
    # All assets via EMBED_FILES (NOT EMBED_TXTFILES) -- the latter
    # appends a NUL terminator so _end - _start is one byte past the
    # real content; httpd_resp_send would then ship that NUL to the
    # browser, which is illegal in JS and chokes the qrcodejs parse.
    # We never use these as C strings, only as length-bounded blobs,
    # so EMBED_FILES is the correct primitive.
    #
    # Bootstrap Icons v1.13.1 (MIT, https://icons.getbootstrap.com/) --
    # binary woff2 (~130 KB) + name->codepoint JSON for the picker.
    # favicon.ico (multi-resolution, ~34 KB) is original artwork by the
    # project author, no third-party attribution required. Inkscape
    # source kept at doc/pt700favicon.svg if you need to re-export.
    EMBED_FILES    "spa/index.html"
                   "spa/setup.html"
                   "spa/qrcode.min.js"
                   "spa/fonts/bootstrap-icons.woff2"
                   "spa/fonts/bootstrap-icons.json"
                   "spa/favicon.ico"
)
