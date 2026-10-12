add_library(dgpu_telemetry STATIC
    src/telemetry.cpp
)
target_include_directories(dgpu_telemetry PUBLIC include)
target_link_libraries(dgpu_telemetry PUBLIC dgpu_compile_options)
target_compile_features(dgpu_telemetry PUBLIC cxx_std_20)
