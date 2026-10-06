#define _POSIX_C_SOURCE 200809L

#include "background.h"
#include "jobs.h"

#include <signal.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* =========================================================
   SIGCHLD HANDLER

   Detects background processes that have finished and
   updates the job table.
   ========================================================= */

static void sigchld_handler(int signo)
{
    int status;
    pid_t pid;

    (void)signo;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        /*
         * For a normal single background command,
         * the PID is also the process-group ID.
         */
        job_done(pid);

        printf("[background] process %d finished\n", pid);
        fflush(stdout);
    }
}

/* =========================================================
   SET UP SIGCHLD HANDLER
   ========================================================= */

void setup_background_handler(void)
{
    struct sigaction sa;

    sa.sa_handler = sigchld_handler;

    sigemptyset(&sa.sa_mask);

    /*
     * Do NOT use SA_NOCLDSTOP here.
     *
     * We want the shell to receive SIGCHLD when a child
     * is stopped by Ctrl+Z.
     */
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGCHLD, &sa, NULL) == -1)
    {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}
