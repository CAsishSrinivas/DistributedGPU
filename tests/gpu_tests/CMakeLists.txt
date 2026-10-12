add_executable(test_gpu test_gpu.cpp)
target_link_libraries(test_gpu PRIVATE dgpu_d3d12 dgpu_protocol dgpu_telemetry)
add_test(NAME gpu_tests COMMAND test_gpu)
