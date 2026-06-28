set(_buildspec_path "${CMAKE_CURRENT_LIST_DIR}/../../buildspec.json")
if(NOT EXISTS "${_buildspec_path}")
    message(FATAL_ERROR "Missing build metadata: ${_buildspec_path}")
endif()

file(READ "${_buildspec_path}" _buildspec_json)
string(JSON _name GET "${_buildspec_json}" name)
string(JSON _version GET "${_buildspec_json}" version)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_CURRENT_LIST_DIR}")
