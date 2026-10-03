cmake_minimum_required(VERSION 3.21)

if(NOT CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{USERPROFILE})
    if(EXISTS "$ENV{USERPROFILE}/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{USERPROFILE}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    endif()
endif()
if(WIN32)
    set(VCPKG_TARGET_TRIPLET x64-windows CACHE STRING "")
endif()

project(UsageMeter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(imgui CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

set(SOURCES
    src/main.cpp src/http.cpp src/jsonutil.cpp src/store.cpp
    src/providers.cpp src/import.cpp src/ui.cpp)

if(WIN32)
    add_executable(UsageMeter WIN32 ${SOURCES})
else()
    add_executable(UsageMeter ${SOURCES})
endif()

target_include_directories(UsageMeter PRIVATE src)

target_link_libraries(UsageMeter PRIVATE
    imgui::imgui
    glfw
    OpenGL::GL
    CURL::libcurl
    nlohmann_json::nlohmann_json)

if(WIN32)
    target_link_libraries(UsageMeter PRIVATE crypt32 bcrypt shell32 ws2_32 dwmapi)
endif()
if(APPLE)
    target_link_libraries(UsageMeter PRIVATE "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation" "-framework Security")
endif()

if(MSVC)
    target_compile_options(UsageMeter PRIVATE /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX)
    target_compile_definitions(UsageMeter PRIVATE CURL_STATICLIB _CRT_SECURE_NO_WARNINGS)
endif()
