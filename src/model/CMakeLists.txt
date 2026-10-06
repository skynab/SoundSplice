# The model is header-only, apart from the few tables too expensive to compile
# in every file that includes them - see EffectPresets.cpp. Those are built
# once here and linked by everything that uses the model.
add_library(soundsplice_model STATIC
  EffectParams.cpp
  EffectPresets.cpp)
add_library(soundsplice::model ALIAS soundsplice_model)

target_link_libraries(soundsplice_model PUBLIC soundsplice::rt)
set_target_properties(soundsplice_model PROPERTIES POSITION_INDEPENDENT_CODE ON)
