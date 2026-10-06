#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#include "parser.h"
#include "executor.h"
#include "builtin.h"
#include "jobs.h"
#include "background.h"
#include "job_control.h"

/* =========================================================
   BLOCK SIGCHLD
   Prevents races while creating/registering jobs.
   ========================================================= */

static void block_sigchld(sigset_t *oldmask)
{
    sigset_t mask;

    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);

    if (sigprocmask(SIG_BLOCK, &mask, oldmask) < 0)
        perror("sigprocmask");
}

/* =========================================================
   RESTORE SIGNAL MASK
   ========================================================= */

static void restore_signal_mask(const sigset_t *oldmask)
{
    if (sigprocmask(SIG_SETMASK, oldmask, NULL) < 0)
        perror("sigprocmask");
}

/* =========================================================
   APPLY REDIRECTION
   ========================================================= */

static int apply_redirection(command_t *cmd)
{
    int fd;

    if (cmd->input[0] != '\0')
    {
        fd = open(cmd->input, O_RDONLY);

        if (fd < 0)
        {
            perror(cmd->input);
            return -1;
        }

        if (dup2(fd, STDIN_FILENO) < 0)
        {
            perror("dup2");
            close(fd);
            return -1;
        }

        close(fd);
    }

    if (cmd->output[0] != '\0')
    {
        int flags = O_WRONLY | O_CREAT;

        if (cmd->append)
            flags |= O_APPEND;
        else
            flags |= O_TRUNC;

        fd = open(cmd->output, flags, 0644);

        if (fd < 0)
        {
            perror(cmd->output);
            return -1;
        }

        if (dup2(fd, STDOUT_FILENO) < 0)
        {
            perror("dup2");
            close(fd);
            return -1;
        }

        close(fd);
    }

    return 0;
}

/* =========================================================
   PREPARE ARGUMENTS
   ========================================================= */

static void prepare_arguments(command_t *cmd)
{
    if (cmd->argc < MAX_ARGS)
        cmd->argv[cmd->argc] = NULL;
    else
        cmd->argv[MAX_ARGS - 1] = NULL;
}

/* =========================================================
   EXECUTE BUILTIN WITH REDIRECTION
   ========================================================= */

static int execute_builtin_with_redirection(command_t *cmd)
{
    int saved_stdin = -1;
    int saved_stdout = -1;
    int result;

    if (cmd->input[0] != '\0')
    {
        saved_stdin = dup(STDIN_FILENO);

        if (saved_stdin < 0)
        {
            perror("dup");
            return -1;
        }
    }

    if (cmd->output[0] != '\0')
    {
        saved_stdout = dup(STDOUT_FILENO);

        if (saved_stdout < 0)
        {
            perror("dup");
            if (saved_stdin >= 0)
                close(saved_stdin);
            return -1;
        }
    }

    if (apply_redirection(cmd) < 0)
    {
        if (saved_stdin >= 0)
            close(saved_stdin);

        if (saved_stdout >= 0)
            close(saved_stdout);

        return -1;
    }

    result = execute_builtin(cmd);

    if (saved_stdin >= 0)
    {
        if (dup2(saved_stdin, STDIN_FILENO) < 0)
            perror("dup2");

        close(saved_stdin);
    }

    if (saved_stdout >= 0)
    {
        if (dup2(saved_stdout, STDOUT_FILENO) < 0)
            perror("dup2");

        close(saved_stdout);
    }

    return result;
}

/* =========================================================
   EXECUTE SINGLE COMMAND
   ========================================================= */

