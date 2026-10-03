CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -pthread
LDFLAGS += -pthread -ldl

SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:.c=.o)
TARGET := quec_battery
TEST_SOURCES := $(filter-out src/main.c,$(SOURCES))
TEST_TARGET := tests/behavior_test
ARCH ?= aarch64_cortex-a53

.PHONY: all clean test ipk

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

src/%.o: src/%.c src/qb.h
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): tests/behavior.c $(TEST_SOURCES) src/qb.h
	$(CC) $(CFLAGS) -Isrc tests/behavior.c $(TEST_SOURCES) $(LDFLAGS) -o $@

# Package the already-built target binary. Cross-compile first, e.g.
#   make clean && make CC=aarch64-linux-musl-gcc && make ipk
ipk: $(TARGET)
	ARCH=$(ARCH) VERSION=$(VERSION) ./packaging/build-ipk.sh

clean:
	rm -f $(OBJECTS) $(TARGET) $(TEST_TARGET)
