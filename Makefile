# SPDX-License-Identifier: Apache-2.0
# Copyright (C) Semihalf, 2022
# Author: Marek Maślanka <mm@semihalf.com>

.PHONY: deploy

all: mklivepatch elfutils

WORKDIR=
ifdef workdir
	WORKDIR=-w $(workdir)
endif

ELFUTILS_FLAGS= -Werror -Wall -Wpedantic -Wextra -lelf
ifdef SUPPORT_DISASSEMBLY
	ELFUTILS_FLAGS=-DSUPPORT_DISASSEMBLY -lopcodes
endif

mklivepatch: mklivepatch.c
	gcc mklivepatch.c -Werror -Wall -Wpedantic -Wextra -lelf -o mklivepatch

elfutils: elfutils.c
	gcc elfutils.c $(ELFUTILS_FLAGS) -o elfutils

clean:
	rm -f mklivepatch elfutils

deploy:
	./deku $(WORKDIR) deploy

build:
	./deku $(WORKDIR) build

sync:
	./deku $(WORKDIR) sync
