target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/sync_core.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sync_app.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sync_format.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sync_task.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Port/sync_port_stm32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Port/sync_pps_stm32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Port/sync_log.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
