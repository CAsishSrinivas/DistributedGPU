add_executable(dgpu_game_adapter
    main.cpp
    d3d12_game_hook.cpp
    ../host/job_scheduler.cpp
    ../host/workload_cost_model.cpp
    ../host/workload_partitioner.cpp
    ../host/frame_compositor.cpp
)

target_include_directories(dgpu_game_adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../host
)

target_link_libraries(dgpu_game_adapter PRIVATE
    dgpu_compile_options
    dgpu_protocol
    dgpu_networking
    dgpu_task_runtime
    dgpu_telemetry
    $<$<BOOL:${D3D12_AVAILABLE}>:dgpu_d3d12>
)

set_target_properties(dgpu_game_adapter PROPERTIES OUTPUT_NAME "game_adapter")

# M7 Live DXGI Proxy Hook DLL (dxgi.dll for game directory injection)
if (WIN32)
    add_library(dgpu_dxgi SHARED
        dxgi_proxy.cpp
        dxgi.def
        d3d12_game_hook.cpp
        ../host/job_scheduler.cpp
        ../host/workload_cost_model.cpp
        ../host/workload_partitioner.cpp
        ../host/frame_compositor.cpp
    )

    target_include_directories(dgpu_dxgi PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../host
    )

    target_link_libraries(dgpu_dxgi PRIVATE
        dgpu_compile_options
        dgpu_protocol
        dgpu_networking
        dgpu_task_runtime
        dgpu_telemetry
        $<$<BOOL:${D3D12_AVAILABLE}>:dgpu_d3d12>
    )

    set_target_properties(dgpu_dxgi PROPERTIES
        OUTPUT_NAME "dxgi"
        PREFIX ""
    )
endif()
