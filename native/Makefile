CXX = g++
CXXFLAGS = -O2 -Wall -std=c++26

BIN_DIR = bin

# "all" é o alvo default
all: $(BIN_DIR)/k_means

$(BIN_DIR)/k_means: k_means.cpp | $(BIN_DIR) # deps
	$(CXX) $(CXXFLAGS) -o $@ $< # command

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
