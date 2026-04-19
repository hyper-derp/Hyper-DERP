add_executable(hd-wg hd_wg.cc)
target_link_libraries(hd-wg PRIVATE hd_wg spdlog::spdlog)

add_executable(hdctl hdctl.cc)
target_link_libraries(hdctl PRIVATE libderp ${ZMQ_LIB})
target_include_directories(hdctl PRIVATE ${ZMQ_INCLUDE_DIR})
