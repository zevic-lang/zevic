CC      ?= clang
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
SRC     := $(wildcard zevic0/*.c)
OBJ     := $(patsubst zevic0/%.c,build/obj/%.o,$(SRC))
BIN     := build/zevic
PM      := build/zevpm
PMSRC   := $(wildcard zevpm/*.c) zevic0/util.c
VERSION := $(shell cat VERSION)

.PHONY: all test clean install

all: $(BIN) $(PM)

build/obj/%.o: zevic0/%.c $(wildcard zevic0/*.h) VERSION
	@mkdir -p build/obj
	$(CC) $(CFLAGS) -DZEVIC_VERSION='"$(VERSION)"' -c $< -o $@

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@

$(PM): $(PMSRC) $(wildcard zevpm/*.h) zevic0/util.h VERSION
	@mkdir -p build
	$(CC) $(CFLAGS) -DZEVPM_VERSION='"$(VERSION)"' $(PMSRC) -o $@

test: $(BIN) $(PM)
	python3 tests/run.py
	python3 tests/zevpm_test.py
	python3 tests/examples_test.py

clean:
	rm -rf build
