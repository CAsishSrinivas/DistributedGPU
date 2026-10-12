add_library(dgpu_protocol STATIC
    src/message.cpp
    src/lz4.cpp
    src/adaptive_transfer_policy.cpp
)
target_include_directories(dgpu_protocol PUBLIC include)
target_link_libraries(dgpu_protocol PUBLIC dgpu_compile_options)
target_compile_features(dgpu_protocol PUBLIC cxx_std_20)
