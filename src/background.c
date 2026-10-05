#define _POSIX_C_SOURCE 200809L

#include "background.h"
#include <signal.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Handler for SIGCHLD to reap background processes
static void sigchld_handler(int signo)
{
    (void)signo;

    int status;
    pid_t pid;

    // Reap all finished child processes
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        printf("[background] process %d finished\n", pid);
    }
}

// Set up SIGCHLD handler
void setup_background_handler(void)
{
    struct sigaction sa;

    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &sa, NULL) == -1)
    {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}
