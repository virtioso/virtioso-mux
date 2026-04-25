#
# Copyright 2026, Unikie
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.8.2)

project(ConsolePassthroughSink C)

DeclareCAmkESComponent(
    ConsolePassthroughSink
    SOURCES
    src/console_passthrough_sink.c
)