int execute_command(command_t *cmd)
{
    pid_t pid;
    int status;
    sigset_t oldmask;

    if (cmd == NULL || cmd->argc == 0)
        return -1;

    prepare_arguments(cmd);

    /*
     * Builtins must execute in the shell process when they
     * are foreground commands. This is necessary for things
     * such as "cd".
     */
    if (is_builtin(cmd) && !cmd->background)
        return execute_builtin_with_redirection(cmd);

    /*
     * Block SIGCHLD while creating/registering the child.
     */
    block_sigchld(&oldmask);

    pid = fork();

    if (pid < 0)
    {
        perror("fork");
        restore_signal_mask(&oldmask);
        return -1;
    }

    if (pid == 0)
    {
        /*
         * Child gets its own process group.
         */
        if (setpgid(0, 0) < 0)
        {
            /* Ignore harmless race/error here. */
        }

        /*
         * Restore normal terminal-related signals in child.
         */
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);

        restore_signal_mask(&oldmask);

        if (apply_redirection(cmd) < 0)
            _exit(1);

        if (is_builtin(cmd))
        {
            execute_builtin(cmd);
            _exit(0);
        }

        execvp(cmd->argv[0], cmd->argv);

        fprintf(stderr, "shellforge: %s: %s\n",
                cmd->argv[0], strerror(errno));

        _exit(127);
    }

    /*
     * Parent also places the child in its own process group.
     */
    if (setpgid(pid, pid) < 0)
    {
        if (errno != EACCES && errno != ESRCH)
            perror("setpgid");
    }

    if (cmd->background)
    {
        int job_id;

        job_id = job_add(pid, cmd->argv[0], JOB_RUNNING);

        if (job_id < 0)
        {
            fprintf(stderr, "shellforge: failed to add job\n");
            kill(-pid, SIGTERM);
            restore_signal_mask(&oldmask);
            return -1;
        }

        printf("[%d] %d\n", job_id, pid);
        fflush(stdout);

        restore_signal_mask(&oldmask);
        return 0;
    }

    /*
     * Foreground process gets terminal control.
     */
    give_terminal_to(pid);

    /*
     * Wait for the foreground process.
     *
     * WUNTRACED lets us detect Ctrl-Z.
     */
    if (waitpid(pid, &status, WUNTRACED) < 0)
    {
        if (errno != EINTR)
            perror("waitpid");
    }

    /*
     * Return terminal control to shell.
     */
    take_terminal_back();

    if (WIFSTOPPED(status))
    {
        job_add(pid, cmd->argv[0], JOB_STOPPED);
        printf("\n[%d]+  Stopped    %s\n",
               job_find_by_pgid(pid) ?
               job_find_by_pgid(pid)->job_id : 0,
               cmd->argv[0]);
        fflush(stdout);
    }
    else if (WIFEXITED(status) || WIFSIGNALED(status))
    {
        job_t *job = job_find_by_pgid(pid);

        if (job != NULL)
            job_remove(job->job_id);
    }

    restore_signal_mask(&oldmask);

    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return 0;
}

/* =========================================================
   EXECUTE PIPELINE
   ========================================================= */

