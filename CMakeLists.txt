cmake_minimum_required(VERSION 3.16)
project(obs-streamtranslate C)

set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(OBS REQUIRED libobs)
pkg_check_modules(LWS REQUIRED libwebsockets)

add_library(obs-streamtranslate MODULE plugin.c)

target_include_directories(obs-streamtranslate PRIVATE ${OBS_INCLUDE_DIRS} ${LWS_INCLUDE_DIRS})
target_link_directories(obs-streamtranslate PRIVATE ${OBS_LIBRARY_DIRS} ${LWS_LIBRARY_DIRS})
target_link_libraries(obs-streamtranslate ${OBS_LIBRARIES} ${LWS_LIBRARIES} pthread dl)
target_compile_definitions(obs-streamtranslate PRIVATE _GNU_SOURCE)

set_target_properties(obs-streamtranslate PROPERTIES PREFIX "")
