CC      ?= clang
# The compiler is written in Zevic (tools/zevic1). A seed compiler is built from the committed
# LLVM IR (bootstrap/zevic.ll) with clang alone; the seed builds the compiler from its sources, and
# `make bootstrap` checks that the result writes the very same IR. The C implementation that came
# before lives in zevic-lang/zevic-c.
VERSION := $(shell cat VERSION)
Z1SRC   := $(sort $(wildcard tools/zevic1/src/*.zev))
SEED    := build/zevic-seed
BIN     := build/zevic
export ZEVIC_HOME := $(CURDIR)

all: build/zevic build/zevpm2 build/zevfmt

.PHONY: test-interop all test clean install zevpm zevfmt fmt fmt-check bootstrap bootstrap-update

$(SEED): bootstrap/zevic.ll runtime/zevic_rt.c
	./tools/bootstrap-build.sh $@

tools/zevic1/src/Version.zev: VERSION
	@printf 'module Zevic1.Version\n\n// Generated from the file VERSION by `make` (do not edit).\nlet Current: String = "%s"\n' "$(VERSION)" > $@

$(BIN): $(SEED) $(Z1SRC) tools/zevic1/src/Version.zev $(wildcard packages/Zevic/src/*.zev) runtime/zevic_rt.c
	./$(SEED) $(Z1SRC) -o $@

# zevpm written in Zevic (tools/zevpm), built by zevic
PM2     := build/zevpm2
tools/zevpm/src/Version.zev: VERSION
	@printf 'module Zevpm.Version\n\n// Generated from the file VERSION by `make` (do not edit).\nlet Current: String = "%s"\n' "$(VERSION)" > $@

$(PM2): $(BIN) $(wildcard tools/zevpm/src/*.zev) tools/zevpm/src/Version.zev $(wildcard packages/Zevic/src/*.zev) runtime/zevic_rt.c
	./$(BIN) tools/zevpm/src/*.zev -o $@

zevpm: $(PM2)

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

test: $(BIN) $(PM2) $(FMT) $(TESTER)
	ZEVIC=$(BIN) ZEVPM=$(PM2) ZEVFMT=$(FMT) $(TESTER) golden
	ZEVIC=$(BIN) ZEVPM=$(PM2) ZEVFMT=$(FMT) $(TESTER) zevpm
	ZEVIC=$(BIN) ZEVPM=$(PM2) ZEVFMT=$(FMT) $(TESTER) examples
	ZEVIC=$(BIN) ZEVPM=$(PM2) ZEVFMT=$(FMT) $(TESTER) zevfmt
	ZEVIC=$(BIN) ZEVPM=$(PM2) ZEVFMT=$(FMT) $(TESTER) install

clean:
	rm -rf build

# ---- the fixed point ----
# The compiler built by the seed writes the IR the seed was made from. `make bootstrap` checks it (stage 2 too).
Z2      := build/zevic2

$(Z2): $(BIN)
	./$(BIN) $(Z1SRC) -o $@

bootstrap: $(Z2)
	./$(BIN) --emit-ll $(Z1SRC) -o build/zevic1.ll
	./$(Z2) --emit-ll $(Z1SRC) -o build/zevic2.ll
	cmp build/zevic1.ll build/zevic2.ll
	cmp build/zevic2.ll bootstrap/zevic.ll
	@echo "bootstrap: stage 1 and stage 2 write identical LLVM IR, equal to bootstrap/zevic.ll"

# refresh bootstrap/zevic.ll after the compiler changed (then run `make bootstrap`)
bootstrap-update: $(Z2)
	./$(Z2) --emit-ll $(Z1SRC) -o bootstrap/zevic.ll
