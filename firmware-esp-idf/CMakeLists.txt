cmake_minimum_required(VERSION 3.16)

# ESP-IDF project for MedGasAlarm.
# Build:  idf.py set-target esp32 && idf.py build
# Flash:  idf.py -p /dev/ttyUSB0 flash monitor
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(medgas_alarm)
