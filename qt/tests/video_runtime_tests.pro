QT += core gui testlib

CONFIG += testcase console c++17
CONFIG -= app_bundle

TEMPLATE = app
TARGET = video_runtime_tests

SOURCES += \
    tst_video_runtime.cpp \
    ../VideoRuntime.cpp

HEADERS += \
    ../VideoRuntime.h
