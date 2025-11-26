.DEFAULT_GOAL := all

CC := $(if $(CROSS_COMPILE),$(CROSS_COMPILE)gcc,gcc)

CFLAGS  ?= -Wall -O2
LDFLAGS ?=
LDLIBS  ?= -lm

TARGET := dummy_task
SRC    := dummy_task.c
OBJ    := $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	@echo "Linking $@"
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	@echo "Compiling $<"
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	@echo "Running: ./$(TARGET) $(ARGS)"
	@./$(TARGET) $(ARGS)

clean:
	@echo "Cleaning"
	@rm -f $(TARGET) $(OBJ)

.PHONY: all run clean
