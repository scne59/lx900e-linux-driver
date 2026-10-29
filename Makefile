# Convenience targets; install.sh does the privileged work.

.PHONY: all test clean
all:
	$(MAKE) -C driver

test:
	tests/run.sh

clean:
	$(MAKE) -C driver clean
	rm -rf build
