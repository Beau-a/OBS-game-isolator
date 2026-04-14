cmake_minimum_required(VERSION 3.16)
project(obs-pipewire-extended VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# OBS
find_package(libobs REQUIRED)

# PipeWire / SPA (SPA headers are bundled with PipeWire's pkg-config)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

# Plugin target
add_library(obs-pipewire-extended MODULE
    src/plugin-main.c
    src/pipewire-audio.c
)

target_include_directories(obs-pipewire-extended PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
)

target_compile_options(obs-pipewire-extended PRIVATE
    ${PIPEWIRE_CFLAGS_OTHER}
    -Wall
    -Wextra
    -Wno-unused-parameter
)

target_link_libraries(obs-pipewire-extended PRIVATE
    OBS::libobs
    ${PIPEWIRE_LIBRARIES}
)

# Installation paths
if(NOT DEFINED OBS_PLUGIN_DESTINATION)
    set(OBS_PLUGIN_DESTINATION /usr/lib/obs-plugins)
endif()

install(TARGETS obs-pipewire-extended
    LIBRARY DESTINATION ${OBS_PLUGIN_DESTINATION}
)
