#==============================================================================
# @file    CMakeLists.txt
# @brief   Build script for the WatchTower firmware.
#
# Built with GNU Make, not Ninja. The first build selects it, later builds
# reuse it:  idf.py -G "Unix Makefiles" build   (tools/run.sh does this)
#
#   cmake --build build/s3-16mb --target check  cppcheck, fails on any finding
#   cmake --build build/s3-16mb --target format clang-format + Prettier (runs before builds)
#   cmake --build build/s3-16mb --target docs   Doxygen HTML in build/s3-16mb/docs/html
#==============================================================================

cmake_minimum_required(VERSION 3.16)

# The generator is fixed before this file is read, so a build directory set up
# for Ninja (idf.py's default when installed) is refused instead.
if(CMAKE_GENERATOR MATCHES "Ninja")
    message(FATAL_ERROR
        "This project is built with GNU Make, not Ninja.\n"
        "Delete the build directory and configure it again:\n"
        "  rm -rf build\n"
        "  idf.py -G \"Unix Makefiles\" build")
endif()

# Firmware version from version.txt, as MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]
# (semver.org). Changing the file re-runs CMake.
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/version.txt FW_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS version.txt)

set(SEMVER_REGEX
    "^((0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*))(-[0-9A-Za-z.-]+)?(\\+[0-9A-Za-z.-]+)?$")
if(NOT FW_VERSION MATCHES "${SEMVER_REGEX}")
    message(FATAL_ERROR "version.txt: '${FW_VERSION}' is not a semantic "
                        "version (MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD])")
endif()

# ESP-IDF embeds PROJECT_VER in the application image (esp_app_desc_t); the
# web console shows it as the firmware version.
set(PROJECT_VER ${FW_VERSION})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(WatchTower)


set(PROJECT_DESCRIPTION "WiFi-connected WS2812 7-segment desk clock")

# The application code: main/ plus every component under components/.
set(APP_COMPONENTS
    main wt_app_led wt_app_log wt_app_settings wt_app_sound wt_app_time wt_app_web
    wt_app_wifi)
set(APP_SOURCE_DIRS ${CMAKE_SOURCE_DIR}/main ${CMAKE_SOURCE_DIR}/components)

#------------------------------------------------------------------------------
# Tools
#------------------------------------------------------------------------------
# A target whose tool is missing still exists, but fails with a clear message.
function(add_missing_tool_target name tool hint)
    add_custom_target(${name}
        COMMAND ${CMAKE_COMMAND} -E echo "error: ${tool} not found (e.g. '${hint}')"
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
    message(WARNING "${tool} not found - the '${name}' target will fail")
endfunction()

# unusedFunction is suppressed: callbacks and public APIs have no caller here.
# missingInclude is suppressed: ESP-IDF headers are not on cppcheck's path.
set(CPPCHECK_INCLUDES -I${CMAKE_SOURCE_DIR}/main)
file(GLOB COMPONENT_INCLUDE_DIRS LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/components/*/include)
foreach(dir ${COMPONENT_INCLUDE_DIRS})
    list(APPEND CPPCHECK_INCLUDES -I${dir})
endforeach()

find_program(CPPCHECK cppcheck)
if(CPPCHECK)
    add_custom_target(check
        COMMAND ${CPPCHECK} --enable=all --std=c99 --inline-suppr --error-exitcode=1
                --suppress=missingIncludeSystem --suppress=missingInclude
                --suppress=unusedFunction --suppress=checkersReport
                ${CPPCHECK_INCLUDES} ${APP_SOURCE_DIRS}
        VERBATIM
    )
else()
    add_missing_tool_target(check cppcheck "sudo apt install cppcheck")
endif()

add_custom_target(format
    COMMAND ${CMAKE_SOURCE_DIR}/tools/format.sh
    VERBATIM
)

# Format the sources before every build. Files that are already formatted are
# left untouched, so this does not trigger needless recompiles.
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    foreach(component ${APP_COMPONENTS})
        idf_component_get_property(component_lib ${component} COMPONENT_LIB)
        add_dependencies(${component_lib} format)
    endforeach()
else()
    message(WARNING "clang-format not found - sources will not be formatted "
                    "during the build")
endif()

# Doxygen reads a space-separated list of input directories.
string(REPLACE ";" " " SRCDIR "${APP_SOURCE_DIRS}")
configure_file(tools/config/Doxyfile.in ${CMAKE_BINARY_DIR}/Doxyfile @ONLY)

find_program(DOXYGEN doxygen)
if(DOXYGEN)
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/docs
        COMMAND ${DOXYGEN} ${CMAKE_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM
    )
else()
    add_missing_tool_target(docs doxygen "sudo apt install doxygen")
endif()
