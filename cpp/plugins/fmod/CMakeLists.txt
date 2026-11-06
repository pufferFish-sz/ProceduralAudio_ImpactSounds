# FMOD sound module: a DLL for FMOD Studio's plug-ins folder and for the
# game (Unity: Assets/Plugins/FMOD/platforms/win/lib/x86_64, listed under
# "Dynamic Plugins" as procaudio_fmod). Plug-ins call back into FMOD through
# FMOD_DSP_STATE, so no FMOD library is linked.
add_library(procaudio_fmod SHARED procaudio_fmod.cpp)
target_include_directories(procaudio_fmod PRIVATE "${FMOD_SDK_DIR}/api/core/inc")
target_link_libraries(procaudio_fmod PRIVATE procaudio_core)
# Studio deck layout (dropdowns) must sit next to the DLL
add_custom_command(TARGET procaudio_fmod POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/procaudio_fmod.plugin.js"
          "${CMAKE_CURRENT_SOURCE_DIR}/procaudio_fmod_divider.png" $<TARGET_FILE_DIR:procaudio_fmod>)

# Integration test host: the real FMOD engine loads and plays the plugin
# (cpp/tests/fmod_host_check.py runs it and checks the result).
add_executable(procaudio_fmod_host fmod_host_test.cpp)
target_include_directories(procaudio_fmod_host PRIVATE "${FMOD_SDK_DIR}/api/core/inc")
target_link_libraries(procaudio_fmod_host PRIVATE "${FMOD_SDK_DIR}/api/core/lib/x64/fmod_vc.lib")
add_custom_command(TARGET procaudio_fmod_host POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${FMOD_SDK_DIR}/api/core/lib/x64/fmod.dll" $<TARGET_FILE_DIR:procaudio_fmod_host>)
add_dependencies(procaudio_fmod_host procaudio_fmod)
