# Unity native plug-in: a plain C API over procaudio-core (procaudio_unity.cpp).
# The Unity demo (Unity Demo/ProceduralAudioDemo) loads it from
# Assets/Plugins/x86_64. The Editor keeps a loaded DLL locked: close the
# Editor (or the project) before rebuilding, or the copy step fails.
add_library(procaudio_unity SHARED procaudio_unity.cpp)
target_link_libraries(procaudio_unity PRIVATE procaudio_core)

set(PROCAUDIO_UNITY_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/../../../Unity Demo/ProceduralAudioDemo"
    CACHE PATH "Unity project that receives procaudio_unity.dll")
if(EXISTS "${PROCAUDIO_UNITY_PROJECT}/Assets")
  add_custom_command(TARGET procaudio_unity POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROCAUDIO_UNITY_PROJECT}/Assets/Plugins/x86_64"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:procaudio_unity>
            "${PROCAUDIO_UNITY_PROJECT}/Assets/Plugins/x86_64/")
endif()
