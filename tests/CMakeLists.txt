include(GoogleTest)

file(GLOB_RECURSE TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

foreach(test_source ${TEST_SOURCES})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE
    libderp
    GTest::gtest_main
  )
  gtest_discover_tests(${test_name}
    DISCOVERY_TIMEOUT 10
    PROPERTIES TIMEOUT 30
  )
endforeach()