int execute_pipeline(pipeline_t *pipeline)
{
    int n;
    int i;
    int pipes[MAX_COMMANDS - 1][2];
    pid_t pids[MAX_COMMANDS];
    pid_t pgid = 0;
    int status = 0;
    int last_status = 0;
    int background;
    sigset_t oldmask;

    if (pipeline == NULL)
        return -1;

    n = pipeline->command_count;

    if (n <= 0)
        return 0;

    /*
     * A single command can use the normal command path.
     */
    if (n == 1)
        return execute_command(&pipeline->commands[0]);

    background = pipeline->commands[n - 1].background;

    /*
     * Create all required pipes.
     */
    for (i = 0; i < n - 1; i++)
    {
        if (pipe(pipes[i]) < 0)
        {
            perror("pipe");

            while (--i >= 0)
            {
                close(pipes[i][0]);
                close(pipes[i][1]);
            }

            return -1;
        }
    }

    /*
     * Block SIGCHLD while creating the complete pipeline.
     */
    block_sigchld(&oldmask);

    for (i = 0; i < n; i++)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            perror("fork");

            for (int j = 0; j < n - 1; j++)
            {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            restore_signal_mask(&oldmask);
            return -1;
        }

        if (pid == 0)
        {
            /*
             * First child becomes process-group leader.
             */
            if (i == 0)
            {
                if (setpgid(0, 0) < 0)
                {
                    /* Ignore harmless errors. */
                }
            }
            else
            {
                if (setpgid(0, pgid) < 0)
                {
                    /* Ignore harmless errors. */
                }
            }

            /*
             * Restore normal signal behavior.
             */
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);

            /*
             * Connect stdin from previous pipe.
             */
            if (i > 0)
            {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) < 0)
                {
                    perror("dup2");
                    _exit(1);
                }
            }

            /*
             * Connect stdout to next pipe.
             */
            if (i < n - 1)
            {
                if (dup2(pipes[i][1], STDOUT_FILENO) < 0)
                {
                    perror("dup2");
                    _exit(1);
                }
            }

            /*
             * Close all pipe descriptors after dup2.
             */
            for (int j = 0; j < n - 1; j++)
            {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            if (apply_redirection(&pipeline->commands[i]) < 0)
                _exit(1);

            prepare_arguments(&pipeline->commands[i]);

            restore_signal_mask(&oldmask);

            if (is_builtin(&pipeline->commands[i]))
            {
                execute_builtin(&pipeline->commands[i]);
                _exit(0);
            }

            execvp(pipeline->commands[i].argv[0],
                   pipeline->commands[i].argv);

            fprintf(stderr,
                    "shellforge: %s: %s\n",
                    pipeline->commands[i].argv[0],
                    strerror(errno));

            _exit(127);
        }

        pids[i] = pid;

        /*
         * Establish the process group from the parent too.
         */
        if (i == 0)
        {
            pgid = pid;
        }

        if (setpgid(pid, pgid) < 0)
        {
            if (errno != EACCES && errno != ESRCH)
                perror("setpgid");
        }
    }

    /*
     * Parent no longer needs pipe descriptors.
     */
    for (i = 0; i < n - 1; i++)
    {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    if (background)
    {
        char command[MAX_JOB_COMMAND];
        size_t used = 0;
        int job_id;

        command[0] = '\0';

        for (i = 0; i < n; i++)
        {
            if (pipeline->commands[i].argc == 0)
                continue;

            if (used > 0 && used + 1 < sizeof(command))
            {
                command[used++] = ' ';
                command[used] = '\0';
            }

            for (int j = 0;
                 j < pipeline->commands[i].argc;
                 j++)
            {
                size_t len =
                    strlen(pipeline->commands[i].argv[j]);

                if (used + len + 1 >= sizeof(command))
                    break;

                memcpy(command + used,
                       pipeline->commands[i].argv[j],
                       len);

                used += len;
                command[used] = '\0';

                if (j + 1 < pipeline->commands[i].argc &&
                    used + 1 < sizeof(command))
                {
                    command[used++] = ' ';
                    command[used] = '\0';
                }
            }
        }

        job_id = job_add(pgid, command, JOB_RUNNING);

        if (job_id < 0)
        {
            fprintf(stderr, "shellforge: failed to add job\n");
            kill(-pgid, SIGTERM);
            restore_signal_mask(&oldmask);
            return -1;
        }

        printf("[%d] %d\n", job_id, pgid);
        fflush(stdout);

        restore_signal_mask(&oldmask);
        return 0;
    }

    /*
     * Give terminal control to the complete pipeline.
     */
    give_terminal_to(pgid);

    /*
     * Wait for every process in the foreground process group.
     *
     * WUNTRACED allows Ctrl-Z to stop the pipeline.
     */
    for (i = 0; i < n; i++)
    {
        pid_t waited;

        waited = waitpid(pids[i], &status, WUNTRACED);

        if (waited < 0)
        {
            if (errno != EINTR)
                perror("waitpid");

            continue;
        }

        if (i == n - 1)
            last_status = status;

        if (WIFSTOPPED(status))
        {
            /*
             * Stop the complete pipeline if one process stops.
             */
            kill(-pgid, SIGTSTP);
            break;
        }
    }

    /*
     * If one process stopped, make sure the remaining processes
     * in the pipeline are stopped too.
     */
    if (WIFSTOPPED(status))
    {
        job_add(pgid,
                pipeline->commands[0].argv[0],
                JOB_STOPPED);

        {
            job_t *job = job_find_by_pgid(pgid);

            printf("\n[%d]+  Stopped    %s\n",
                   job ? job->job_id : 0,
                   pipeline->commands[0].argv[0]);
        }

        fflush(stdout);
    }
    else
    {
        /*
         * Reap remaining foreground processes.
         */
        for (i = 0; i < n; i++)
        {
            if (waitpid(pids[i], &status, 0) < 0)
            {
                if (errno != ECHILD && errno != EINTR)
                    perror("waitpid");
            }
        }
    }

    /*
     * Give terminal back to shell.
     */
    take_terminal_back();

    restore_signal_mask(&oldmask);

    if (WIFEXITED(last_status))
        return WEXITSTATUS(last_status);

    if (WIFSIGNALED(last_status))
        return 128 + WTERMSIG(last_status);

    return 0;
}
