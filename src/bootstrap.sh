#!/bin/bash
set -euo pipefail

echo "=== Lu Self-Hosting Bootstrap ==="

echo "--- Step 1: Build luc (C compiler) ---"
make

echo "--- Step 2: Compile lu_compiler.lu -> lu_compiler.c ---"
./luc lu_compiler.lu -o lu_compiler.c

echo "--- Step 3: Build luc_self from generated C ---"
gcc -O2 -std=c11 -Wall -Wextra -Wno-unused-function -Wno-misleading-indentation -Wno-stringop-truncation -I. -o luc_self lu_compiler.c

echo "--- Step 4: luc_self compiles lu_compiler.lu -> lu_compiler.lu.c ---"
./luc_self lu_compiler.lu
cp lu_compiler.lu.c lu_compiler.self1.c

echo "--- Step 5: Build luc_self2 from self-compiled output ---"
gcc -O2 -std=c11 -Wall -Wextra -Wno-unused-function -Wno-misleading-indentation -Wno-stringop-truncation -I. -o luc_self2 lu_compiler.lu.c

echo "--- Step 6: luc_self2 compiles lu_compiler.lu -> lu_compiler.lu.c ---"
./luc_self2 lu_compiler.lu
cp lu_compiler.lu.c lu_compiler.self2.c

echo "--- Step 7: Bootstrap verification ---"
if diff -q lu_compiler.self1.c lu_compiler.self2.c >/dev/null; then
    echo "BOOTSTRAP VERIFIED - output is identical"
else
    echo "Outputs differ"
    diff -u lu_compiler.self1.c lu_compiler.self2.c | sed -n '1,120p'
    exit 1
fi

echo "=== Done ==="
