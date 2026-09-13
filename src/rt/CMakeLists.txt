# Real-time primitives: header-only, no dependencies. Consumed by the engine,
# the app, and the tests. Include paths are rooted at src/ so headers are
# referenced as <rt/Foo.h>.
add_library(looper_rt INTERFACE)
add_library(looper::rt ALIAS looper_rt)

target_include_directories(looper_rt INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(looper_rt INTERFACE cxx_std_20)
