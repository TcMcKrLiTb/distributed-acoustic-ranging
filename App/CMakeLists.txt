add_subdirectory(TimeSync)
add_subdirectory(AudioArrival)
add_subdirectory(Ranging)
add_subdirectory(DHT11)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/app.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Diagnostics/pcm_dump.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Model.cpp can consume TimeSync/time_sync.h without generated CMake changes.
target_include_directories(TouchGFX PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
