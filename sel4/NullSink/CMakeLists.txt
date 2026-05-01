#
# Copyright 2026, Unikie
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.8.2)

project(ConsoleNullBatchSink C)

DeclareCAmkESComponent(
    ConsoleNullBatchSink
    SOURCES
    src/console_null_batch_sink.c
)
