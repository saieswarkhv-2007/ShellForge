#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <errno.h>

#include "job_control.h"


static pid_t shell_pgid;


/* =========================================================
   JOB CONTROL INITIALIZATION
   ========================================================= */

void job_control_init(void)
{
    pid_t pid;

    /*
     * If stdin is not a terminal,
     * job control is not available.
     */

    if (!isatty(STDIN_FILENO))
    {
        shell_pgid = getpid();

        return;
    }


    pid = getpid();


    /*
     * Ignore interactive terminal signals
     * in the shell itself.
     */

    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);


    /*
     * Put shell into its own process group.
     */

    shell_pgid = pid;

    if (setpgid(
            shell_pgid,
            shell_pgid
        ) < 0)
    {
        if (errno != EACCES &&
            errno != EPERM)
        {
            perror("setpgid");
        }
    }


    /*
     * Make shell the foreground process group.
     */

    if (tcsetpgrp(
            STDIN_FILENO,
            shell_pgid
        ) < 0)
    {
        perror("tcsetpgrp");
    }
}


/* =========================================================
   GIVE TERMINAL TO JOB
   ========================================================= */

void give_terminal_to(pid_t pgid)
{
    if (!isatty(STDIN_FILENO))
    {
        return;
    }

    if (tcsetpgrp(
            STDIN_FILENO,
            pgid
        ) < 0)
    {
        perror("tcsetpgrp");
    }
}


/* =========================================================
   TAKE TERMINAL BACK
   ========================================================= */

void take_terminal_back(void)
{
    if (!isatty(STDIN_FILENO))
    {
        return;
    }

    if (tcsetpgrp(
            STDIN_FILENO,
            shell_pgid
        ) < 0)
    {
        perror("tcsetpgrp");
    }
}


/* =========================================================
   GET SHELL PROCESS GROUP
   ========================================================= */

pid_t get_shell_pgid(void)
{
    return shell_pgid;
}
