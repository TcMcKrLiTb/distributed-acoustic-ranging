set(DHT11_DRIVER_DIR ${CMAKE_SOURCE_DIR}/Drivers/LibDriver/DHT11)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${DHT11_DRIVER_DIR}/driver_dht11.c
    ${CMAKE_CURRENT_SOURCE_DIR}/dht11_capture.c
    ${CMAKE_CURRENT_SOURCE_DIR}/driver_dht11_interface.c
    ${CMAKE_CURRENT_SOURCE_DIR}/dht11.c)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${DHT11_DRIVER_DIR})

# Keep the short edge ISR and timestamp decoder efficient in Debug builds.
set_source_files_properties(
    ${DHT11_DRIVER_DIR}/driver_dht11.c
    ${CMAKE_CURRENT_SOURCE_DIR}/dht11_capture.c
    ${CMAKE_CURRENT_SOURCE_DIR}/driver_dht11_interface.c
    PROPERTIES COMPILE_OPTIONS "-O2")
