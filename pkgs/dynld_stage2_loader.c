#include <elf.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>

/*
 * Freestanding stage2 replacement for dynld_stage2_loader.sh.
 *
 * Preserved constraints:
 *   - no libc
 *   - no shell for the main execution path
 *   - same two-stage protocol with dynld_stage1_loader
 *   - same argv / env contract with stage1
 *
 * Fidelity goals:
 *   - internalize the shell's realpath/which/patchelf behavior
 *   - keep DYNLD_ALLOW_LDD as an opt-in subprocess fallback, because the
 *     original semantics are explicitly defined in terms of ldd output
 */

typedef unsigned long usize;
typedef long isize;
typedef unsigned long long u64;
typedef long long i64;

#ifndef NULL
#define NULL ((void*)0)
#endif

#define PAGE_SIZE 4096UL
#define AT_FDCWD -100
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define O_PATH 010000000

static isize sys0(long n) {
    isize ret;
    __asm__ volatile("syscall" : "=a"(ret) : "0"(n) : "rcx", "r11", "memory");
    return ret;
}

static isize sys1(long n, usize a) {
    isize ret;
    __asm__ volatile("syscall" : "=a"(ret) : "0"(n), "D"(a) : "rcx", "r11", "memory");
    return ret;
}

static isize sys2(long n, usize a, usize b) {
    isize ret;
    __asm__ volatile("syscall" : "=a"(ret) : "0"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return ret;
}

static isize sys3(long n, usize a, usize b, usize c) {
    isize ret;
    __asm__ volatile("syscall" : "=a"(ret) : "0"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return ret;
}

static isize sys4(long n, usize a, usize b, usize c, usize d) {
    isize ret;
    register usize r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(ret) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}

static isize sys6(long n, usize a, usize b, usize c, usize d, usize e, usize f) {
    isize ret;
    register usize r10 __asm__("r10") = d;
    register usize r8  __asm__("r8") = e;
    register usize r9  __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(ret) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}

static usize c_strlen(const char *s) {
    usize n = 0;
    while (s && s[n]) n++;
    return n;
}

static int c_memcmp(const void *a, const void *b, usize n) {
    const unsigned char *aa = (const unsigned char*)a;
    const unsigned char *bb = (const unsigned char*)b;
    for (usize i = 0; i < n; i++) {
        if (aa[i] != bb[i]) return aa[i] - bb[i];
    }
    return 0;
}

static void *c_memcpy(void *dst, const void *src, usize n) {
    unsigned char *d = (unsigned char*)dst;
    const unsigned char *s = (const unsigned char*)src;
    for (usize i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static char *c_strchr(const char *s, int ch) {
    while (*s) {
        if (*s == ch) return (char*)s;
        s++;
    }
    return ch == 0 ? (char*)s : NULL;
}

static int c_startswith(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static int c_has_suffix(const char *s, const char *suffix) {
    usize ns = c_strlen(s);
    usize nf = c_strlen(suffix);
    return ns >= nf && c_memcmp(s + ns - nf, suffix, nf) == 0;
}

static int c_contains_char(const char *s, char ch) {
    while (*s) {
        if (*s++ == ch) return 1;
    }
    return 0;
}

static int c_contains(const char *hay, const char *needle) {
    usize nn = c_strlen(needle);
    if (!nn) return 1;
    for (; *hay; ++hay) {
        if (*hay == *needle && c_memcmp(hay, needle, nn) == 0) return 1;
    }
    return 0;
}

static void write2(const char *s) {
    sys3(SYS_write, 2, (usize)s, c_strlen(s));
}

static void die_msg(const char *s) {
    write2(s);
    sys1(SYS_exit, 1);
}

static void die_program_not_found(const char *prog) {
    write2("[dynld] Program ");
    write2(prog);
    write2(" is not on the path.\n");
    sys1(SYS_exit, 1);
}

static void die_loader_not_found(const char *prog) {
    write2("[dynld] Unable to find a loader for executable ");
    write2(prog);
    write2("\n");
    sys1(SYS_exit, 1);
}

static void *xalloc(usize n) {
    void *p;

    n = (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    p = (void*)sys6(SYS_mmap, 0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, (usize)-1, 0);
    if ((long)p < 0) die_msg("[dynld] mmap failed\n");
    return p;
}

static char *dup_cstr(const char *s) {
    usize n = c_strlen(s);
    char *r = (char*)xalloc(n + 1);
    c_memcpy(r, s, n + 1);
    return r;
}

static void utoa_dec(unsigned long v, char *buf, usize *len_out) {
    char tmp[32];
    usize n = 0;

    if (v == 0) {
        buf[0] = '0';
        *len_out = 1;
        return;
    }

    while (v) {
        tmp[n++] = '0' + (v % 10);
        v /= 10;
    }

    for (usize i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    *len_out = n;
}

static const char *getenv_raw(char **envp, const char *name) {
    usize n = c_strlen(name);
    for (; *envp; ++envp) {
        if (c_memcmp(*envp, name, n) == 0 && (*envp)[n] == '=') return *envp + n + 1;
    }
    return NULL;
}

static int path_is_executable_file(const char *path) {
    struct stat st;

    if (sys4(SYS_newfstatat, AT_FDCWD, (usize)path, (usize)&st, 0) < 0) return 0;
    if ((st.st_mode & S_IFMT) == S_IFDIR) return 0;
    if (sys2(SYS_access, (usize)path, X_OK) < 0) return 0;
    return 1;
}

static int path_exists(const char *path) {
    struct stat st;
    if (sys4(SYS_newfstatat, AT_FDCWD, (usize)path, (usize)&st, 0) < 0) return 0;
    return (st.st_mode & S_IFMT) != S_IFDIR;
}

static char *join_search_path_component(const char *dir, usize dir_len, const char *name) {
    usize name_len = c_strlen(name);

    if (dir_len == 0) {
        char *out = (char*)xalloc(2 + name_len + 1);
        out[0] = '.';
        out[1] = '/';
        c_memcpy(out + 2, name, name_len + 1);
        return out;
    }

    char *out = (char*)xalloc(dir_len + 1 + name_len + 1);
    c_memcpy(out, dir, dir_len);
    out[dir_len] = '/';
    c_memcpy(out + dir_len + 1, name, name_len + 1);
    return out;
}

static char *canonicalize_via_procfd(const char *path) {
    int fd = (int)sys4(SYS_openat, AT_FDCWD, (usize)path, O_PATH | O_CLOEXEC, 0);
    if (fd < 0) return dup_cstr("");

    char procfd[64] = "/proc/self/fd/";
    usize base = 14;
    usize n;
    char *out = (char*)xalloc(PATH_MAX + 1);
    isize r;

    utoa_dec((unsigned long)fd, procfd + base, &n);
    procfd[base + n] = 0;

    r = sys4(SYS_readlinkat, AT_FDCWD, (usize)procfd, (usize)out, PATH_MAX);
    sys1(SYS_close, fd);
    if (r < 0) return dup_cstr("");
    out[r] = 0;
    return out;
}

/*
 * Close to the shell's `which` behavior:
 *   - if name contains '/' we test it directly and return it verbatim when
 *     executable
 *   - PATH is split on ':'
 *   - empty path element becomes ./name
 *   - no extra canonicalization for PATH hits
 *   - if PATH is absent or empty, resolution fails
 */
static char *resolve_via_path_like(const char *name, const char *path_value) {
    if (!name[0]) return dup_cstr("");

    if (c_contains_char(name, '/')) return path_is_executable_file(name) ? dup_cstr(name) : dup_cstr("");

    if (!path_value || !path_value[0]) return dup_cstr("");

    const char *seg = path_value;
    while (1) {
        const char *colon = c_strchr(seg, ':');
        usize seg_len = colon ? (usize)(colon - seg) : c_strlen(seg);
        char *candidate = join_search_path_component(seg, seg_len, name);
        if (path_is_executable_file(candidate)) return candidate;
        if (!colon) break;
        seg = colon + 1;
    }

    return dup_cstr("");
}

static char *search_existing_in_path(const char *name, const char *path_value, int require_exec) {
    if (!name[0] || !path_value || !path_value[0]) return dup_cstr("");

    const char *seg = path_value;
    while (1) {
        const char *colon = c_strchr(seg, ':');
        usize seg_len = colon ? (usize)(colon - seg) : c_strlen(seg);
        char *candidate = join_search_path_component(seg, seg_len, name);
        if (require_exec ? path_is_executable_file(candidate) : path_exists(candidate)) return candidate;
        if (!colon) break;
        seg = colon + 1;
    }

    return dup_cstr("");
}

static char *resolve_executable(char **envp, const char *arg) {
    if (arg[0] == '/') return canonicalize_via_procfd(arg);
    return resolve_via_path_like(arg, getenv_raw(envp, "PATH"));
}

static int read_exact(int fd, void *buf, usize n, u64 off) {
    unsigned char *p = (unsigned char*)buf;
    usize got = 0;

    while (got < n) {
        isize r = sys4(SYS_pread64, fd, (usize)(p + got), n - got, (usize)(off + got));
        if (r <= 0) return -1;
        got += (usize)r;
    }

    return 0;
}

static i64 vaddr_to_offset(Elf64_Phdr *phdrs, int phnum, u64 vaddr, u64 need) {
    for (int i = 0; i < phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;
        if (vaddr < ph->p_vaddr) continue;
        u64 delta = vaddr - ph->p_vaddr;
        if (delta > ph->p_filesz) continue;
        if (need > ph->p_filesz - delta) continue;
        return (i64)(ph->p_offset + delta);
    }
    return -1;
}

/*
 * Replacement for `patchelf --print-rpath`.
 * Prefer DT_RUNPATH over DT_RPATH and return the raw string with no token
 * expansion, matching the original shell behavior.
 */
static char *read_dyn_rpath(const char *exe) {
    int fd = (int)sys4(SYS_openat, AT_FDCWD, (usize)exe, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return dup_cstr("");

    Elf64_Ehdr eh;
    if (read_exact(fd, &eh, sizeof(eh), 0) < 0) {
        sys1(SYS_close, fd);
        return dup_cstr("");
    }

    if (c_memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB ||
        eh.e_machine != EM_X86_64) {
        sys1(SYS_close, fd);
        return dup_cstr("");
    }

    usize phsz = (usize)eh.e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr *phdrs = (Elf64_Phdr*)xalloc(phsz);
    if (read_exact(fd, phdrs, phsz, eh.e_phoff) < 0) {
        sys1(SYS_close, fd);
        return dup_cstr("");
    }

    Elf64_Off dynoff = 0;
    Elf64_Xword dynsz = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dynoff = phdrs[i].p_offset;
            dynsz = phdrs[i].p_filesz;
            break;
        }
    }
    if (!dynoff || !dynsz) {
        sys1(SYS_close, fd);
        return dup_cstr("");
    }

    usize ndyn = (usize)(dynsz / sizeof(Elf64_Dyn));
    Elf64_Dyn *dyn = (Elf64_Dyn*)xalloc(dynsz);
    if (read_exact(fd, dyn, dynsz, dynoff) < 0) {
        sys1(SYS_close, fd);
        return dup_cstr("");
    }

    u64 strtab_va = 0;
    u64 strsz = 0;
    u64 runpath_off = (u64)-1;
    u64 rpath_off = (u64)-1;

    for (usize i = 0; i < ndyn; i++) {
        if (dyn[i].d_tag == DT_NULL) break;
        if (dyn[i].d_tag == DT_STRTAB) strtab_va = dyn[i].d_un.d_ptr;
        else if (dyn[i].d_tag == DT_STRSZ) strsz = dyn[i].d_un.d_val;
        else if (dyn[i].d_tag == DT_RUNPATH) runpath_off = dyn[i].d_un.d_val;
        else if (dyn[i].d_tag == DT_RPATH) rpath_off = dyn[i].d_un.d_val;
    }

    if (!strtab_va || !strsz) {
        sys1(SYS_close, fd);
        return dup_cstr("");
    }

    u64 chosen = runpath_off != (u64)-1 ? runpath_off : (rpath_off != (u64)-1 ? rpath_off : (u64)-1);
    if (chosen == (u64)-1 || chosen >= strsz) {
        sys1(SYS_close, fd);
        return dup_cstr("");
    }

    i64 stroff = vaddr_to_offset(phdrs, eh.e_phnum, strtab_va, 1);
    if (stroff < 0) {
        sys1(SYS_close, fd);
        return dup_cstr("");
    }

    u64 max = strsz - chosen;
    char *buf = (char*)xalloc(max + 1);
    if (read_exact(fd, buf, max, (u64)stroff + chosen) < 0) {
        sys1(SYS_close, fd);
        return dup_cstr("");
    }
    sys1(SYS_close, fd);

    usize len = 0;
    while (len < max && buf[len]) len++;
    buf[len] = 0;
    return buf;
}

static char *resolve_loader_from_rpath(const char *exe) {
    char *rpath = read_dyn_rpath(exe);
    if (!rpath[0]) return rpath;
    return resolve_via_path_like("ld-linux-x86-64.so.2", rpath);
}

/*
 * Preserve the original opt-in fallback semantics:
 *   ldd "$EXECUTABLE" | grep /lib64/ld-linux-x86-64.so.2 | cut -f 2 | cut -d ' ' -f 3
 */
static char *parse_ldd_output(char *buf) {
    char *line = buf;

    while (*line) {
        char *end = line;
        while (*end && *end != '\n') end++;
        char save = *end;
        *end = 0;

        if (c_contains(line, "/lib64/ld-linux-x86-64.so.2")) {
            char *field = c_strchr(line, '\t');
            const char *src = field ? field + 1 : line;
            int spaces = 0;
            const char *p = src;
            const char *start = src;

            while (*p && spaces < 2) {
                if (*p == ' ') {
                    spaces++;
                    start = p + 1;
                }
                p++;
            }

            if (spaces >= 2) {
                const char *q = start;
                while (*q && *q != ' ') q++;
                if (q > start) {
                    usize n = (usize)(q - start);
                    char *out = (char*)xalloc(n + 1);
                    c_memcpy(out, start, n);
                    out[n] = 0;
                    return out;
                }
            }
        }

        *end = save;
        if (!save) break;
        line = end + 1;
    }

    return dup_cstr("");
}

static char *resolve_via_ldd(char **envp, const char *exe) {
    const char *allow = getenv_raw(envp, "DYNLD_ALLOW_LDD");
    if (!allow || !allow[0]) return dup_cstr("");

    char *ldd = resolve_via_path_like("ldd", getenv_raw(envp, "PATH"));
    if (!ldd[0]) return ldd;

    int pipes[2];
    if (sys1(SYS_pipe, (usize)pipes) < 0) return dup_cstr("");

    isize pid = sys0(SYS_fork);
    if (pid < 0) return dup_cstr("");
    if (pid == 0) {
        sys1(SYS_close, pipes[0]);
        sys2(SYS_dup2, pipes[1], 1);
        sys1(SYS_close, pipes[1]);

        char *argv[3];
        argv[0] = ldd;
        argv[1] = (char*)exe;
        argv[2] = NULL;

        sys3(SYS_execve, (usize)ldd, (usize)argv, (usize)envp);
        sys1(SYS_exit, 127);
    }

    sys1(SYS_close, pipes[1]);

    usize cap = 65536;
    char *buf = (char*)xalloc(cap);
    usize off = 0;
    while (off + 1 < cap) {
        isize n = sys3(SYS_read, pipes[0], (usize)(buf + off), cap - 1 - off);
        if (n <= 0) break;
        off += (usize)n;
    }
    buf[off] = 0;

    sys1(SYS_close, pipes[0]);
    {
        int status = 0;
        sys4(SYS_wait4, (usize)pid, (usize)&status, 0, 0);
    }

    return parse_ldd_output(buf);
}

static char *derive_loader_from_library(const char *lib_path) {
    const char *slash = NULL;
    const char *cursor = lib_path;
    char *base;
    char *candidate;

    while (*cursor) {
        if (*cursor == '/') slash = cursor;
        cursor++;
    }
    if (!slash || slash == lib_path) return dup_cstr("");

    base = (char*)xalloc((usize)(slash - lib_path) + 1);
    c_memcpy(base, lib_path, (usize)(slash - lib_path));
    base[slash - lib_path] = 0;

    candidate = join_search_path_component(base, c_strlen(base), "ld-linux-x86-64.so.2");
    if (path_is_executable_file(candidate)) return candidate;

    {
        const char *last = NULL;
        const char *p = base;
        while (*p) {
            if (*p == '/') last = p;
            p++;
        }
        if (last && last != base) {
            char *parent = (char*)xalloc((usize)(last - base) + 1);
            c_memcpy(parent, base, (usize)(last - base));
            parent[last - base] = 0;

            candidate = join_search_path_component(parent, c_strlen(parent), "lib/ld-linux-x86-64.so.2");
            if (path_is_executable_file(candidate)) return candidate;

            candidate = join_search_path_component(parent, c_strlen(parent), "lib64/ld-linux-x86-64.so.2");
            if (path_is_executable_file(candidate)) return candidate;
        }
    }

    return dup_cstr("");
}

static char *resolve_via_ambient_paths(char **envp, const char *exe) {
    const char *allow = getenv_raw(envp, "DYNLD_ALLOW_LDD");
    if (!allow || !allow[0]) return dup_cstr("");

    const char *ld_library_path = getenv_raw(envp, "LD_LIBRARY_PATH");
    char *rpath = read_dyn_rpath(exe);
    char *loader = search_existing_in_path("ld-linux-x86-64.so.2", ld_library_path, 1);
    if (loader[0]) return loader;

    loader = search_existing_in_path("ld-linux-x86-64.so.2", rpath, 1);
    if (loader[0]) return loader;

    char *libc = search_existing_in_path("libc.so.6", ld_library_path, 0);
    if (libc[0]) {
        loader = derive_loader_from_library(libc);
        if (loader[0]) return loader;
    }

    libc = search_existing_in_path("libc.so.6", rpath, 0);
    if (libc[0]) {
        loader = derive_loader_from_library(libc);
        if (loader[0]) return loader;
    }

    return dup_cstr("");
}

static void build_final_env(char **envp, const char *exe, char ***out_envp) {
    usize count = 0;
    for (char **e = envp; *e; ++e) count++;

    const char *md = getenv_raw(envp, "MD_PRELOAD");
    if (!md) md = "";

    usize ld_len = c_strlen("LD_PRELOAD=") + c_strlen(md) + 1;
    char *ld = (char*)xalloc(ld_len);
    c_memcpy(ld, "LD_PRELOAD=", 11);
    c_memcpy(ld + 11, md, c_strlen(md) + 1);

    usize me_len = c_strlen("DYNLD_EXECUTABLE_NAME=") + c_strlen(exe) + 1;
    char *me = (char*)xalloc(me_len);
    c_memcpy(me, "DYNLD_EXECUTABLE_NAME=", 22);
    c_memcpy(me + 22, exe, c_strlen(exe) + 1);

    char **newenv = (char**)xalloc((count + 3) * sizeof(char*));
    usize j = 0;
    for (char **e = envp; *e; ++e) {
        if (c_startswith(*e, "LD_PRELOAD=")) continue;
        if (c_startswith(*e, "DYNLD_EXECUTABLE_NAME=")) continue;
        newenv[j++] = *e;
    }
    newenv[j++] = ld;
    newenv[j++] = me;
    newenv[j] = NULL;
    *out_envp = newenv;
}

void _start_c(long *sp) {
    int argc = (int)sp[0];
    char **argv = (void*)(sp + 1);
    char **envp = argv + argc + 1;
    int i = 1;

    if (argc > 1 &&
        (c_has_suffix(argv[1], "-dynld_stage1_loader") ||
         c_has_suffix(argv[1], "/ld-linux-x86-64.so.2"))) {
        i++;
    }

    const char *prog = i < argc ? argv[i] : "";
    char *exe = resolve_executable(envp, prog);
    if (!exe[0]) die_program_not_found(prog);
    i++;

    char *loader = resolve_loader_from_rpath(exe);
    if (!loader[0]) loader = resolve_via_ldd(envp, exe);
    if (!loader[0]) loader = resolve_via_ambient_paths(envp, exe);
    if (!loader[0]) die_loader_not_found(exe);

    char *final_argv[260];
    int j = 0;
    final_argv[j++] = loader;
    final_argv[j++] = exe;
    for (; i < argc && j < 259; ++i) final_argv[j++] = argv[i];
    final_argv[j] = NULL;

    char **final_env = NULL;
    build_final_env(envp, exe, &final_env);

    sys3(SYS_execve, (usize)loader, (usize)final_argv, (usize)final_env);
    write2("[dynld] exec failed for loader ");
    write2(loader);
    write2("\n");
    sys1(SYS_exit, 126);
}

void _start() {
    __asm__(
        ".text\n"
        ".global _start\n"
        "_start:\n"
        "xor %rbp,%rbp\n"
        "mov %rsp,%rdi\n"
        "andq $-16,%rsp\n"
        "call _start_c\n"
    );
}
