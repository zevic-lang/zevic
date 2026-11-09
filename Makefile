CC      ?= clang
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
SRC     := $(wildcard zevic0/*.c)
OBJ     := $(patsubst zevic0/%.c,build/obj/%.o,$(SRC))
BIN     := build/zevic
PM      := build/zevpm
PMSRC   := $(wildcard zevpm/*.c) zevic0/util.c
VERSION := $(shell cat VERSION)

.PHONY: test-interop all test clean install zevpm2 zevfmt fmt fmt-check zevic1 bootstrap bootstrap-update test-selfhost

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

FMT_DIRS := packages examples tools

fmt: $(FMT)
	./$(FMT) -w $(FMT_DIRS)

test-interop: $(BIN)
	sh examples/interop/run.sh $(BIN)

fmt-check: $(FMT)
	./$(FMT) -check $(FMT_DIRS)

# the test runner (tests/zevtest.c) is C too
TESTER  := build/zevtest
$(TESTER): tests/zevtest.c
	@mkdir -p build
	$(CC) -std=c11 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -pthread $< -o $@

test: $(BIN) $(PM) $(PM2) $(FMT) $(TESTER)
	ZEVIC=$(BIN) ZEVPM=$(PM) ZEVFMT=$(FMT) $(TESTER) golden
	ZEVIC=$(BIN) ZEVPM=$(PM) ZEVFMT=$(FMT) $(TESTER) zevpm
	ZEVIC=$(BIN) ZEVPM=$(PM2) ZEVFMT=$(FMT) $(TESTER) zevpm
	ZEVIC=$(BIN) ZEVPM=$(PM2) ZEVFMT=$(FMT) $(TESTER) examples
	ZEVIC=$(BIN) ZEVPM=$(PM2) ZEVFMT=$(FMT) $(TESTER) zevfmt
	ZEVIC=$(BIN) ZEVPM=$(PM2) ZEVFMT=$(FMT) $(TESTER) install

clean:
	rm -rf build

# ---- self-hosting: zevic1 is the compiler written in Zevic (tools/zevic1) ----
# zevic (C) builds zevic1; zevic1 builds zevic2; zevic2 builds zevic3. All of them must write the same LLVM IR,
# and that IR is kept in bootstrap/zevic.ll, so a C compiler for Zevic is no longer needed to rebuild the compiler.
Z1SRC   := $(sort $(wildcard tools/zevic1/src/*.zev))
Z1      := build/zevic1
Z2      := build/zevic2

tools/zevic1/src/Version.zev: VERSION
	@printf 'module Zevic1.Version\n\n// Generated from the file VERSION by `make` (do not edit).\nlet Current: String = "%s"\n' "$(VERSION)" > $@

$(Z1): $(BIN) $(Z1SRC) tools/zevic1/src/Version.zev $(wildcard packages/Zevic/src/*.zev) runtime/zevic_rt.c
	./$(BIN) $(Z1SRC) -o $@

$(Z2): $(Z1)
	./$(Z1) $(Z1SRC) -o $@

zevic1: $(Z1)

# build zevic1 from the committed IR with clang alone (no zevic0)
build/zevic1-from-ll: bootstrap/zevic.ll runtime/zevic_rt.c
	./tools/bootstrap-build.sh $@

bootstrap: $(Z2)
	./$(Z1) --emit-ll $(Z1SRC) -o build/zevic1.ll
	./$(Z2) --emit-ll $(Z1SRC) -o build/zevic2.ll
	cmp build/zevic1.ll build/zevic2.ll
	cmp build/zevic2.ll bootstrap/zevic.ll
	@echo "bootstrap: zevic1 and zevic2 write identical LLVM IR, equal to bootstrap/zevic.ll"

# refresh bootstrap/zevic.ll after the compiler changed (then run `make bootstrap`)
bootstrap-update: $(Z2)
	./$(Z2) --emit-ll $(Z1SRC) -o bootstrap/zevic.ll

# the test suite with the self-hosted compiler
test-selfhost: $(Z2) $(PM2) $(FMT) $(TESTER)
	ZEVIC=$(Z2) ZEVPM=$(PM2) ZEVFMT=$(FMT) $(TESTER) golden
