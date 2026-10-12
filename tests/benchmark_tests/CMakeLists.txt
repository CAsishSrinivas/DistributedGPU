add_executable(test_telemetry test_telemetry.cpp)
target_link_libraries(test_telemetry PRIVATE dgpu_telemetry)
add_test(NAME telemetry_tests COMMAND test_telemetry)

add_executable(test_compositor test_compositor.cpp ../../apps/host/frame_compositor.cpp)
target_include_directories(test_compositor PRIVATE ../../apps/host)
target_link_libraries(test_compositor PRIVATE dgpu_protocol)
add_test(NAME compositor_tests COMMAND test_compositor)

add_executable(test_partitioner
    test_partitioner.cpp
    ../../apps/host/workload_partitioner.cpp
    ../../apps/host/statistical_validator.cpp
    ../../apps/host/batch_scheduler.cpp
    ../../apps/host/frame_compositor.cpp
)
target_include_directories(test_partitioner PRIVATE ../../apps/host)
target_link_libraries(test_partitioner PRIVATE dgpu_protocol)
add_test(NAME partitioner_tests COMMAND test_partitioner)

