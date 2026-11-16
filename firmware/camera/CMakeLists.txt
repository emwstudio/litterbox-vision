cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common/components/catvision_net")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(catvision_camera)
