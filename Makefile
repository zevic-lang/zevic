CC      ?= clang
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
SRC     := $(wildcard zevic0/*.c)
OBJ     := $(patsubst zevic0/%.c,build/obj/%.o,$(SRC))
BIN     := build/zevic

.PHONY: all test clean install

all: $(BIN)

build/obj/%.o: zevic0/%.c $(wildcard zevic0/*.h)
	@mkdir -p build/obj
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@

test: $(BIN)
	python3 tests/run.py

clean:
	rm -rf build
