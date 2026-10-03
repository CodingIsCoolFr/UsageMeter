cmake_minimum_required(VERSION 3.21)

set(VCPKG_TARGET_TRIPLET x64-windows)
set(CMAKE_TOOLCHAIN_FILE "$ENV{USERPROFILE}/vcpkg/scripts/buildsystems/vcpkg.cmake"
    CACHE STRING "" FORCE)

project(UsageMeter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(imgui CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(UsageMeter WIN32
    src/main.cpp
    src/http.cpp
    src/jsonutil.cpp
    src/store.cpp
    src/providers.cpp
    src/import.cpp
    src/ui.cpp)

target_include_directories(UsageMeter PRIVATE src)

target_link_libraries(UsageMeter PRIVATE
    imgui::imgui
    glfw
    OpenGL::GL
    CURL::libcurl
    nlohmann_json::nlohmann_json
    crypt32
    bcrypt
    shell32
    ws2_32
    dwmapi)

if(MSVC)
    target_compile_options(UsageMeter PRIVATE /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX)
    target_compile_definitions(UsageMeter PRIVATE CURL_STATICLIB _CRT_SECURE_NO_WARNINGS)
endif()
