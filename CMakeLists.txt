cmake_minimum_required(VERSION 3.30)
project(PardusJarvis VERSION 1.4.5 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Quick QuickControls2 LinguistTools)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WEBRTC_APM REQUIRED webrtc-audio-processing-1)
include(FetchContent)
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio
    GIT_TAG 0.11.25
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/miniaudio
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(miniaudio)
FetchContent_Declare(
    Lowwi
    GIT_REPOSITORY https://github.com/CLFML/lowwi
    GIT_TAG v3.0
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/Lowwi
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(Lowwi)
FetchContent_Declare(
    whisper
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp
    GIT_TAG v1.9.1
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/whisper
    EXCLUDE_FROM_ALL
)
find_package(Vulkan QUIET COMPONENTS glslc)
if(Vulkan_FOUND AND Vulkan_glslc_FOUND)
    set(GGML_VULKAN ON CACHE BOOL "" FORCE)
else()
    set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
endif()
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(whisper)
add_executable(PardusJarvis app/main.cpp app/backend.h app/backend.cpp app/audio.cpp app/whisper_wrapper.h app/whisper_wrapper.cpp app/audio_processor.h app/audio_processor.cpp)
add_executable(JarvisDaemon daemon/main.cpp)
add_executable(JarvisSettings settings/main.cpp settings/backend.h settings/backend.cpp)
set_source_files_properties(app/audio_processor.cpp PROPERTIES COMPILE_OPTIONS "-std=c++17")
qt_add_resources(PardusJarvis "assets"
    PREFIX "/"
    FILES
        app/main.qml
        images/mic.svg
)
qt_add_resources(JarvisSettings "settings_assets"
    PREFIX "/"
    BASE settings
    FILES
        settings/settings.qml
        settings/fonts/Inter_18pt-Black.ttf
        settings/fonts/PlusJakartaSans-Bold.ttf
)
qt_add_translations(JarvisSettings TS_FILES settings/settings_tr.ts)
set_target_properties(PardusJarvis PROPERTIES
    OUTPUT_NAME "Pardus Jarvis"
    INSTALL_RPATH "$ORIGIN"
)
set_target_properties(JarvisDaemon PROPERTIES
    OUTPUT_NAME "Pardus-Jarvis-Daemon"
    INSTALL_RPATH "$ORIGIN"
)
set_target_properties(JarvisSettings PROPERTIES
    OUTPUT_NAME "Pardus-Jarvis-Settings"
    INSTALL_RPATH "$ORIGIN"
)
target_include_directories(PardusJarvis PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/whisper/include
    ${CMAKE_SOURCE_DIR}/lib/miniaudio
    ${WEBRTC_APM_INCLUDE_DIRS}
)
target_include_directories(JarvisDaemon PRIVATE ${CMAKE_SOURCE_DIR}/lib/miniaudio)
target_include_directories(JarvisSettings PRIVATE ${CMAKE_SOURCE_DIR}/lib/miniaudio)
target_link_libraries(PardusJarvis PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Quick
    whisper
    ${WEBRTC_APM_LIBRARIES}
)
target_link_libraries(JarvisDaemon PRIVATE CLFML::lowwi)
target_link_libraries(JarvisSettings PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
)
add_custom_command(TARGET JarvisDaemon POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/lib/Lowwi/models
        $<TARGET_FILE_DIR:JarvisDaemon>/models
)
set(WHISPER_MODEL_PATH ${CMAKE_SOURCE_DIR}/ggml-small-q5_1.bin)
if(NOT EXISTS ${WHISPER_MODEL_PATH})
    message(STATUS "Downloading ggml-small-q5_1.bin...")
    file(DOWNLOAD
        https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin
        ${WHISPER_MODEL_PATH}
        SHOW_PROGRESS
    )
endif()
add_custom_command(TARGET PardusJarvis POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${WHISPER_MODEL_PATH}
        $<TARGET_FILE_DIR:PardusJarvis>/ggml-small-q5_1.bin
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/sounds
        $<TARGET_FILE_DIR:PardusJarvis>/sounds
)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
install(TARGETS PardusJarvis JarvisDaemon JarvisSettings
    RUNTIME DESTINATION lib/pardusjarvis
)
install(TARGETS whisper ggml ggml-base ggml-cpu lowwi_lib
    LIBRARY DESTINATION lib/pardusjarvis NAMELINK_SKIP
)
if(Vulkan_FOUND AND Vulkan_glslc_FOUND)
    install(TARGETS ggml-vulkan
        LIBRARY DESTINATION lib/pardusjarvis NAMELINK_SKIP)
endif()
file(GLOB LOWWI_ONNXRUNTIME ${CMAKE_SOURCE_DIR}/lib/Lowwi/lib/x64/lib/libonnxruntime.so*)
install(FILES ${LOWWI_ONNXRUNTIME}
    DESTINATION lib/pardusjarvis)
install(FILES ${WHISPER_MODEL_PATH}
    DESTINATION lib/pardusjarvis)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/lib/Lowwi/models
    DESTINATION lib/pardusjarvis)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/sounds
    DESTINATION lib/pardusjarvis)
