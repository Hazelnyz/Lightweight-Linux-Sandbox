#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
    if (argc < 2) { puts("usage: stress cpu | mem <MB> | fork <N>"); return 1; }
    if (!strcmp(argv[1], "cpu")) {
        volatile unsigned long x = 0;
        for (;;) x++;                                   /* burn 100% of a core */
    }
    if (!strcmp(argv[1], "mem")) {
        int mb = argc > 2 ? atoi(argv[2]) : 1024;
        for (int i = 0; i < mb; i++) {
            char *p = malloc(1 << 20);
            if (!p) { puts("malloc failed"); break; }
            memset(p, 1, 1 << 20);                      /* touch it so it's really used */
            printf("allocated %d MB\n", i + 1); fflush(stdout);
            usleep(50000);
        }
        sleep(5);
        return 0;
    }
    if (!strcmp(argv[1], "fork")) {
        int n = argc > 2 ? atoi(argv[2]) : 100, made = 0;
        for (int i = 0; i < n; i++) {
            pid_t p = fork();
            if (p < 0) { perror("fork denied"); break; }
            if (p == 0) { sleep(10); _exit(0); }
            made++;
        }
        printf("created %d children\n", made); fflush(stdout);
        while (wait(NULL) > 0);
        return 0;
    }
    return 1;
}
