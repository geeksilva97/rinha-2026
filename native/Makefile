# Default to g++-15 (brew, macOS dev) só se Make estiver usando seu builtin.
# Env/command-line ainda overrideiam (CI passa CXX=g++ via env).
ifeq ($(origin CXX), default)
override CXX := g++-15
endif

SDK_PATH := $(shell xcrun --show-sdk-path 2>/dev/null)
CXXFLAGS = -O2 -Wall -std=c++26 $(if $(SDK_PATH),-isysroot $(SDK_PATH)) -I_compat

BIN_DIR = bin

# "all" é o alvo default
all: $(BIN_DIR)/k_means

$(BIN_DIR)/query: query.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BIN_DIR)/bench: bench.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $<

# k_means roda offline na máquina que compilou — pode usar -march=native
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
 #  │     │      significado       │
 #  ├─────┼────────────────────────┤
 #  │ $@  │ o alvo da regra        │
 #  ├─────┼────────────────────────┤
 #  │ $<  │ a primeira dependência │
 #  ├─────┼────────────────────────┤
 #  │ $^  │ todas as dependências  │
