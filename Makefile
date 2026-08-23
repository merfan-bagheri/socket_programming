CC ?= gcc

all:
	$(MAKE) -C phase_1 CC="$(CC)"
	$(MAKE) -C phase_2 CC="$(CC)"

clean:
	$(MAKE) -C phase_1 clean
	$(MAKE) -C phase_2 clean

test: all
	bash tests/run_tests.sh

.PHONY: all clean test
