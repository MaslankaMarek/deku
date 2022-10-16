#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) Semihalf, 2022
# Author: Marek Maślanka <mm@semihalf.com>

echo "Show diff against the kernel installed on the device"

git --work-tree="$SOURCE_DIR" --git-dir="$workdir/.git" diff
