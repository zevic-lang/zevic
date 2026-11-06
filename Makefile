CC      ?= clang
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
SRC     := $(wildcard zevic0/*.c)
OBJ     := $(patsubst zevic0/%.c,build/obj/%.o,$(SRC))
BIN     := build/zevic
PM      := build/zevpm
PMSRC   := $(wildcard zevpm/*.c) zevic0/util.c
VERSION := $(shell cat VERSION)

.PHONY: all test clean install zevpm2 zevfmt fmt fmt-check zevic1

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

$(PM2): $(BIN) $(wildcard tools/zevpm/src/*.zev) tools/zevpm/src/Version.zev $(wildcard packages/Zevic/src/*.zev) runtime/zevic_rt.c
	./$(BIN) tools/zevpm/src/*.zev -o $@

zevpm2: $(PM2)

# the formatter, written in Zevic (tools/zevfmt)
FMT     := build/zevfmt
$(FMT): $(BIN) $(wildcard tools/zevfmt/src/*.zev) $(wildcard packages/Zevic/src/*.zev) runtime/zevic_rt.c
	./$(BIN) tools/zevfmt/src/*.zev -o $@

zevfmt: $(FMT)

# the compiler written in Zevic (tools/zevic1), built by zevic0
ZEVIC1  := build/zevic1
$(ZEVIC1): $(BIN) $(wildcard tools/zevic1/src/*.zev) $(wildcard packages/Zevic/src/*.zev) runtime/zevic_rt.c
	./$(BIN) tools/zevic1/src/*.zev -o $@

zevic1: $(ZEVIC1)

FMT_DIRS := packages examples tools

fmt: $(FMT)
	./$(FMT) -w $(FMT_DIRS)

fmt-check: $(FMT)
	./$(FMT) -check $(FMT_DIRS)

test: $(BIN) $(PM) $(PM2) $(FMT) $(ZEVIC1)
	python3 tests/run.py
	python3 tests/zevpm_test.py
	ZEVPM=$(CURDIR)/$(PM2) python3 tests/zevpm_test.py
	python3 tests/examples_test.py
	python3 tests/zevfmt_test.py ./$(BIN) ./$(FMT)
	python3 tests/zevic1_test.py ./$(BIN) ./$(ZEVIC1) tokens ast modules types gentypes

clean:
	rm -rf build
