default: all

all:
	@$(MAKE) -C src
	@$(MAKE) -C examples
	@$(MAKE) -C benchmarks

test:
	@$(MAKE) -C tests

.PHONY: all clean default test

clean:
	@$(MAKE) -C src clean
	@$(MAKE) -C tests clean
	@$(MAKE) -C benchmarks clean
	@$(MAKE) -C examples clean
