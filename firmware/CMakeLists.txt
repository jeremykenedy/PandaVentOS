cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The image identifies itself as the factory app so the stock updater and this
# updater accept each other's images in both directions.
project(panda_vent VERSION 1.0.0)
