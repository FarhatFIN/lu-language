# Lu Compiler v1.3 - Makefile

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic \
	  -Wno-unused-parameter -Wno-unused-function \
	  -Wno-overlength-strings \
	  -g -O2
TARGET  = luc
SRCS    = main.c lexer.c parser.c semantic.c codegen.c util.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean test test-all test-gui bootstrap doctor agent-test fuzz

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^
	@echo "  LD  $@"

%.o: %.c lu.h
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c -o $@ $<

# Quick self-test: compile the bundled example.
test: $(TARGET) example.lu
	./$(TARGET) -s example.lu -o example_out.c
	$(CC) -O2 -std=c11 -o example_out example_out.c -lm
	./example_out

# Full regression pass over bundled Lu programs.
# calculator_gui.lu is excluded: it needs an X11 display and blocks
# waiting for events — build it with `make test-gui` instead.
test-all: $(TARGET)
	@rm -rf .lu_test_build
	@mkdir -p .lu_test_build
	@set -e; \
	for f in *.lu ../tests/*.lu; do \
	        base=$$(basename $$f .lu); \
	        if [ "$$base" = "calculator_gui" ]; then continue; fi; \
	        echo "[LU TEST] $$f"; \
	        ./$(TARGET) $$f -o .lu_test_build/$$base.c >/dev/null; \
	        $(CC) -O2 -std=c11 -I. -o .lu_test_build/$$base .lu_test_build/$$base.c -lm; \
	        .lu_test_build/$$base >/dev/null; \
	done
	@echo "[LU TEST] all bundled Lu programs compiled and ran"

# GUI demo: compile + link the X11 calculator (no run — needs a display).
test-gui: $(TARGET) calculator_gui.lu
	./$(TARGET) calculator_gui.lu -o .lu_test_build/calculator_gui.c
	$(CC) -O2 -std=c11 -o .lu_test_build/calculator_gui .lu_test_build/calculator_gui.c -lX11
	@echo "[LU TEST] calculator_gui compiled and linked (run: .lu_test_build/calculator_gui)"

# C-only local agents: syntax, C backend and runtime regression checks.
doctor: $(TARGET) lu_doctor.c
	$(CC) $(CFLAGS) -o lu_doctor lu_doctor.c

agent-test: doctor
	./lu_doctor

# Fuzz testing — manual, not part of agent-test. Generates random
# programs and checks luc doesn't crash or hang. Run: make fuzz [ROUNDS=100]
fuzz: $(TARGET)
	./fuzz_test.sh $${ROUNDS:-100}

bootstrap: $(TARGET) lu_compiler.lu lu_self_runtime.h
	./bootstrap.sh

clean:
	rm -rf .lu_test_build .lu_doctor
	rm -f $(OBJS) $(TARGET) lu_doctor *.c.out example_out.c example_out lu_compiler.c lu_compiler.lu.c lu_compiler.lu.c.2 lu_compiler.self1.c lu_compiler.self2.c luc_self luc_self2
