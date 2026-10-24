# Lu Compiler v1.3 - Makefile

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic \
	  -Wno-unused-parameter -Wno-unused-function \
	  -Wno-overlength-strings \
	  -g -O2
TARGET  = luc
SRCS    = main.c lexer.c parser.c semantic.c codegen.c libraries.c util.c x86.c v5.c includes.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean test test-all test-cli test-native test-v5 test-ports test-gui test-motion bootstrap doctor agent-test fuzz

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^
	@echo "  LD  $@"

%.o: %.c lu.h
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c -o $@ $<

# Quick self-test: compile the bundled Classic example.
test: $(TARGET) ../examples/classic/example.lu
	./$(TARGET) -s ../examples/classic/example.lu -o example_out.c
	$(CC) -O2 -std=c11 -o example_out example_out.c -lm
	./example_out

# Shipped Lu programs covered by test-all.  This is deliberately an explicit
# list, not a glob: untracked scratch files in src/ must not break the floor.
# calculator_gui.lu is built by `make test-gui` (it needs a display).
TEST_PROGRAMS = ../examples/classic/example.lu \
                ../examples/classic/showcase.lu \
                ../examples/classic/test_comprehensive.lu \
                ../examples/classic/test_core_plus.lu \
                ../examples/classic/test_full.lu \
                kernel_advanced.lu kernel_sim.lu lu_compiler.lu \
                messenger.lu native_demo.lu test_paradigms.lu \
                test_v13.lu test_v20.lu ../tests/calculator.lu

# Full regression pass over bundled Lu programs.
test-all: $(TARGET) test-cli test-native test-v5 test-ports
	@rm -rf .lu_test_build
	@mkdir -p .lu_test_build
	@set -e; \
	for f in $(TEST_PROGRAMS); do \
	        base=$$(basename $$f .lu); \
	        echo "[LU TEST] $$f"; \
	        ./$(TARGET) $$f -o .lu_test_build/$$base.c >/dev/null; \
	        $(CC) -O2 -std=c11 -I. -I../include -o .lu_test_build/$$base .lu_test_build/$$base.c -lm; \
	        .lu_test_build/$$base >/dev/null; \
	done
	@echo "[LU TEST] all bundled Lu programs compiled and ran"

# CLI validation and file I/O error regressions.
test-cli: $(TARGET)
	sh ../tests/cli_test.sh

# Native backend parity: each tests/native program must produce the same
# stdout and exit status through `luc -n` and through `luc -c` + cc.
test-native: $(TARGET)
	sh ../tests/native_test.sh

# Lu/Strict v5: every good program builds and runs, every bad one is
# rejected with the documented diagnostic.
test-v5: $(TARGET)
	sh ../tests/v5_test.sh

# Shipped programs ported to v5: each port must match the Classic original's
# stdout and exit status on every backend listed in tests/ports/backends.txt.
test-ports: $(TARGET)
	sh ../tests/ports/run.sh

# GUI demo: compile + link the X11 calculator (no run — needs a display).
test-gui: $(TARGET) calculator_gui.lu
	./$(TARGET) calculator_gui.lu -o .lu_test_build/calculator_gui.c
	$(CC) -O2 -std=c11 -o .lu_test_build/calculator_gui .lu_test_build/calculator_gui.c -lX11
	@echo "[LU TEST] calculator_gui compiled and linked (run: .lu_test_build/calculator_gui)"

# The bundled LuMotion application: builds both binaries and renders frames.
# Needs libX11 at link time; the renderer checks themselves are headless.
test-motion: $(TARGET)
	sh ../motion/test.sh

# C-only local agents: syntax, C backend and runtime regression checks.
doctor: $(TARGET) lu_doctor.c
	$(CC) $(CFLAGS) -o lu_doctor lu_doctor.c

agent-test: doctor
	./lu_doctor

# Fuzz testing — manual, not part of agent-test. Generates random
# programs and checks luc doesn't crash or hang. Run: make fuzz [ROUNDS=100]
fuzz: $(TARGET)
	./fuzz_test.sh $${ROUNDS:-100}

# lu_compiler.lu is an input, not a target: if it is listed as a
# prerequisite while a stale lu_compiler.lu.c exists, make's built-in
# "%: %.c" rule compiles that artifact over the source.
bootstrap: $(TARGET) lu_self_runtime.h
	./bootstrap.sh

clean:
	rm -rf .lu_test_build .lu_doctor
	rm -f $(OBJS) $(TARGET) lu_doctor *.c.out example_out.c example_out lu_compiler.c lu_compiler.lu.c lu_compiler.lu.c.2 lu_compiler.self1.c lu_compiler.self2.c luc_self luc_self2
