#define _GNU_SOURCE
#include <sched.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <grp.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>

#define CG_ROOT "/sys/fs/cgroup"
#define CG_BASE CG_ROOT "/minibox"
#define STACK_SIZE (1024 * 1024)

typedef struct {
    char name[64], hostname[64], rootfs[512], io[128];
    unsigned long long mem;   /* bytes, 0 = unlimited */
    double cpus;              /* 0 = unlimited */
    int pids, weight, userns, monitor;
    unsigned int uid_base, uid_count;   /* --userns mapping: container ids 0..count-1 = host ids base.. */
    char **cmd;
    int pipefd[2];
} cfg_t;

static volatile pid_t g_child = 0;
static void on_sigint(int s) { (void)s; if (g_child > 0) kill(g_child, SIGKILL); }

/* ---------- small helpers ---------- */
static int try_write(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return n < 0 ? -1 : 0;
}
static int writef(const char *path, const char *fmt, ...) {
    char buf[256]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (try_write(path, buf) < 0) {
        fprintf(stderr, "[minibox] cannot write '%s' to %s: %s\n", buf, path, strerror(errno));
        return -1;
    }
    return 0;
}
/* read a number from a cgroup file; key==NULL -> first number, else the line "key N" */
static long long read_ll(const char *path, const char *key) {
    char buf[2048];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    char *p = buf;
    if (key) {
        size_t kl = strlen(key); p = NULL;
        for (char *l = buf; l && *l; ) {
            if (!strncmp(l, key, kl) && l[kl] == ' ') { p = l + kl + 1; break; }
            l = strchr(l, '\n'); if (l) l++;
        }
        if (!p) return -1;
    }
    return strtoll(p, NULL, 10);
}
static unsigned long long parse_size(const char *s) {
    char *end; unsigned long long v = strtoull(s, &end, 10);
    switch (*end) {
        case 'k': case 'K': return v << 10;
        case 'm': case 'M': return v << 20;
        case 'g': case 'G': return v << 30;
        default: return v;
    }
}
/* strict unsigned parse: digits only (no sign, no spaces, not empty), must fit below 2^32-1 */
static int parse_u32(const char *s, unsigned int *out) {
    char *end; unsigned long long v;
    if (!s || s[0] < '0' || s[0] > '9') return -1;
    errno = 0; v = strtoull(s, &end, 10);
    if (errno || *end || v > 0xFFFFFFFEULL) return -1;
    *out = (unsigned int)v;
    return 0;
}
static long long now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* ---------- cgroup setup (runs in the parent, on the host) ---------- */
static void cg_setup(cfg_t *c, const char *cg) {
    const char *ctl[] = { "+cpu", "+memory", "+pids", "+io" };
    mkdir(CG_BASE, 0755);
    for (int i = 0; i < 4; i++) {            /* let children use these controllers */
        try_write(CG_ROOT "/cgroup.subtree_control", ctl[i]);
        try_write(CG_BASE "/cgroup.subtree_control", ctl[i]);
    }
    if (mkdir(cg, 0755) < 0 && errno != EEXIST) { perror("mkdir cgroup"); exit(1); }
    char p[600];
    if (c->mem) {
        snprintf(p, sizeof p, "%s/memory.max", cg);  writef(p, "%llu", c->mem);
        snprintf(p, sizeof p, "%s/memory.swap.max", cg); try_write(p, "0");
        snprintf(p, sizeof p, "%s/memory.oom.group", cg); try_write(p, "1");
    }
    if (c->cpus > 0) {
        snprintf(p, sizeof p, "%s/cpu.max", cg);
        writef(p, "%d 100000", (int)(c->cpus * 100000));
    }
    if (c->weight) { snprintf(p, sizeof p, "%s/cpu.weight", cg); writef(p, "%d", c->weight); }
    if (c->pids)   { snprintf(p, sizeof p, "%s/pids.max", cg);   writef(p, "%d", c->pids); }
    if (c->io[0])  { snprintf(p, sizeof p, "%s/io.max", cg);     writef(p, "%s", c->io); }
}

