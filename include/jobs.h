#ifndef JOBS_H
#define JOBS_H

#include <sys/types.h>

#define MAX_JOBS 32
#define MAX_JOB_COMMAND 256
#define MAX_JOB_PROCESSES 32

typedef enum
{
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} job_state_t;

typedef struct
{
    int job_id;

    pid_t pgid;

    job_state_t state;

    char command[MAX_JOB_COMMAND];

    pid_t pids[MAX_JOB_PROCESSES];
    int process_count;

} job_t;


/* Initialize job system */
void jobs_init(void);


/* Add a job */
int job_add(pid_t pgid,
            const char *command,
            job_state_t state);


/* Add a process to an existing job */
void job_add_process(pid_t pgid, pid_t pid);


/* Remove job */
void job_remove(int job_id);


/* Display jobs */
void jobs_print(void);


/* Find job by job ID */
job_t *job_find(int job_id);


/* Find job by process group */
job_t *job_find_by_pgid(pid_t pgid);


/* Find job containing a process */
job_t *job_find_by_pid(pid_t pid);


/* Mark stopped */
void job_stop(pid_t pgid);


/* Mark running */
void job_continue(pid_t pgid);


/* Mark done */
void job_done(pid_t pgid);

#endif
