add_executable(dgpu_host
    main.cpp
    host_server.cpp
    job_scheduler.cpp
    workload_cost_model.cpp
    work_distributor.cpp
    pipeline_executor.cpp
    frame_compositor.cpp
    workload_partitioner.cpp
    statistical_validator.cpp
    batch_scheduler.cpp
)
# The host app headers are in its own directory
target_include_directories(dgpu_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dgpu_host PRIVATE
    dgpu_compile_options
    dgpu_protocol
    dgpu_networking
    dgpu_task_runtime
    dgpu_telemetry
    $<$<BOOL:${D3D12_AVAILABLE}>:dgpu_d3d12>
)
set_target_properties(dgpu_host PROPERTIES OUTPUT_NAME "host")
