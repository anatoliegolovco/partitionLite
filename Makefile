CC      ?= cc
CSTD    := $(shell $(CC) -std=c23 -E -x c /dev/null >/dev/null 2>&1 \
            && echo -std=c23 || echo -std=c2x)

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    EXT       := dylib
    SHFLAGS   := -dynamiclib -undefined dynamic_lookup
else
    EXT       := so
    SHFLAGS   := -shared
endif

TARGET  := ts_partition.$(EXT)
SRC     := src/ts_partition.c
CFLAGS  := $(CSTD) -O2 -Wall -Wextra -Wpedantic -fPIC -fvisibility=hidden
LDLIBS  :=

DATA_DIR := test/data

.PHONY: all clean distclean fixtures test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SHFLAGS) -o $@ $< $(LDLIBS)

fixtures: $(DATA_DIR)/.stamp

$(DATA_DIR)/.stamp: test/gen_fixtures.sh
	./test/gen_fixtures.sh "$(DATA_DIR)"
	@touch $@

test: $(TARGET) fixtures
	./test/smoke_test.sh

clean:
	rm -f $(TARGET) src/*.o

distclean: clean
	rm -rf $(DATA_DIR)
