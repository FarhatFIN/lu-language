#ifndef LU_X86_H
#define LU_X86_H

#include "lu.h"

/* ─────────────────────────────────────────────
   Native x86-64 backend.

   Lowers the Lu AST straight to machine code and writes a static
   ELF64 executable for Linux x86-64. No libc, no gcc, no as/ld:
   the runtime is written in Lu itself and talks to the kernel
   through raw syscalls.

   Returns false when the program uses a construct the native
   backend cannot lower yet (classes, Vector<T>, f-strings with
   runtime parts, floats, X11, tensors). Those still go through
   the C backend with `-c`.
   ───────────────────────────────────────────── */
bool native_compile(ASTNode *root, const char *out_path, const char *src_name);

#endif /* LU_X86_H */
