/*
 * exec_helper.c - small embedded ELF program used by vmtest to verify execve,
 * #! script chains, vfork+exec, posix_spawn_fast, credentials, and pgid/sid.
 */
#include <stdint.h>

#include <fastix/abi.h>
#include <fastix_sys/sys.h>

_Noreturn void prog_main(uint64_t *sp);

static int str_eq(const char *a, const char *b)
{
    if (a == 0 || b == 0) {
        return 0;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static uint64_t aux_get(const uint64_t *aux, uint64_t key)
{
    for (; aux[0] != FASTIX_AT_NULL; aux += 2) {
        if (aux[0] == key) {
            return aux[1];
        }
    }
    return 0;
}

static uint32_t parse_u32(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

static int run_check(uint64_t *sp)
{
    uint64_t argc = sp[0];
    const char **argv = (const char **)(sp + 1);
    const char **envp = argv + argc + 1;
    const uint64_t *aux = (const uint64_t *)envp;
    while (*aux != 0) {
        aux++;
    }
    aux++;

    const struct fastix_procinfo *info = fastix_procinfo();
    if (argc < 2) {
        return 1;
    }
    if (str_eq(argv[1], "check_exec")) {
        if (argc != 3 || !str_eq(argv[2], "arg2")) return 2;
        if (envp[0] == 0 || !str_eq(envp[0], "FOO=bar") || envp[1] != 0) return 3;
        if (info->uid != 1000 || info->euid != 1001 || info->gid != 2000 || info->egid != 2001) {
            return 4;
        }
        if (aux_get(aux, FASTIX_AT_UID) != 1000 || aux_get(aux, FASTIX_AT_EUID) != 1001 ||
            aux_get(aux, FASTIX_AT_GID) != 2000 || aux_get(aux, FASTIX_AT_EGID) != 2001) {
            return 5;
        }
        return 77;
    }
    if (str_eq(argv[1], "-flag1")) {
        if (argc != 4) return 10;
        if (!str_eq(argv[0], "/bin/exec_helper")) return 11;
        if (!str_eq(argv[2], "/bin/sh_basic")) return 12;
        if (!str_eq(argv[3], "userarg")) return 13;
        return 88;
    }
    if (str_eq(argv[1], "a4")) {
        if (argc != 7) return 20;
        if (!str_eq(argv[0], "/bin/exec_helper")) return 21;
        if (!str_eq(argv[2], "/bin/sh_chain4")) return 22;
        if (!str_eq(argv[3], "/bin/sh_chain3")) return 23;
        if (!str_eq(argv[4], "/bin/sh_chain2")) return 24;
        if (!str_eq(argv[5], "/bin/sh_chain1")) return 25;
        if (!str_eq(argv[6], "end")) return 26;
        return 89;
    }
    if (str_eq(argv[1], "check_spawn")) {
        if (argc != 3) return 30;
        if (envp[0] == 0 || !str_eq(envp[0], "SPAWN=ok") || envp[1] != 0) return 31;
        uint32_t want_pgid = str_eq(argv[2], "self") ? info->pid : parse_u32(argv[2]);
        if (info->pgid != want_pgid) return 32;
        if (fastix_sys1(FASTIX_SYS_getpgid, 0) != (int64_t)want_pgid) return 33;
        return 90;
    }
    if (str_eq(argv[1], "check_vfork")) {
        return 91;
    }
    return 50;
}

_Noreturn void prog_main(uint64_t *sp)
{
    fastix_sys_init();
    (void)fastix_sys1(FASTIX_SYS_exit_group, (uint64_t)run_check(sp));
    for (;;) {
    }
}
