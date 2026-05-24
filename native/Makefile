# Default to g++-15 (brew, macOS dev) only when Make is using its builtin.
# Env/command-line still override (CI passes CXX=g++ via env).
ifeq ($(origin CXX), default)
override CXX := g++-15
endif

SDK_PATH := $(shell xcrun --show-sdk-path 2>/dev/null)
CXXFLAGS = -O2 -Wall -std=c++26 $(if $(SDK_PATH),-isysroot $(SDK_PATH)) -I_compat

BIN_DIR = bin

# "all" is the default target
all: $(BIN_DIR)/k_means

$(BIN_DIR)/query: query.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BIN_DIR)/bench: bench.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $<

# Standalone HTTP server (replaces Puma+Ruby). Links picohttpparser.c
# directly. -march=native locally; CI/Docker should set CXX_MARCH explicitly.
$(BIN_DIR)/server: CXXFLAGS += -O2 -march=native
# Static-link libstdc++/libgcc so the binary is portable across glibcxx versions
# (lets us use a tiny runtime image like debian:bookworm-slim regardless of
# which gcc the build container had).
$(BIN_DIR)/server: LDFLAGS += -static-libstdc++ -static-libgcc
$(BIN_DIR)/server: server.cpp fraud_payload.hpp ivf_index.hpp vendor/picohttpparser/picohttpparser.h vendor/picohttpparser/picohttpparser.c | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ server.cpp vendor/picohttpparser/picohttpparser.c $(LDFLAGS)

# k_means runs offline on the build machine — safe to use -march=native
$(BIN_DIR)/k_means: CXXFLAGS += -march=native
$(BIN_DIR)/k_means: k_means.cpp ivf_index.hpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $<

references.json: ../resources/references.json.gz
	gunzip -k -c $< > $@

$(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(BIN_DIR)

.PHONY: all clean

 # ┌─────┬────────────────────────┐
 #  │     │      meaning           │
 #  ├─────┼────────────────────────┤
 #  │ $@  │ the rule's target      │
 #  ├─────┼────────────────────────┤
 #  │ $<  │ the first prerequisite │
 #  ├─────┼────────────────────────┤
 #  │ $^  │ all prerequisites      │
