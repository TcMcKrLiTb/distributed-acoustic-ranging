target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/arrival_detector.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arrival_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arrival_clock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_arrival.c)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