file(WRITE ${CMAKE_BINARY_DIR}/packaging/pardusjarvis-daemon.desktop
[=[[Desktop Entry]
Type=Application
Name=Pardus Jarvis Daemon
Exec=env QT_QPA_PLATFORM=xcb /usr/lib/pardusjarvis/Pardus-Jarvis-Daemon
NoDisplay=true
X-GNOME-Autostart-enabled=true
]=])
install(FILES ${CMAKE_BINARY_DIR}/packaging/pardusjarvis-daemon.desktop
    DESTINATION /etc/xdg/autostart)
install(FILES ${CMAKE_SOURCE_DIR}/images/mic.svg
    DESTINATION share/icons/hicolor/scalable/apps
    RENAME pardusjarvis.svg)
file(WRITE ${CMAKE_BINARY_DIR}/packaging/pardusjarvis-settings.desktop
[=[[Desktop Entry]
Type=Application
Name=Pardus Jarvis
Name[tr]=Pardus Jarvis
GenericName=Assistant Settings
GenericName[tr]=Asistan Ayarları
Comment=Configure the Pardus Jarvis voice assistant
Comment[tr]=Pardus Jarvis sesli asistanını yapılandır
Icon=pardusjarvis
Exec=/usr/lib/pardusjarvis/Pardus-Jarvis-Settings
Categories=Settings;Utility;
Keywords=jarvis;pardus;voice;assistant;asistan;ses;
]=])
install(FILES ${CMAKE_BINARY_DIR}/packaging/pardusjarvis-settings.desktop
    DESTINATION share/applications)
file(WRITE ${CMAKE_BINARY_DIR}/packaging/postinst
[=[#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
    for u in $(loginctl list-users --no-legend 2>/dev/null | awk '{print $2}'); do
        uid=$(id -u "$u" 2>/dev/null) || continue
        real_env=""
        for p in /proc/[0-9]*; do
            [ "$(stat -c %u "$p" 2>/dev/null)" = "$uid" ] || continue
            grep -q "^DBUS_SESSION_BUS_ADDRESS=" "$p/environ" 2>/dev/null || continue
            real_env=$(tr '\0' '\n' < "$p/environ" | grep -E '^(DBUS_SESSION_BUS_ADDRESS|WAYLAND_DISPLAY|XDG_RUNTIME_DIR|PULSE_SERVER|DISPLAY)=')
            break
        done
        runuser -u "$u" -- env $real_env XDG_RUNTIME_DIR=/run/user/$uid QT_QPA_PLATFORM=xcb \
            /usr/lib/pardusjarvis/Pardus-Jarvis-Settings >/dev/null 2>&1 &
        runuser -u "$u" -- env $real_env XDG_RUNTIME_DIR=/run/user/$uid \
            /usr/lib/pardusjarvis/Pardus-Jarvis-Daemon >/dev/null 2>&1 || true
    done
fi
exit 0
]=])
file(WRITE ${CMAKE_BINARY_DIR}/packaging/prerm
[=[#!/bin/sh
pkill -f "Pardus-Jarvis-Daemon" 2>/dev/null || true
exit 0
]=])
file(CHMOD ${CMAKE_BINARY_DIR}/packaging/postinst ${CMAKE_BINARY_DIR}/packaging/prerm
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_BINARY_DIR}/packaging/postinst;${CMAKE_BINARY_DIR}/packaging/prerm")
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "pardusjarvis")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Ahmet <ahmetcikko@gmail.com>")
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libqt6core6, libqt6gui6, libqt6network6, libqt6qml6, libqt6quick6, libqt6quickcontrols2-6, libwebrtc-audio-processing-1-3, qml6-module-qtquick, qml6-module-qtquick-window, qml6-module-qtquick-controls, qml6-module-qtquick-templates, procps, polkitd, xdg-utils, pulseaudio-utils")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "libvulkan1")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "PardusJarvis - Voice Assistant for Pardus Linux")
include(CPack)
