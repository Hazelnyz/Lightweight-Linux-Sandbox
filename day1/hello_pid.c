#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("My PID: %d\n", getpid());
    printf("My parent's PID: %d\n", getppid());
    return 0;
}

