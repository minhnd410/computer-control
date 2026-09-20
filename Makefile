SHELL := /bin/sh

BUILD_DIR ?= build
PYTHON ?= python3
VENV_DIR ?= .venv
CLANG_FORMAT ?= $(VENV_DIR)/bin/clang-format
CLANG_FORMAT_VERSION ?= 18.1.8

.PHONY: all install-deps lint build test clean

all: build test

install-deps:
	@case "$$(uname -s)" in \
		Darwin) \
			command -v brew >/dev/null 2>&1 || { echo "Homebrew is required on macOS: https://brew.sh" >&2; exit 1; }; \
			command -v python3 >/dev/null 2>&1 || { echo "python3 is required on macOS" >&2; exit 1; }; \
			for formula in cmake ninja; do \
				brew list --formula "$$formula" >/dev/null 2>&1 || brew install "$$formula"; \
			done; \
			;; \
		Linux) \
			command -v apt-get >/dev/null 2>&1 || { echo "install-deps supports Debian/Ubuntu via apt-get" >&2; exit 1; }; \
			sudo apt-get update; \
			sudo apt-get install -y --no-install-recommends cmake ninja-build python3 python3-venv clang-format libx11-dev libxtst-dev libxrandr-dev libxfixes-dev zlib1g-dev; \
			;; \
		*) echo "unsupported host: $$(uname -s)" >&2; exit 1 ;; \
	esac
	$(PYTHON) -m venv $(VENV_DIR)
	$(VENV_DIR)/bin/python -m pip install --upgrade pip
	$(VENV_DIR)/bin/python -m pip install "clang-format==$(CLANG_FORMAT_VERSION)"

lint:
	@formatter="$(CLANG_FORMAT)"; \
	if [ ! -x "$$formatter" ]; then formatter="$${CLANG_FORMAT_FALLBACK:-clang-format}"; fi; \
	command -v "$$formatter" >/dev/null 2>&1 || { echo "clang-format not found; run 'make install-deps'" >&2; exit 1; }; \
	find include src tests -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.h' -o -name '*.mm' \) -print0 | \
		xargs -0 "$$formatter" --dry-run --Werror
	@if git grep -nIE '(BEGIN [A-Z ]*PRIVATE KEY|AKIA[0-9A-Z]{16}|ghp_[A-Za-z0-9]{36})' -- . ':!.github/workflows/ci.yml'; then \
		echo "credential-looking string found" >&2; exit 1; \
	fi

build:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) --config Release -j 4

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure -C Release

clean:
	cmake --build $(BUILD_DIR) --target clean
