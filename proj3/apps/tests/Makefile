CC := $(if $(CROSS_COMPILE),$(CROSS_COMPILE)gcc,gcc)
CFLAGS ?= -Wall -O2

TARGETS := rsv_probe test_budget test_period test_rm

.PHONY: all clean

all: $(TARGETS)

rsv_probe: rsv_probe.c
	@echo "Building $@"
	$(CC) $(CFLAGS) -o $@ $<

test_budget: test_budget.c
	@echo "Building $@"
	$(CC) $(CFLAGS) -o $@ $<

test_period: test_period.c
	@echo "Building $@"
	$(CC) $(CFLAGS) -o $@ $<

test_rm: test_rm.c
	@echo "Building $@"
	$(CC) $(CFLAGS) -o $@ $<

clean:
	@echo "Cleaning"
	@rm -f $(TARGETS)
