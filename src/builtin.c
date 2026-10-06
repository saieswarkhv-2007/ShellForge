#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

#include "builtin.h"
#include "jobs.h"
#include "job_control.h"
/* =========================================================
   BUILTIN: cd
   ========================================================= */

static int builtin_cd(command_t *cmd)
{
    const char *directory;

    if (cmd->argc == 1)
    {
        directory = getenv("HOME");

        if (directory == NULL)
        {
            fprintf(stderr, "cd: HOME not set\n");
            return -1;
        }
    }
    else if (cmd->argc == 2)
    {
        directory = cmd->argv[1];
    }
    else
    {
        fprintf(stderr, "cd: too many arguments\n");
        return -1;
    }

    if (chdir(directory) != 0)
    {
        perror("cd");
        return -1;
    }

    return 0;
}

/* =========================================================
   BUILTIN: pwd
   ========================================================= */

static int builtin_pwd(command_t *cmd)
{
    char current_directory[4096];

    if (cmd->argc > 1)
    {
        fprintf(stderr, "pwd: too many arguments\n");
        return -1;
    }

    if (getcwd(current_directory, sizeof(current_directory)) == NULL)
    {
        perror("pwd");
        return -1;
    }

    printf("%s\n", current_directory);

    return 0;
}

/* =========================================================
   BUILTIN: echo
   ========================================================= */

static int builtin_echo(command_t *cmd)
{
    for (int i = 1; i < cmd->argc; i++)
    {
        printf("%s", cmd->argv[i]);

        if (i < cmd->argc - 1)
            printf(" ");
    }

    printf("\n");

    return 0;
}

/* =========================================================
   BUILTIN: exit
   ========================================================= */

static int builtin_exit(command_t *cmd)
{
    if (cmd->argc > 1)
    {
        fprintf(stderr, "exit: too many arguments\n");
        return -1;
    }

    return 1;
}

/* =========================================================
   BUILTIN: jobs
   ========================================================= */

int builtin_jobs(command_t *cmd)
{
    if (cmd->argc > 1)
    {
        fprintf(stderr, "jobs: too many arguments\n");
        return -1;
    }

    jobs_print();

    return 0;
}

/* =========================================================
   GET JOB ID FROM ARGUMENT
   Supports:
       fg 1
       fg %1
       bg 1
       bg %1
   ========================================================= */

static int get_job_id(command_t *cmd)
{
    const char *arg;
    char *end;
    long value;

    if (cmd->argc != 2)
    {
        fprintf(stderr,
                "%s: usage: %s <job-id>\n",
                cmd->argv[0],
                cmd->argv[0]);

        return -1;
    }

    arg = cmd->argv[1];

    if (arg[0] == '%')
        arg++;

    if (*arg == '\0')
    {
        fprintf(stderr, "%s: invalid job id\n", cmd->argv[0]);
        return -1;
    }

    value = strtol(arg, &end, 10);

    if (*end != '\0' || value <= 0)
    {
        fprintf(stderr, "%s: invalid job id\n", cmd->argv[0]);
        return -1;
    }

    return (int)value;
}

/* =========================================================
   BUILTIN: bg
   ========================================================= */

int builtin_bg(command_t *cmd)
{
    int job_id;
    job_t *job;

    job_id = get_job_id(cmd);

    if (job_id < 0)
        return -1;

    job = job_find(job_id);

    if (job == NULL)
    {
        fprintf(stderr, "bg: job %d not found\n", job_id);
        return -1;
    }

    if (kill(-job->pgid, SIGCONT) < 0)
    {
        perror("bg: SIGCONT");
        return -1;
    }

    job_continue(job->pgid);

    printf("[%d]+  Running    %s &\n",
           job->job_id,
           job->command);

    fflush(stdout);

    return 0;
}

/* =========================================================
   BUILTIN: fg
   ========================================================= */

