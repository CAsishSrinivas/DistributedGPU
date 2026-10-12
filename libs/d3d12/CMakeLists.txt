add_library(dgpu_d3d12 STATIC
    src/d3d12_context.cpp
    src/gpu_executor.cpp
    src/pipelined_gpu_executor.cpp
    src/resource_pool.cpp
)
target_include_directories(dgpu_d3d12 PUBLIC include)
target_link_libraries(dgpu_d3d12
    PUBLIC  dgpu_compile_options dgpu_protocol dgpu_telemetry
    PRIVATE d3d12 dxgi d3dcompiler dxguid
)
target_compile_features(dgpu_d3d12 PUBLIC cxx_std_20)
