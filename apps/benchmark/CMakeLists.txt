add_executable(dgpu_benchmark
    main.cpp
    ../../apps/host/workload_cost_model.cpp
    ../../apps/host/workload_partitioner.cpp
    ../../apps/host/statistical_validator.cpp
    ../../apps/host/batch_scheduler.cpp
    ../../apps/host/pipeline_executor.cpp
    ../../apps/host/work_distributor.cpp
    ../../apps/host/frame_compositor.cpp
)
target_include_directories(dgpu_benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../apps/host
    ../..
    ../../libs
)
target_link_libraries(dgpu_benchmark PRIVATE
    dgpu_compile_options
    dgpu_protocol
    dgpu_networking
    dgpu_task_runtime
    dgpu_telemetry
    $<$<BOOL:${D3D12_AVAILABLE}>:dgpu_d3d12>
)
set_target_properties(dgpu_benchmark PROPERTIES OUTPUT_NAME "benchmark")
