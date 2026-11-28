/* Runs a program and writes its peak RSS (KB) to file descriptor 3, for
 * timeit.py --exact-rss.
 *
 * Why: a child forked from Python starts out with Python's memory mapped, and Linux
 * keeps the child's peak RSS across exec, so every program timed straight from Python
 * shows at least Python's own ~9 MB. Forked from this small program instead, a
 * program's peak starts near 1 MB.
 *   peakrss ./prog [args...]   (fd 3 must be open for writing) */
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: peakrss prog [args...]\n");
        return 2;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 2;
    }
    if (pid == 0) {
        close(3);
        execv(argv[1], argv + 1);
        perror(argv[1]);
        _exit(127);
    }
    int status;
    struct rusage ru;
    if (wait4(pid, &status, 0, &ru) < 0) {
        perror("wait4");
        return 2;
    }
    char line[32];
    int n = snprintf(line, sizeof line, "%ld\n", ru.ru_maxrss);
    if (write(3, line, (size_t)n) != n) return 2;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
