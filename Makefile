default: all

all:
	@$(MAKE) -C src
	@$(MAKE) -C benchmarks

.PHONY: all default test

clean:
	@$(MAKE) -C src clean
	@$(MAKE) -C benchmarks clean
