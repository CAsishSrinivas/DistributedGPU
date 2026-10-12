add_executable(dgpu_worker
    main.cpp
    worker_client.cpp
    gpu_executor.cpp
)
# The worker app headers are in its own directory
target_include_directories(dgpu_worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dgpu_worker PRIVATE
    dgpu_compile_options
    dgpu_protocol
    dgpu_networking
    dgpu_telemetry
    $<$<BOOL:${D3D12_AVAILABLE}>:dgpu_d3d12>
)
set_target_properties(dgpu_worker PROPERTIES OUTPUT_NAME "worker")
