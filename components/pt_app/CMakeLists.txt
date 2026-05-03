# --- SPA asset pipeline ---
#
# build_spa_assets.py is the single source of truth that turns
# spa/* into the embedded blobs. It does two things:
#   1. inlines qrcode.min.js into index.html (saves one HTTP req)
#   2. emits a gzip-compressed copy of every text asset alongside
#      the plain copy
#
# We embed BOTH the plain and the .gz copies. The HTTP server picks
# which to send based on the client's Accept-Encoding header. Because
# both originate from one bundler run, the gzipped and ungzipped
# versions can never drift -- a source change forces a rebuild of
# both via the DEPENDS list below.
#
# Material Icons (Apache-2.0, (c) Google,
# https://github.com/marella/material-icons) -- filled variant.
# Binary woff2 (~125 KB) goes through EMBED_FILES untouched (already
# compressed; gzipping hurts). The companion name->codepoint table is
# emitted by the bundler from the upstream hex-string format into
# decimal integers the SPA can hand directly to String.fromCodePoint.
# favicon.ico (multi-resolution, ~34 KB) is original artwork by the
# project author. Inkscape source kept at doc/pt700favicon.svg if you
# need to re-export.

set(SPA_BUNDLE_DIR ${CMAKE_CURRENT_BINARY_DIR}/spa-bundle)
set(SPA_BUNDLE_OUTPUTS
    ${SPA_BUNDLE_DIR}/index.html
    ${SPA_BUNDLE_DIR}/index.html.gz
    ${SPA_BUNDLE_DIR}/setup.html
    ${SPA_BUNDLE_DIR}/setup.html.gz
    ${SPA_BUNDLE_DIR}/material-icons.json
    ${SPA_BUNDLE_DIR}/material-icons.json.gz
)
set(SPA_BUNDLE_INPUTS
    ${CMAKE_CURRENT_SOURCE_DIR}/spa/index.html
    ${CMAKE_CURRENT_SOURCE_DIR}/spa/qrcode.min.js
    ${CMAKE_CURRENT_SOURCE_DIR}/spa/setup.html
    ${CMAKE_CURRENT_SOURCE_DIR}/spa/fonts/material-icons-codepoints.json
    ${CMAKE_SOURCE_DIR}/scripts/build_spa_assets.py
)
# ESP-IDF processes this CMakeLists twice: once in script mode (just
# to extract REQUIRES) where add_custom_command isn't allowed, and
# once in full mode to actually build. The guard skips the recipe
# during the script pass; the EMBED_FILES list still resolves there
# because CMake only records the path strings at configure time.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_custom_command(
        OUTPUT  ${SPA_BUNDLE_OUTPUTS}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SPA_BUNDLE_DIR}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/build_spa_assets.py
                --spa-dir    ${CMAKE_CURRENT_SOURCE_DIR}/spa
                --output-dir ${SPA_BUNDLE_DIR}
        DEPENDS ${SPA_BUNDLE_INPUTS}
        COMMENT "Bundling + gzipping SPA assets"
        VERBATIM
    )
endif()

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
    # EMBED_FILES (not EMBED_TXTFILES) -- the latter appends a NUL
    # terminator that ends up in the wire response and breaks JS
    # parsing. We never use these as C strings, only length-bounded
    # blobs.
    EMBED_FILES    ${SPA_BUNDLE_DIR}/index.html
                   ${SPA_BUNDLE_DIR}/index.html.gz
                   ${SPA_BUNDLE_DIR}/setup.html
                   ${SPA_BUNDLE_DIR}/setup.html.gz
                   ${SPA_BUNDLE_DIR}/material-icons.json
                   ${SPA_BUNDLE_DIR}/material-icons.json.gz
                   "spa/fonts/material-icons.woff2"
                   "spa/favicon.ico"
)
# Tie the bundler to the component lib so EMBED_FILES sees fresh
# outputs on every build (custom_command DEPENDS already chains the
# bundler to source changes; this line ensures it actually runs).
# Same script-mode guard as above.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_custom_target(pt_app_spa_bundle DEPENDS ${SPA_BUNDLE_OUTPUTS})
    add_dependencies(${COMPONENT_LIB} pt_app_spa_bundle)
endif()
