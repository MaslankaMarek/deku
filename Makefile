# SPDX-License-Identifier: Apache-2.0
# Copyright (C) Semihalf, 2022
# Author: Marek Maślanka <mm@semihalf.com>

.PHONY: deploy

all: mklivepatch elfutils

WORKDIR=
ifdef workdir
	WORKDIR=-w $(workdir)
endif

CC ?= gcc
CFLAG ?= -Werror -Wall -Wpedantic -Wextra -Wno-gnu-zero-variadic-macro-arguments

ELFUTILS_FLAGS= $(CFLAG) -lelf
ifdef SUPPORT_DISASSEMBLY
	ELFUTILS_FLAGS=-DSUPPORT_DISASSEMBLY -lopcodes
endif

mklivepatch: mklivepatch.c
	$(CC) mklivepatch.c $(CFLAG) -lelf -o $@

elfutils: elfutils.c
	$(CC) elfutils.c $(ELFUTILS_FLAGS) -o $@

clean:
	rm -f mklivepatch elfutils

deploy:
	$(warning Using DEKU with "make deploy" is deprecated and will be removed soon. Instead, use the "./deku deploy" command.)
	./deku $(WORKDIR) deploy

build:
	$(warning Using DEKU with "make build" is deprecated and will be removed soon. Instead, use the "./deku build" command.)
	./deku $(WORKDIR) build

sync:
	$(warning Using DEKU with "make sync" is deprecated and will be removed soon. Instead, use the "./deku sync" command.)
	./deku $(WORKDIR) sync
