idf_component_register(
    SRCS
        "wt_app_web.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_timer
        esp_http_server
        app_update
        spi_flash
        esp_driver_tsens
        espressif__cjson
        wt_app_log
        wt_app_wifi
        wt_app_settings
        wt_app_led
)

# The web console (web/ at the project root) is gzip-compressed and built into
# the firmware, so one OTA image updates firmware and console together. Each
# file is linked as _binary_<name>_gz_start / _end.
idf_build_get_property(python PYTHON)
foreach(name index.html style.css app.js)
    set(src ${PROJECT_DIR}/web/${name})
    set(gz ${CMAKE_CURRENT_BINARY_DIR}/${name}.gz)
    add_custom_command(
        OUTPUT ${gz}
        COMMAND ${python} ${PROJECT_DIR}/tools/gzip_web.py ${src} ${gz}
        DEPENDS ${src} ${PROJECT_DIR}/tools/gzip_web.py
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} ${gz} BINARY)
endforeach()
