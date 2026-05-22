#
# Copyright 2026, Unikie
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.8.2)

project(ConsoleDebugBatchSink C)

DeclareCAmkESComponent(
    ConsoleDebugBatchSink
    SOURCES
    src/console_debug_batch_sink.c
)
