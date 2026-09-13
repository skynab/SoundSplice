# Real-time primitives: header-only, no dependencies. Consumed by the engine,
# the app, and the tests. Include paths are rooted at src/ so headers are
# referenced as <rt/Foo.h>.
add_library(soundsplice_rt INTERFACE)
add_library(soundsplice::rt ALIAS soundsplice_rt)

target_include_directories(soundsplice_rt INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(soundsplice_rt INTERFACE cxx_std_20)
