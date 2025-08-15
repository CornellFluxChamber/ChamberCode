add_library(SCD30_I2C INTERFACE)
target_sources(SCD30_I2C INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/scd30_i2c.c
    ${CMAKE_CURRENT_LIST_DIR}/src/sensirion_common.c
    ${CMAKE_CURRENT_LIST_DIR}/src/sensirion_i2c_hal.c
    ${CMAKE_CURRENT_LIST_DIR}/src/sensirion_i2c.c
)


target_include_directories(SCD30_I2C INTERFACE
    scd30
    include
)

target_link_libraries(SCD30_I2C INTERFACE
    hardware_i2c
    pico_stdlib
)