cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME rws)

find_package(OpenSSL REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# cpp-httplib ships inside DuckDB's third_party tree. We compile our own copy
# with OpenSSL support because the one DuckDB links has TLS disabled.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/duckdb/third_party/httplib/httplib.hpp")
  include_directories(${CMAKE_CURRENT_LIST_DIR}/duckdb/third_party/httplib)
elseif(EXISTS "${CMAKE_SOURCE_DIR}/third_party/httplib/httplib.hpp")
  include_directories(${CMAKE_SOURCE_DIR}/third_party/httplib)
else()
  message(FATAL_ERROR "Could not locate duckdb/third_party/httplib/httplib.hpp")
endif()

set(EXTENSION_SOURCES
    src/rws_extension.cpp
    src/rws_secret.cpp
    src/rws_url.cpp
    src/rws_client.cpp
    src/rws_xml.cpp
    src/rws_odm.cpp
    src/rws_request.cpp
    src/functions/rws_table_functions.cpp
    src/storage/rws_catalog.cpp
    src/storage/rws_storage.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)

if(APPLE)
  # cpp-httplib reads the trust store from the macOS keychain.
  target_link_libraries(${EXTENSION_NAME} "-framework CoreFoundation" "-framework Security")
  target_link_libraries(${LOADABLE_EXTENSION_NAME} "-framework CoreFoundation" "-framework Security")
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
