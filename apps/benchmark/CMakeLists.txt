add_executable(dgpu_benchmark main.cpp)
target_link_libraries(dgpu_benchmark PRIVATE
    dgpu_compile_options
    dgpu_protocol
    dgpu_telemetry
    $<$<BOOL:${D3D12_AVAILABLE}>:dgpu_d3d12>
)
set_target_properties(dgpu_benchmark PROPERTIES OUTPUT_NAME "benchmark")
