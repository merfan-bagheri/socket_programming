all:
	$(MAKE) -C phase_1
	$(MAKE) -C phase_2

clean:
	$(MAKE) -C phase_1 clean
	$(MAKE) -C phase_2 clean

test: all
	bash tests/run_tests.sh

.PHONY: all clean test
