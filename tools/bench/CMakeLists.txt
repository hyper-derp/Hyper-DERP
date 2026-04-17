# Standalone test tools.

add_executable(derp-test-client derp_test_client.cc)
target_link_libraries(derp-test-client PRIVATE libderp)

add_executable(derp-tun-proxy derp_tun_proxy.cc)
target_link_libraries(derp-tun-proxy PRIVATE libderp)

add_executable(derp-scale-test derp_scale_test.cc)
target_link_libraries(derp-scale-test PRIVATE libderp)

add_executable(derp-scale-test-uring derp_scale_test_uring.cc)
target_link_libraries(derp-scale-test-uring PRIVATE libderp)

add_executable(hd-test-client hd_test_client.cc)
target_link_libraries(hd-test-client PRIVATE libderp)

add_executable(hd-scale-test hd_scale_test.cc)
target_link_libraries(hd-scale-test PRIVATE libderp)
