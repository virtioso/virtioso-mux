#
# Copyright 2026, Unikie
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.8.2)

project(PassthruSink C)

DeclareCAmkESComponent(
    PassthruSink
    SOURCES
    src/console_passthrough_sink.c
)
