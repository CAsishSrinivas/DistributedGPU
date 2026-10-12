add_library(dgpu_task_runtime STATIC
    src/task_runtime.cpp
)
target_include_directories(dgpu_task_runtime PUBLIC include)
target_link_libraries(dgpu_task_runtime PUBLIC dgpu_compile_options dgpu_protocol)
target_compile_features(dgpu_task_runtime PUBLIC cxx_std_20)
