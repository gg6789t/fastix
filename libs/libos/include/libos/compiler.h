/* libos/compiler.h - small compiler helpers shared by libos and servers. */
#ifndef LIBOS_COMPILER_H
#define LIBOS_COMPILER_H

/* Callers must look at the result (AGENTS.md rule 4). */
#define LIBOS_MUST_CHECK __attribute__((warn_unused_result))

#endif /* LIBOS_COMPILER_H */
