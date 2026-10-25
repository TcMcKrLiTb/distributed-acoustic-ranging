target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/range_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/range_temperature.c
    ${CMAKE_CURRENT_SOURCE_DIR}/range_recorder.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ranging.c)
# DSP runs in a separate preemptible task; avoid Debug's -O0 in the inner loop.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/range_core.c PROPERTIES COMPILE_OPTIONS "-O2")
