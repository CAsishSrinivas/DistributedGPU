add_library(dgpu_networking STATIC
    src/tcp_connection.cpp
)
target_include_directories(dgpu_networking PUBLIC include)
target_link_libraries(dgpu_networking
    PUBLIC  dgpu_compile_options dgpu_protocol
    PRIVATE Ws2_32
)
target_compile_features(dgpu_networking PUBLIC cxx_std_20)
