cmake_minimum_required(VERSION 3.23)
project(outriced LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# ---------------------------------------------------------------- SDL3
FetchContent_Declare(
    SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.8/SDL3-3.2.8.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(SDL3)

# ---------------------------------------------------------------- Dear ImGui
# Not a CMake project, so we fetch the sources and build them ourselves.
FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.8.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

# ---------------------------------------------------------------- nlohmann/json
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------- app
add_executable(outriced WIN32)

target_sources(outriced PRIVATE
    src/app/main.cpp
    src/app/settings.cpp
    src/capture/audio_capture.cpp
    src/capture/video_capture.cpp
    src/capture/recorder.cpp
    src/media/sessions.cpp
    src/media/clipper.cpp
    src/platform/subprocess.cpp
    src/platform/tray.cpp
    src/media/player.cpp
    src/app/ui.cpp
    src/app/ui_common.cpp
    src/app/ui_library.cpp
    src/app/ui_preview.cpp
    src/app/ui_settings.cpp
    src/capture/displays.cpp
    src/capture/encoders.cpp
    src/capture/game_watch.cpp
    src/platform/overlay.cpp
    src/app/dialogs.cpp
    src/platform/log.cpp
    src/capture/audio_devices.cpp
    src/app/app.manifest
)

target_include_directories(outriced PRIVATE src)

target_link_libraries(outriced PRIVATE
    SDL3::SDL3
    imgui
    nlohmann_json::nlohmann_json
    ole32          # COM, for WASAPI
    shell32        # tray icon
    user32
    avrt           # MMCSS, keeps the audio thread scheduled promptly
)

target_compile_definitions(outriced PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(outriced PRIVATE /W4 /permissive-)

    # Symbols in Release too. Without a .pdb a crash dump gives raw addresses
    # for our own frames and nothing else, which is exactly why the first real
    # crash reports could not be traced past the faulting instruction.
    #
    # /Zi only emits debug information; it does not weaken optimisation. /DEBUG
    # defaults the linker to /OPT:NOREF, which would leave dead code in the
    # binary, so the two /OPT flags put that back.
    target_compile_options(outriced PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(outriced PRIVATE
        $<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)

    # WIN32 executable has no console; use main() as the entry point anyway.
    set_target_properties(outriced PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        LINK_FLAGS "/ENTRY:mainCRTStartup"
    )
endif()

# ---------------------------------------------------------------- libmpv
# Prebuilt shinchiro package, unpacked to third_party/mpv by tools/fetch_mpv.ps1.
set(MPV_DIR ${CMAKE_SOURCE_DIR}/third_party/mpv)
if(EXISTS ${MPV_DIR}/include/mpv/client.h AND EXISTS ${MPV_DIR}/mpv.lib)
    target_include_directories(outriced PRIVATE ${MPV_DIR}/include)
    target_link_libraries(outriced PRIVATE ${MPV_DIR}/mpv.lib)
    target_compile_definitions(outriced PRIVATE OC_HAVE_MPV=1)
    add_custom_command(TARGET outriced POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MPV_DIR}/libmpv-2.dll
        $<TARGET_FILE_DIR:outriced>
    )
else()
    message(WARNING "libmpv not found in ${MPV_DIR} -- building without playback. Run tools/fetch_mpv.ps1.")
    target_compile_definitions(outriced PRIVATE OC_HAVE_MPV=0)
endif()

add_custom_command(TARGET outriced POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:SDL3::SDL3>
    $<TARGET_FILE_DIR:outriced>
)

# ---------------------------------------------------------------- test harness
# Console app that drives the recorder without the GUI.
add_executable(record_test
    tests/record_test.cpp
    src/capture/recorder.cpp
    src/capture/audio_capture.cpp
    src/capture/video_capture.cpp
    src/capture/audio_devices.cpp
    src/platform/subprocess.cpp
    src/app/settings.cpp
    src/capture/displays.cpp
    src/capture/encoders.cpp
    src/platform/log.cpp
)
target_include_directories(record_test PRIVATE src)
target_link_libraries(record_test PRIVATE nlohmann_json::nlohmann_json ole32 avrt d3d11 dxgi)
target_compile_definitions(record_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(clip_test
    tests/clip_test.cpp
    src/media/clipper.cpp
    src/media/sessions.cpp
    src/platform/subprocess.cpp
    src/app/settings.cpp
    src/capture/displays.cpp
    src/capture/encoders.cpp
    src/platform/log.cpp
)
target_include_directories(clip_test PRIVATE src)
target_link_libraries(clip_test PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(clip_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(mpv_test
    tests/mpv_test.cpp
    src/media/player.cpp
    src/media/sessions.cpp
    src/platform/subprocess.cpp
    src/app/settings.cpp
    src/capture/displays.cpp
    src/capture/encoders.cpp
    src/platform/log.cpp
)
target_include_directories(mpv_test PRIVATE src)
target_link_libraries(mpv_test PRIVATE nlohmann_json::nlohmann_json user32)
target_compile_definitions(mpv_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(EXISTS ${MPV_DIR}/include/mpv/client.h AND EXISTS ${MPV_DIR}/mpv.lib)
    target_include_directories(mpv_test PRIVATE ${MPV_DIR}/include)
    target_link_libraries(mpv_test PRIVATE ${MPV_DIR}/mpv.lib)
    target_compile_definitions(mpv_test PRIVATE OC_HAVE_MPV=1)
    add_custom_command(TARGET mpv_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MPV_DIR}/libmpv-2.dll $<TARGET_FILE_DIR:mpv_test>
    )
else()
    target_compile_definitions(mpv_test PRIVATE OC_HAVE_MPV=0)
endif()
