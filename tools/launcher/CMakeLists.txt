cmake_minimum_required(VERSION 3.20)
project(mgpu_launcher CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: it runs on a machine with no VC++ runtime, like nrcheck.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(mgpu_launcher WIN32 src/launcher.cpp src/launcher.rc)
set_target_properties(mgpu_launcher PROPERTIES OUTPUT_NAME "MGPU Bridge Launcher")
target_compile_definitions(mgpu_launcher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(mgpu_launcher PRIVATE user32 gdi32 comctl32 shell32 ole32 shlwapi)

# The manifest (common controls v6, DPI aware) goes in through the linker,
# which embeds one of its own anyway - a second one from a .rc is CVT1100.
target_link_options(mgpu_launcher PRIVATE
    "/MANIFEST:EMBED"
    "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/launcher.manifest")
