CC      ?= clang
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
SRC     := $(wildcard zevic0/*.c)
OBJ     := $(patsubst zevic0/%.c,build/obj/%.o,$(SRC))
BIN     := build/zevic
PM      := build/zevpm
PMSRC   := $(wildcard zevpm/*.c) zevic0/util.c
VERSION := $(shell cat VERSION)

.PHONY: all test clean install zevpm2

all: $(BIN) $(PM)

build/obj/%.o: zevic0/%.c $(wildcard zevic0/*.h) VERSION
	@mkdir -p build/obj
	$(CC) $(CFLAGS) -DZEVIC_VERSION='"$(VERSION)"' -c $< -o $@

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@

$(PM): $(PMSRC) $(wildcard zevpm/*.h) zevic0/util.h VERSION
	@mkdir -p build
	$(CC) $(CFLAGS) -DZEVPM_VERSION='"$(VERSION)"' $(PMSRC) -o $@

# zevpm written in Zevic (tools/zevpm), built by zevic
PM2     := build/zevpm2
tools/zevpm/src/Version.zev: VERSION
	@printf 'module Zevpm.Version\n\n// Generated from the file VERSION by `make` (do not edit).\nlet Current: String = "%s"\n' "$(VERSION)" > $@

$(PM2): $(BIN) $(wildcard tools/zevpm/src/*.zev) tools/zevpm/src/Version.zev $(wildcard stdlib/*.zev) runtime/zevic_rt.c
	./$(BIN) tools/zevpm/src/*.zev -o $@

zevpm2: $(PM2)

test: $(BIN) $(PM) $(PM2)
	python3 tests/run.py
	python3 tests/zevpm_test.py
	ZEVPM=$(CURDIR)/$(PM2) python3 tests/zevpm_test.py
	python3 tests/examples_test.py

clean:
	rm -rf build