/* ---------- the container side (PID 1 inside the new namespaces) ---------- */
static void die(const char *what) { fprintf(stderr, "[container] %s: %s\n", what, strerror(errno)); _exit(1); }

static int child_main(void *arg) {
    cfg_t *c = arg; char ch;
    close(c->pipefd[1]);
    if (read(c->pipefd[0], &ch, 1) != 1) _exit(1);   /* wait until parent finished setup */
    close(c->pipefd[0]);

    if (c->userns) {   /* we start with an unmapped id: become container root (host uid_base) */
        if (setgroups(0, NULL) < 0) die("setgroups");
        if (setresgid(0, 0, 0) < 0) die("setresgid");
        if (setresuid(0, 0, 0) < 0) die("setresuid");
    }

    if (sethostname(c->hostname, strlen(c->hostname)) < 0) die("sethostname");

    /* stop our mounts from propagating back to the host */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) die("make / private");
    /* pivot_root needs the new root to be a mount point */
    if (mount(c->rootfs, c->rootfs, NULL, MS_BIND | MS_REC, NULL) < 0) die("bind rootfs");
    if (chdir(c->rootfs) < 0) die("chdir rootfs");

    if (mount("proc", "proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0) die("mount /proc");
    if (mount("sysfs", "sys", "sysfs", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
        fprintf(stderr, "[container] warning: /sys mount failed: %s\n", strerror(errno));
    if (mount("tmpfs", "tmp", "tmpfs", 0, "size=64m") < 0) die("mount /tmp");
    if (mount("tmpfs", "dev", "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=755,size=1m") < 0) die("mount /dev");

    const char *devs[] = { "null", "zero", "random", "urandom", "tty" };
    for (int i = 0; i < 5; i++) {                    /* borrow safe device files from the host */
        char src[64], dst[64];
        snprintf(src, sizeof src, "/dev/%s", devs[i]);
        snprintf(dst, sizeof dst, "dev/%s", devs[i]);
        int fd = open(dst, O_CREAT | O_WRONLY, 0666);
        if (fd >= 0) close(fd);
        if (mount(src, dst, NULL, MS_BIND, NULL) < 0)
            fprintf(stderr, "[container] warning: bind %s failed\n", src);
    }

    /* swap the root: "." becomes /, old root goes to /.oldroot, then we drop it */
    if (syscall(SYS_pivot_root, ".", ".oldroot") < 0) die("pivot_root");
    if (chdir("/") < 0) die("chdir /");
    if (umount2("/.oldroot", MNT_DETACH) < 0) die("umount old root");

    clearenv();
    setenv("PATH", "/bin:/sbin:/usr/bin", 1);
    setenv("HOME", "/", 1);
    setenv("PS1", "minibox# ", 1);
    execvp(c->cmd[0], c->cmd);
    die("execvp");
    return 127;
}

/* ---------- the host side: setup, monitor, report, cleanup ---------- */
static int cmd_run(cfg_t *c) {
    char cg[300], p[600], logname[128];
    snprintf(cg, sizeof cg, "%s/%s", CG_BASE, c->name);
    snprintf(logname, sizeof logname, "minibox-%s.csv", c->name);

    /* make sure the mount points exist (created here as real root, so it also works with --userns) */
    const char *dirs[] = { "proc", "sys", "dev", "tmp", ".oldroot" };
    for (int i = 0; i < 5; i++) {
        snprintf(p, sizeof p, "%s/%s", c->rootfs, dirs[i]);
        mkdir(p, 0755);
    }

    cg_setup(c, cg);
    if (pipe(c->pipefd) < 0) { perror("pipe"); rmdir(cg); return 1; }

    char *stack = malloc(STACK_SIZE);
    if (!stack) { perror("malloc"); close(c->pipefd[0]); close(c->pipefd[1]); rmdir(cg); return 1; }
    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWNET | SIGCHLD;
    if (c->userns) flags |= CLONE_NEWUSER;

    long long t0 = now_ms();
    pid_t pid = clone(child_main, stack + STACK_SIZE, flags, c);
    if (pid < 0) { perror("clone"); close(c->pipefd[0]); close(c->pipefd[1]); free(stack); rmdir(cg); return 1; }
    g_child = pid;
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    if (c->userns) {
        /* container ids 0..count-1 = host ids base..base+count-1.
           We are real root (main() requires euid 0), so the kernel allows writing gid_map
           without setgroups=deny. The child drops its supplementary groups itself. */
        const char *fail = NULL;
        snprintf(p, sizeof p, "/proc/%d/uid_map", pid);
        if (writef(p, "0 %u %u\n", c->uid_base, c->uid_count) < 0) fail = "uid_map";
        else {
            snprintf(p, sizeof p, "/proc/%d/gid_map", pid);
            if (writef(p, "0 %u %u\n", c->uid_base, c->uid_count) < 0) fail = "gid_map";
        }
        if (fail) {
            fprintf(stderr, "[minibox] --userns: could not write %s (base %u, count %u), aborting\n", fail, c->uid_base, c->uid_count);
            kill(pid, SIGKILL);
            if (waitpid(pid, NULL, 0) < 0) perror("[minibox] waitpid");
            close(c->pipefd[0]); close(c->pipefd[1]);
            int rc = -1, e = 0;   /* the child never joined the cgroup, so it should be empty */
            for (int k = 0; k < 20; k++) { rc = rmdir(cg); e = errno; if (rc == 0) break; usleep(50000); }
            if (rc < 0) fprintf(stderr, "[minibox] warning: could not remove cgroup %s: %s\n", cg, strerror(e));
            free(stack);
            return 1;
        }
    }
    snprintf(p, sizeof p, "%s/cgroup.procs", cg);
    writef(p, "%d", pid);               /* put the child under the limits BEFORE it runs anything */

    if (write(c->pipefd[1], "x", 1) < 0) perror("signal child");
    close(c->pipefd[1]);
    printf("[minibox] container '%s' started: host PID %d, cgroup %s\n", c->name, pid, cg);

    /* monitor loop: poll cgroup files until the container exits */
    FILE *log = fopen(logname, "w");
    if (log) fprintf(log, "t_ms,mem_bytes,pids,cpu_usec\n");
    long long peak = 0, last_print = 0; int status;
    for (;;) {
        if (waitpid(pid, &status, WNOHANG) == pid) break;
        long long t = now_ms() - t0;
        snprintf(p, sizeof p, "%s/memory.current", cg); long long mem = read_ll(p, NULL);
        snprintf(p, sizeof p, "%s/pids.current", cg);   long long np  = read_ll(p, NULL);
        snprintf(p, sizeof p, "%s/cpu.stat", cg);       long long cpu = read_ll(p, "usage_usec");
        if (mem > peak) peak = mem;
        if (log) { fprintf(log, "%lld,%lld,%lld,%lld\n", t, mem, np, cpu); fflush(log); }
        if (c->monitor && t - last_print >= 1000) {
            fprintf(stderr, "[mon] t=%5.1fs mem=%7.2f MB pids=%3lld cpu=%.2fs\n",
                    t / 1000.0, mem / 1048576.0, np, cpu / 1e6);
            last_print = t;
        }
        usleep(100000);
    }
    long long wall = now_ms() - t0;
    if (log) fclose(log);

    /* post-run report, read BEFORE removing the cgroup */
    snprintf(p, sizeof p, "%s/memory.peak", cg);     long long kpeak = read_ll(p, NULL);
    snprintf(p, sizeof p, "%s/cpu.stat", cg);        long long cpu   = read_ll(p, "usage_usec");
    snprintf(p, sizeof p, "%s/memory.events", cg);   long long oom   = read_ll(p, "oom_kill");
    snprintf(p, sizeof p, "%s/pids.events", cg);     long long pmax  = read_ll(p, "max");
    if (kpeak > peak) peak = kpeak;

    char why[128];
    if (WIFEXITED(status)) snprintf(why, sizeof why, "exited normally, code %d", WEXITSTATUS(status));
    else if (WTERMSIG(status) == SIGKILL && oom > 0) snprintf(why, sizeof why, "KILLED BY OOM (memory limit hit)");
    else snprintf(why, sizeof why, "killed by signal %d (%s)", WTERMSIG(status), strsignal(WTERMSIG(status)));

    printf("\n========== minibox report: %s ==========\n", c->name);
    printf("exit reason     : %s\n", why);
    printf("wall time       : %.2f s\n", wall / 1000.0);
    printf("cpu time used   : %.2f s\n", cpu / 1e6);
    printf("peak memory     : %.2f MB\n", peak / 1048576.0);
    printf("OOM kills       : %lld\n", oom);
    printf("fork denied (pids.max hit) : %lld times\n", pmax);
    printf("log written to  : %s\n", logname);

    for (int i = 0; i < 20 && rmdir(cg) < 0; i++) usleep(50000);   /* cleanup */
    free(stack);
    return 0;
}

static void usage(void) {
    fprintf(stderr,
      "usage: sudo ./minibox run [options] <cmd> [args]\n"
      "  --mem 64M   --cpus 0.5   --pids 20   --weight 100\n"
      "  --hostname h   --name n   --rootfs dir   --io \"MAJ:MIN wbps=1048576\"\n"
      "  --userns   --uid-base 100000   --uid-count 65536   --monitor\n");
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[1], "run")) usage();
    if (geteuid() != 0) { fprintf(stderr, "run with sudo (needs root for namespaces/cgroups)\n"); return 1; }
    cfg_t c; memset(&c, 0, sizeof c);
    strcpy(c.name, "box"); strcpy(c.hostname, "minibox"); strcpy(c.rootfs, "rootfs");
    c.uid_base = 100000; c.uid_count = 65536;   /* defaults, override with --uid-base / --uid-count */
    int i = 2;
    for (; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (a[0] != '-') break;
        if (!strcmp(a, "--userns")) c.userns = 1;
        else if (!strcmp(a, "--monitor")) c.monitor = 1;
        else if (i + 1 >= argc) usage();
        else if (!strcmp(a, "--mem")) c.mem = parse_size(argv[++i]);
        else if (!strcmp(a, "--cpus")) c.cpus = atof(argv[++i]);
        else if (!strcmp(a, "--pids")) c.pids = atoi(argv[++i]);
        else if (!strcmp(a, "--weight")) c.weight = atoi(argv[++i]);
        else if (!strcmp(a, "--hostname")) snprintf(c.hostname, sizeof c.hostname, "%s", argv[++i]);
        else if (!strcmp(a, "--name")) snprintf(c.name, sizeof c.name, "%s", argv[++i]);
        else if (!strcmp(a, "--io")) snprintf(c.io, sizeof c.io, "%s", argv[++i]);
        else if (!strcmp(a, "--rootfs")) snprintf(c.rootfs, sizeof c.rootfs, "%s", argv[++i]);
        else if (!strcmp(a, "--uid-base")) {
            if (parse_u32(argv[++i], &c.uid_base) < 0) { fprintf(stderr, "[minibox] bad --uid-base value: '%s'\n", argv[i]); return 1; }
        }
        else if (!strcmp(a, "--uid-count")) {
            if (parse_u32(argv[++i], &c.uid_count) < 0) { fprintf(stderr, "[minibox] bad --uid-count value: '%s'\n", argv[i]); return 1; }
        }
        else usage();
    }
    if (c.uid_base == 0 || c.uid_count == 0 || (unsigned long long)c.uid_base + c.uid_count > 0xFFFFFFFFULL) {
        fprintf(stderr, "[minibox] invalid --uid-base/--uid-count: base must be >= 1 (0 would map container root to host root), count >= 1, base+count must fit in 32 bits\n");
        return 1;
    }
    if (i >= argc) usage();
    c.cmd = &argv[i];
    char real[PATH_MAX];
    if (!realpath(c.rootfs, real)) { perror("rootfs"); return 1; }
    snprintf(c.rootfs, sizeof c.rootfs, "%s", real);
    return cmd_run(&c);
}
