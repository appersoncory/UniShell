#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "jobs.h"
#include "params.h"
#include "parser.h"
#include "wait.h"

int
wait_on_fg_pgid(pid_t const pgid)
{
    if (pgid < 0) return -1;

    jid_t const jid = jobs_get_jid(pgid);
    if (jid < 0) return -1;

    /* Make sure the foreground group is running */
    if (kill(-pgid, SIGCONT) < 0) {
        if (errno == ESRCH) {
            // Process group doesn't exist; the job is no longer active
            fprintf(stderr, "Job [%jd] no longer exists\n", (intmax_t)jid);
            return -1;
        }
        else if (errno == EPERM) {
            // Permission denied to send the signal
            perror("kill(SIGCONT) failed: Permission denied");
            return -1;
        }
        else {
            // Unexpected error
            perror("kill(SIGCONT) failed");
            return -1;
        }
    }


    if (is_interactive) {
        // Check if pgid is valid and make it the foreground process group
        if (tcsetpgrp(STDIN_FILENO, pgid) < 0) {
            if (errno == EPERM) {
                fprintf(stderr, "Error: Process group [%jd] does not belong to this session\n", (intmax_t)pgid);
                return -1;
            }
            else if (errno == EINVAL) {
                fprintf(stderr, "Error: Invalid process group [%jd]\n", (intmax_t)pgid);
                return -1;
            }
            else {
                perror("tcsetpgrp failed");
                return -1;
            }
        }
    }


  /* XXX From this point on, all exit paths must account for setting bigshell
   * back to the foreground process group--no naked return statements */
    int retval = 0;  // Default return value
    int last_status = 0;  // Track the last valid status

    /* XXX Notice here we loop until ECHILD and we use the status of
     * the last child process that terminated (in the previous iteration).
     * Consider a pipeline,
     *        cmd1 | cmd2 | cmd3
     *
     * We will loop exactly 4 times, once for each child process, and a
     * fourth time to see ECHILD.
     */
    for (;;) {
        /* Wait on ALL processes in the process group 'pgid' */
        int status;
        pid_t res = waitpid(-pgid, &status, 0);  // Wait for any process in the group
        if (res < 0) {
            /* Error occurred (some errors are ok, see below)
             *
             * XXX status may have a garbage value, use last_status from the
             * previous loop iteration */
            if (errno == ECHILD) {
                errno = 0;
                if (jobs_get_status(jid, &status) < 0) {
                    goto err;  // Actual error: No status found
                }

                if (WIFEXITED(status)) {
                    params.status = WEXITSTATUS(status);
                }
                else if (WIFSIGNALED(status)) {
                    params.status = 128 + WTERMSIG(status);
                }

                if (jobs_remove_pgid(pgid) < 0) {
                    // DO NOT treat this as a fatal error; continue execution
                }

                retval = 0;  // Indicate success even if job removal fails
                goto out;
            }

            else if (errno == EINTR) {
                continue;  // Retry on interrupted system call
            }
            goto err;  // Actual error
        }

        assert(res > 0);  // Ensure a valid child process was waited on

        /* Record the status for reporting later when we see ECHILD */
        if (jobs_set_status(jid, status) < 0) goto err;

        /* Handle case where a child process is stopped
         * The entire process group is placed in the background */
        if (WIFSTOPPED(status)) {
            fprintf(stderr, "[%jd] Stopped\n", (intmax_t)jid);
            goto out;
        }

        /* Track the last valid status */
        last_status = status;
    }

out:
    if (0) {
    err:
        retval = -1;
    }

    if (is_interactive) {
        /* Make BigShell the foreground process group again
         *
         * XXX review tcsetpgrp(3)
         *
         * Note: this will cause BigShell to receive a SIGTTOU signal.
         *       You need to also finish signal.c to have full functionality here.
         *       Otherwise, BigShell will get stopped.
         */
        if (tcsetpgrp(STDIN_FILENO, getpgid(0)) < 0) {
            perror("Failed to restore BigShell as foreground process group");
        }
    }

    return retval;
}

/* XXX DO NOT MODIFY XXX */
int
wait_on_fg_job(jid_t jid)
{
  pid_t pgid = jobs_get_pgid(jid);
  if (pgid < 0) return -1;
  return wait_on_fg_pgid(pgid);
}

int
wait_on_bg_jobs()
{
  size_t job_count = jobs_get_joblist_size();
  struct job const *jobs = jobs_get_joblist();
  for (size_t i = 0; i < job_count; ++i) {
    pid_t pgid = jobs[i].pgid;
    jid_t jid = jobs[i].jid;
    for (;;) {
      /* TODO: Modify the following line to wait for process group
       * XXX make sure to do a nonblocking wait!
       */
      int status;
      pid_t pid = waitpid(0, &status, 0);
      if (pid == 0) {
        /* Unwaited children that haven't exited */
        break;
      } else if (pid < 0) {
        /* Error -- some errors are ok though! */
        if (errno == ECHILD) {
          /* No children -- print saved exit status */
          errno = 0;
          if (jobs_get_status(jid, &status) < 0) return -1;
          if (WIFEXITED(status)) {
            fprintf(stderr, "[%jd] Done\n", (intmax_t)jid);
          } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "[%jd] Terminated\n", (intmax_t)jid);
          }
          jobs_remove_pgid(pgid);
          job_count = jobs_get_joblist_size();
          jobs = jobs_get_joblist();
          break;
        }
        return -1; /* Other errors are not ok */
      }

      /* Record status for reporting later when we see ECHILD */
      if (jobs_set_status(jid, status) < 0) return -1;

      /* Handle case where a process in the group is stopped */
      if (WIFSTOPPED(status)) {
        fprintf(stderr, "[%jd] Stopped\n", (intmax_t)jid);
        break;
      }
    }
  }
  return 0;
}
