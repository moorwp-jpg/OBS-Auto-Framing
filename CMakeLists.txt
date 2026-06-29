cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version} LANGUAGES CXX)

option(BUILD_PLUGIN "Build the OBS plugin module" ON)
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)
option(ENABLE_TESTS "Build unit tests" ON)
set(ONNXRUNTIME_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime"
    CACHE PATH "Path to the extracted ONNX Runtime win-x64 package")
set(DEFAULT_YOLOX_MODEL_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/data/models"
    CACHE PATH "Directory containing bundled YOLOX ONNX models")
set(OBS_DEV_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/obs"
    CACHE PATH "Project-local OBS development files with include/ and lib/ folders")

include(compilerconfig)
include(defaults)
include(helpers)

add_library(obs-auto-framing-core STATIC)
target_sources(
    obs-auto-framing-core
    PRIVATE
        src/crop_controller.cpp
        src/iou_tracker.cpp
        src/mock_detector.cpp
        src/tracker/assignment.cpp
        src/tracker/bytetrack_tracker.cpp
        src/yolox_postprocess.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS src
        FILES
            src/crop_controller.hpp
            src/detector.hpp
            src/geometry.hpp
            src/letterbox.hpp
            src/settings.hpp
            src/tracker/assignment.hpp
            src/tracker/bytetrack_tracker.hpp
            src/tracker.hpp
            src/yolox_postprocess.hpp
            src/yuv.hpp)
target_compile_features(obs-auto-framing-core PUBLIC cxx_std_17)

if(BUILD_PLUGIN)
    add_library(${CMAKE_PROJECT_NAME} MODULE)

    set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_ROOT}/include")
    set(ONNXRUNTIME_LIBRARY "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
    set(ONNXRUNTIME_DLL "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll")

    if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h"
       OR NOT EXISTS "${ONNXRUNTIME_LIBRARY}"
       OR NOT EXISTS "${ONNXRUNTIME_DLL}")
        message(
            FATAL_ERROR
                "ONNX Runtime is required for the OBS plugin build. Run scripts/download_onnxruntime.ps1, "
                "or set ONNXRUNTIME_ROOT to an extracted onnxruntime-win-x64 package containing include/, lib/onnxruntime.lib, and lib/onnxruntime.dll.")
    endif()

    set(DEFAULT_YOLOX_TINY_MODEL "${DEFAULT_YOLOX_MODEL_DIR}/yolox_tiny.onnx")
    if(NOT EXISTS "${DEFAULT_YOLOX_TINY_MODEL}")
        message(
            FATAL_ERROR
                "YOLOX-Tiny model is missing. Run scripts/download_yolox_model.ps1 -Model tiny, "
                "or set DEFAULT_YOLOX_MODEL_DIR to a directory containing yolox_tiny.onnx.")
    endif()

    set(OBS_DEV_INCLUDE_ROOT "${OBS_DEV_ROOT}/include")
    set(OBS_DEV_LIBOBS_ROOT "${OBS_DEV_INCLUDE_ROOT}/libobs")
    set(OBS_DEV_LIBRARY "${OBS_DEV_ROOT}/lib/obs.lib")
    set(
        OBS_DEV_INCLUDE_DIRS
        "${OBS_DEV_INCLUDE_ROOT}"
        "${OBS_DEV_LIBOBS_ROOT}"
        "${OBS_DEV_LIBOBS_ROOT}/graphics"
        "${OBS_DEV_LIBOBS_ROOT}/media-io"
        "${OBS_DEV_LIBOBS_ROOT}/util")

    foreach(
        OBS_DEV_REQUIRED_PATH
        IN ITEMS
            "${OBS_DEV_LIBOBS_ROOT}/obs-module.h"
            "${OBS_DEV_LIBOBS_ROOT}/obs.h"
            "${OBS_DEV_LIBOBS_ROOT}/obs-config.h"
            "${OBS_DEV_LIBOBS_ROOT}/obsconfig.h"
            "${OBS_DEV_LIBOBS_ROOT}/graphics"
            "${OBS_DEV_LIBOBS_ROOT}/media-io"
            "${OBS_DEV_LIBOBS_ROOT}/util"
            "${OBS_DEV_LIBRARY}")
        if(NOT EXISTS "${OBS_DEV_REQUIRED_PATH}")
            message(
                FATAL_ERROR
                    "OBS development files are incomplete. Missing: ${OBS_DEV_REQUIRED_PATH}\n"
                    "Run scripts/import_obs_dev_files.ps1 -ObsRoot <OBS-source-tree> "
                    "-ObsBuildRoot <OBS-build_x64-directory>, or set OBS_DEV_ROOT to a directory with include/libobs and lib/obs.lib.")
        endif()
    endforeach()

    add_library(OBS::libobs UNKNOWN IMPORTED)
    set_target_properties(
        OBS::libobs
        PROPERTIES
            IMPORTED_LOCATION "${OBS_DEV_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OBS_DEV_INCLUDE_DIRS}")
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
    target_link_libraries(
        ${CMAKE_PROJECT_NAME}
        PRIVATE OBS::libobs obs-auto-framing-core "${ONNXRUNTIME_LIBRARY}")
    target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)
    target_compile_definitions(
        ${CMAKE_PROJECT_NAME}
        PRIVATE
            PLUGIN_NAME="${_name}"
            PLUGIN_VERSION="${PROJECT_VERSION}"
            PLUGIN_RELEASE_CHANNEL="${_release_channel}"
            HAVE_ONNXRUNTIME=1
            DEFAULT_YOLOX_MODEL_DIR="${DEFAULT_YOLOX_MODEL_DIR}")

    if(ENABLE_FRONTEND_API)
        find_package(obs-frontend-api REQUIRED)
        target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
    endif()

    if(ENABLE_QT)
        find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
        target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
        set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
    endif()

    target_sources(
        ${CMAKE_PROJECT_NAME}
        PRIVATE
            src/auto-framing-filter.cpp
            src/crop_renderer.cpp
            src/crop_renderer.hpp
            src/onnx_person_detector.cpp
            src/onnx_person_detector.hpp
            src/plugin-main.cpp)

    add_custom_command(
        TARGET ${CMAKE_PROJECT_NAME}
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
        COMMENT "Copying ONNX Runtime DLL beside ${CMAKE_PROJECT_NAME}")

    set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

    install(FILES "${ONNXRUNTIME_DLL}" DESTINATION "obs-plugins/64bit")
endif()

if(ENABLE_TESTS)
    enable_testing()
    add_executable(crop_controller_tests tests/crop_controller_tests.cpp)
    target_link_libraries(crop_controller_tests PRIVATE obs-auto-framing-core)
    add_test(NAME crop_controller_tests COMMAND crop_controller_tests)

    add_executable(bytetrack_tracker_tests tests/bytetrack_tracker_tests.cpp)
    target_link_libraries(bytetrack_tracker_tests PRIVATE obs-auto-framing-core)
    add_test(NAME bytetrack_tracker_tests COMMAND bytetrack_tracker_tests)

    add_executable(yolox_postprocess_tests tests/yolox_postprocess_tests.cpp)
    target_link_libraries(yolox_postprocess_tests PRIVATE obs-auto-framing-core)
    add_test(NAME yolox_postprocess_tests COMMAND yolox_postprocess_tests)

    add_executable(yuv_tests tests/yuv_tests.cpp)
    target_link_libraries(yuv_tests PRIVATE obs-auto-framing-core)
    add_test(NAME yuv_tests COMMAND yuv_tests)
endif()