int builtin_fg(command_t *cmd)
{
    int job_id;
    job_t *job;
    pid_t pgid;
    int status;
    sigset_t mask;
    sigset_t oldmask;

    job_id = get_job_id(cmd);

    if (job_id < 0)
        return -1;

    job = job_find(job_id);

    if (job == NULL)
    {
        fprintf(stderr, "fg: job %d not found\n", job_id);
        return -1;
    }

    pgid = job->pgid;

    /*
     * Block SIGCHLD so the background handler cannot
     * reap this foreground job while fg is waiting.
     */
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);

    if (sigprocmask(SIG_BLOCK, &mask, &oldmask) < 0)
    {
        perror("fg: sigprocmask");
        return -1;
    }

    /*
     * Give terminal control to the job.
     */
    give_terminal_to(pgid);

    /*
     * If the job was stopped, continue it.
     */
    if (job->state == JOB_STOPPED)
    {
        if (kill(-pgid, SIGCONT) < 0)
        {
            perror("fg: SIGCONT");
            take_terminal_back();
            sigprocmask(SIG_SETMASK, &oldmask, NULL);
            return -1;
        }

        job_continue(pgid);
    }

    /*
     * Wait until a process in this job stops or exits.
     */
    while (1)
    {
        pid_t result;

        result = waitpid(-pgid, &status, WUNTRACED);

        if (result < 0)
        {
            if (errno == EINTR)
                continue;

            if (errno == ECHILD)
                break;

            perror("fg: waitpid");
            break;
        }

        if (WIFSTOPPED(status))
        {
            job_stop(pgid);
            break;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status))
        {
            /*
             * Keep checking until all processes in the
             * process group have finished.
             */
            continue;
        }
    }

    /*
     * Return terminal control to Shellforge.
     */
    take_terminal_back();

    /*
     * Check the job again.
     */
    job = job_find_by_pgid(pgid);

    if (job != NULL)
    {
        if (job->state != JOB_STOPPED)
            job_remove(job->job_id);
    }

    /*
     * Allow SIGCHLD handler to run again.
     */
    sigprocmask(SIG_SETMASK, &oldmask, NULL);

    return 0;
}

/* =========================================================
   CHECK WHETHER COMMAND IS A BUILTIN
   ========================================================= */

int is_builtin(const command_t *cmd)
{
    if (cmd == NULL || cmd->argc == 0)
        return 0;

    if (strcmp(cmd->argv[0], "cd") == 0)
        return 1;

    if (strcmp(cmd->argv[0], "pwd") == 0)
        return 1;

    if (strcmp(cmd->argv[0], "echo") == 0)
        return 1;

    if (strcmp(cmd->argv[0], "exit") == 0)
        return 1;

    if (strcmp(cmd->argv[0], "jobs") == 0)
        return 1;

    if (strcmp(cmd->argv[0], "fg") == 0)
        return 1;

    if (strcmp(cmd->argv[0], "bg") == 0)
        return 1;

    return 0;
}

/* =========================================================
   EXECUTE BUILTIN
   ========================================================= */

int execute_builtin(command_t *cmd)
{
    if (cmd == NULL || cmd->argc == 0)
        return -1;

    if (strcmp(cmd->argv[0], "cd") == 0)
        return builtin_cd(cmd);

    if (strcmp(cmd->argv[0], "pwd") == 0)
        return builtin_pwd(cmd);

    if (strcmp(cmd->argv[0], "echo") == 0)
        return builtin_echo(cmd);

    if (strcmp(cmd->argv[0], "exit") == 0)
        return builtin_exit(cmd);

    if (strcmp(cmd->argv[0], "jobs") == 0)
        return builtin_jobs(cmd);

    if (strcmp(cmd->argv[0], "fg") == 0)
        return builtin_fg(cmd);

    if (strcmp(cmd->argv[0], "bg") == 0)
        return builtin_bg(cmd);

    return -1;
}
