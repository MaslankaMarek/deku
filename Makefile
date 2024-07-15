# Author: Marek Maślanka
# Project: DEKU
# URL: https://github.com/MarekMaslanka/deku

all: mklivepatch.o mklivepatch elfutils.o elfutils symbolindex deku

C_DEBUG_FLAG=
GO_DEBUG_FLAG=
ifdef debug
	C_DEBUG_FLAG=-g
	GO_DEBUG_FLAG=-gcflags=all="-N -l"
endif

CC ?= gcc
CFLAG ?= -Wno-gnu-zero-variadic-macro-arguments
CFLAG := $(C_DEBUG_FLAG)

ELFUTILS_FLAGS= $(CFLAG) -lelf

mklivepatch.o: src/mklivepatch.c
	$(CC) $< -c -DUSE_AS_LIB $(CFLAG) -lelf -o $@

mklivepatch: src/mklivepatch.c
	$(CC) $< $(CFLAG) -lelf -o $@

elfutils.o: src/libelfutils.c
	$(shell echo "void t() { init_disassemble_info(NULL, 0, NULL); }" | \
			$(CC) -DPACKAGE=1 -include dis-asm.h -S -o - -x c - > /dev/null 2>&1)
	$(CC) $< $(ELFUTILS_FLAGS) -c -DUSE_AS_LIB -DDISASSEMBLY_STYLE_SUPPORT=$(.SHELLSTATUS) -o $@

elfutils: src/elfutils.c
	$(shell echo "void t() { init_disassemble_info(NULL, 0, NULL); }" | \
			$(CC) -DPACKAGE=1 -include dis-asm.h -S -o - -x c - > /dev/null 2>&1)
	$(CC) $< elfutils.o -lopcodes -lbfd -lz -liberty $(ELFUTILS_FLAGS) -DDISASSEMBLY_STYLE_SUPPORT=$(.SHELLSTATUS) -o $@

symbolindex: src/symbolindex.c
	$(CC) $< $(CFLAG) -Wno-unused-function -lelf -o $@

deku:
	go clean -cache
	go build $(GO_DEBUG_FLAG) -o $@ src/go/*.go

clean:
	rm -f mklivepatch.o mklivepatch elfutils.o elfutils symbolindex deku
