# Compiler and standard
cc = g++
std = -std=c++17

include_path = -I ./src/
includes = -lboost_context

# Output directory
output = ./bin/

LIB_HEADERS = ./src/co_lib.hpp

EXAMPLE_SOURCES = $(wildcard examples/*.cpp)
EXAMPLE_TARGETS = $(patsubst examples/%.cpp,$(output)%,$(EXAMPLE_SOURCES))

CFLAGS_EXAMPLES = $(std) $(include_path)
LDFLAGS_EXAMPLES = $(includes) 


all: $(EXAMPLE_TARGETS)

examples: $(EXAMPLE_TARGETS)

# --- Example Rule ---

$(output)%: examples/%.cpp $(LIB_HEADERS)
	@echo "Compiling example: $@"
	@mkdir -p $(output)
	$(cc) $(CFLAGS_EXAMPLES) $< $(LDFLAGS_EXAMPLES) -o $@

# --- Clean Rule ---

clean:
	@echo "Cleaning build files..."
	rm -f $(EXAMPLE_TARGETS)
.PHONY: all examples clean
