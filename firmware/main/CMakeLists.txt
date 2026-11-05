idf_component_register(
    SRCS
        "main.cc"
        "app/app.cc"
        "app/input.cc"
        "app/library.cc"
        "app/saved_state.cc"
        "bsp/battery_adc.cc"
        "bsp/board.cc"
        "bsp/board_power.cc"
        "bsp/charge_status.cc"
        "drivers/audio/audio_player.cc"
        "drivers/bus/i2c_bus_lock.cc"
        "drivers/display/epd_ssd1683.cc"
        "drivers/display/epd_ssd1683_commands.cc"
        "drivers/display/epd_ssd1683_spi.cc"
        "drivers/input/button.cc"
        "power/sleep.cc"
        "ui/font.cc"
        "ui/menu.cc"
        "ui/status_bar.cc"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        driver
        esp_adc
        esp_driver_usb_serial_jtag
        esp_timer
        esp_psram
        json
)

# 固件版本每次编译都重新生成：取 git 提交号，esp_app_desc 里的版本只在 CMake 配置时更新。
set(version_c ${CMAKE_CURRENT_BINARY_DIR}/version.c)
add_custom_target(chalk_version
    COMMAND ${CMAKE_COMMAND} -DOUT=${version_c} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/../..
            -P ${CMAKE_CURRENT_SOURCE_DIR}/version.cmake
    BYPRODUCTS ${version_c})
target_sources(${COMPONENT_LIB} PRIVATE ${version_c})
add_dependencies(${COMPONENT_LIB} chalk_version)

# 内容镜像由 tools（chalk build）生成，idf.py flash 时一起烧到 storage 分区。
set(content_image ${CMAKE_CURRENT_SOURCE_DIR}/../../build/storage.bin)
if(EXISTS ${content_image})
    partition_table_get_partition_info(storage_offset "--partition-name storage" "offset")
    esptool_py_flash_target_image(flash storage "${storage_offset}" "${content_image}")
else()
    message(WARNING "build/storage.bin 不存在，先运行 chalk build；本次只烧录固件")
endif()
