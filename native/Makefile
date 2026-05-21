CXX = g++
CXXFLAGS = -O2 -Wall -std=c++26

BIN_DIR = bin

# "all" é o alvo default
all: $(BIN_DIR)/knn

$(BIN_DIR)/knn: knn.cpp | $(BIN_DIR) # deps
	$(CXX) $(CXXFLAGS) -o $@ $^ # command

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
