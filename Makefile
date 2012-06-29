# Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Simple Makefile for rollog.

include common.mk

rollog_OBJS= main.o

clean: CLEAN(rollog)
all: CC_BINARY(rollog)

CC_BINARY(rollog): $(rollog_OBJS)
