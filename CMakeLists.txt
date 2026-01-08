# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(002)

# Embed web files as binary data after project is defined
target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "${CMAKE_SOURCE_DIR}/main/web/index.html" TEXT)
target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "${CMAKE_SOURCE_DIR}/main/web/style.css" TEXT)
target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "${CMAKE_SOURCE_DIR}/main/web/script.js" TEXT)
# Embed favicon.ico if it exists (optional)
if(EXISTS "${CMAKE_SOURCE_DIR}/main/web/favicon.ico")
    target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "${CMAKE_SOURCE_DIR}/main/web/favicon.ico" BINARY)
endif()

# Embed TLS certificate/key (replace with your real files if needed)
target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "${CMAKE_SOURCE_DIR}/main/certs/server_cert.pem" TEXT)
target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "${CMAKE_SOURCE_DIR}/main/certs/server_key.pem" TEXT)

