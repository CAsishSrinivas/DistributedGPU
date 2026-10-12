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
    ../../apps/host/pipeline_executor.cpp
    ../../apps/host/work_distributor.cpp
    ../../apps/host/workload_cost_model.cpp
    ../../apps/host/frame_compositor.cpp
)
target_include_directories(test_partitioner PRIVATE ../../apps/host ../.. ../../libs)
target_link_libraries(test_partitioner PRIVATE
    dgpu_compile_options
    dgpu_protocol
    dgpu_telemetry
    $<$<BOOL:${D3D12_AVAILABLE}>:dgpu_d3d12>
)
add_test(NAME partitioner_tests COMMAND test_partitioner)

add_executable(test_cost_model
    test_cost_model.cpp
    ../../apps/host/workload_cost_model.cpp
    ../../apps/host/workload_partitioner.cpp
    ../../apps/host/job_scheduler.cpp
)
target_include_directories(test_cost_model PRIVATE ../../apps/host)
target_link_libraries(test_cost_model PRIVATE
    dgpu_compile_options
    dgpu_protocol
    dgpu_task_runtime
)
add_test(NAME cost_model_tests COMMAND test_cost_model)

add_executable(test_work_stealing
    test_work_stealing.cpp
    ../../apps/host/work_distributor.cpp
    ../../apps/host/batch_scheduler.cpp
    ../../apps/host/pipeline_executor.cpp
    ../../apps/host/workload_cost_model.cpp
    ../../apps/host/workload_partitioner.cpp
    ../../apps/host/statistical_validator.cpp
    ../../apps/host/frame_compositor.cpp
)
target_include_directories(test_work_stealing PRIVATE ../../apps/host)
target_link_libraries(test_work_stealing PRIVATE
    dgpu_compile_options
    dgpu_protocol
)
add_test(NAME work_stealing_tests COMMAND test_work_stealing)

add_executable(test_pipeline_transfers
    test_pipeline_transfers.cpp
    ../../apps/host/pipeline_executor.cpp
    ../../apps/host/batch_scheduler.cpp
    ../../apps/host/work_distributor.cpp
    ../../apps/host/workload_cost_model.cpp
    ../../apps/host/workload_partitioner.cpp
    ../../apps/host/statistical_validator.cpp
    ../../apps/host/frame_compositor.cpp
)
target_include_directories(test_pipeline_transfers PRIVATE ../../apps/host ../.. ../../libs)
target_link_libraries(test_pipeline_transfers PRIVATE
    dgpu_compile_options
    dgpu_protocol
    dgpu_telemetry
    $<$<BOOL:${D3D12_AVAILABLE}>:dgpu_d3d12>
)
add_test(NAME pipeline_transfers_tests COMMAND test_pipeline_transfers)

add_executable(test_resource_pool
    test_resource_pool.cpp
    ../../apps/host/frame_compositor.cpp
)
target_include_directories(test_resource_pool PRIVATE ../../apps/host ../.. ../../libs)
target_link_libraries(test_resource_pool PRIVATE
    dgpu_compile_options
    dgpu_protocol
    dgpu_telemetry
    $<$<BOOL:${D3D12_AVAILABLE}>:dgpu_d3d12>
)
add_test(NAME resource_pool_tests COMMAND test_resource_pool)

add_executable(test_benchmark_matrix
    test_benchmark_matrix.cpp
    ../../apps/host/workload_cost_model.cpp
    ../../apps/host/workload_partitioner.cpp
    ../../apps/host/statistical_validator.cpp
    ../../apps/host/batch_scheduler.cpp
    ../../apps/host/pipeline_executor.cpp
    ../../apps/host/work_distributor.cpp
    ../../apps/host/frame_compositor.cpp
)
target_include_directories(test_benchmark_matrix PRIVATE ../../apps/host ../.. ../../libs)
target_link_libraries(test_benchmark_matrix PRIVATE
    dgpu_compile_options
    dgpu_protocol
    dgpu_networking
    dgpu_task_runtime
    dgpu_telemetry
    $<$<BOOL:${D3D12_AVAILABLE}>:dgpu_d3d12>
)
add_test(NAME benchmark_matrix_tests COMMAND test_benchmark_matrix)
