#  Copyright (c) Microsoft Corporation. All rights reserved.
#  Licensed under the MIT License.

cmake_minimum_required (VERSION 3.10)

project (wolfMQTTSphere C)

azsphere_configure_tools(TOOLS_REVISION "22.02")
azsphere_configure_api(TARGET_API_SET "12")

include_directories(../src)
include_directories(../wolfmqtt)
include_directories(..)
add_subdirectory(.. wolfMQTT)

# Create executable

add_executable (${PROJECT_NAME} main.c)
target_link_libraries (${PROJECT_NAME} applibs pthread gcc_s c wolfMQTT)


azsphere_target_add_image_package(${PROJECT_NAME})
