#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wait.h>

#include "builtins.h"
#include "exit.h"
#include "expand.h"
#include "jobs.h"
#include "params.h"
#include "parser.h"
#include "signal.h"
#include "util/gprintf.h"
#include "vars.h"
#include "wait.h"

#include "runner.h"

/* Expands all the command words in a command
 *
 * This is:
 *   cmd->words[i]
 *      ; i from 0 to cmd->word_count
 *
 *   cmd->assignments[i]->value
 *      ; i from 0 to cmd->assignment_count
 *
 *   cmd->io_redirs[i]->filename
 *      ; i from 0 to cmd->io_redir_count
 *
 * */
static int
expand_command_words(struct command *cmd)
{
  for (size_t i = 0; i < cmd->word_count; ++i) {
    expand(&cmd->words[i]);
  }

  // Expand assignment values
  for (size_t i = 0; i < cmd->assignment_count; ++i) {
      expand(&cmd->assignments[i]->value);
  }

  // Expand I/O redirection filenames
  for (size_t i = 0; i < cmd->io_redir_count; ++i) {
      expand(&cmd->io_redirs[i]->filename);
  }

  return 0;
}

/** Performs variable assignments before running a command
 *
 * @param cmd        the command to be executed
 * @param export_all controls whether variables are also exported
 *
 * if export_all is zero, variables are assigned but not exported.
 * if export_all is non-zero, variables are assigned and exported.
 */
static int
do_variable_assignment(struct command const *cmd, int export_all)
{
    for (size_t i = 0; i < cmd->assignment_count; ++i) {
        struct assignment* a = cmd->assignments[i];

        // Attempt to set the variable
        if (vars_set(a->name, a->value) != 0) {
            return -1; // Return immediately if assignment fails
        }

        // If export_all is non-zero, mark the variable for export
        if (export_all && vars_export(a->name) != 0) {
            return -1; // Return immediately if export fails
        }
    }
    return 0; // Return 0 on success
}

static int
get_io_flags(enum io_operator io_op)
{
    int flags = 0;

    switch (io_op) {
    case OP_LESSAND: /* <& */
    case OP_LESS:    /* < */
        // Open for reading
        flags = O_RDONLY;
        break;

    case OP_GREATAND: /* >& */
    case OP_GREAT:    /* > */
        // Open for writing; create if doesn't exist; fail if file exists
        flags = O_WRONLY | O_CREAT | O_EXCL;
        break;

    case OP_DGREAT: /* >> */
        // Open for appending; create if doesn't exist
        flags = O_WRONLY | O_CREAT | O_APPEND;
        break;

    case OP_LESSGREAT: /* <> */
        // Open for reading and writing; create if doesn't exist
        flags = O_RDWR | O_CREAT;
        break;

    case OP_CLOBBER: /* >| */
        // Open for writing; create if doesn't exist; truncate if it exists
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        break;
    }

    return flags;
}

static int
move_fd(int src, int dst)
{
    if (src == dst) return dst; // No action needed if src and dst are the same

    // Duplicate src to dst
    if (dup2(src, dst) < 0) {
        // Duplication failed, return -1 to indicate failure
        return -1;
    }

    // Close the original source descriptor
    if (close(src) < 0) {
        // Closing src failed, return -1 to indicate failure
        return -1;
    }

    return dst; // Return the target descriptor on success
}


/** Performs i/o pseudo-redirection for builtin commands
 *
 * @param [in]cmd the command we are performing redirections for.
 * @param [out]redir_list a virtual file descriptor table on top of the shell's
 * own file descriptors.
 *
 * This function performs all of the normal i/o redirection, but doesn't
 * overwrite any existing open files. Instead, it performs virtual redirections,
 * maintainig a list of what /would/ have changed if the redirection was
 * actually performed. The builtins refer to this list to access the correct
 * file descriptors for i/o.
 *
 * This allows the redirections to be undone after executing a builtin, which is
 * necessary to avoid screwing up the shell, since builtins don't run as
 * separate child processes--they are just functions that are a part of the
 * shell itself.
 *
 * This is, of course, very complicated, and not something you're expected to
 * wrap your head around. You can ignore this function entirely.
 *
 * XXX DO NOT MODIFY XXX
 */
static int
do_builtin_io_redirects(struct command *cmd, struct builtin_redir **redir_list)
{
  int status = 0;
  for (size_t i = 0; i < cmd->io_redir_count; ++i) {
    struct io_redir *r = cmd->io_redirs[i];
    if (r->io_op == OP_GREATAND || r->io_op == OP_LESSAND) {
      /* These are the operators [n]>& and [n]<&
       *
       * They are identical except that they have different default
       * values for n when omitted: 0 for <& and 1 for >&. */

      if (strcmp(r->filename, "-") == 0) {
        /* [n]>&- and [n]<&- close file descriptor [n] */
        struct builtin_redir *rec = *redir_list;
        for (; rec; rec = rec->next) {
          if (rec->pseudofd == r->io_number) {
            close(rec->realfd);
            rec->pseudofd = -1;
            break;
          }
        }
        if (rec == 0) {
          rec = malloc(sizeof *rec);
          if (!rec) goto err;
          rec->pseudofd = r->io_number;
          rec->realfd = -1;
          rec->next = *redir_list;
          *redir_list = rec;
        }
      } else {
        /* The filename is interpreted as a file descriptor number to
         * redirect to. For example, 2>&1 duplicates file descriptor 1
         * onto file descriptor 2 (yes, it feels backwards). */
        char *end = r->filename;
        long src = strtol(r->filename, &end, 10);

        if (*(r->filename) && !*end && src <= INT_MAX) {
          for (struct builtin_redir *rec = *redir_list; rec; rec = rec->next) {
            if (rec->realfd == src) {
              errno = EBADF;
              goto err;
            }
            if (rec->pseudofd == src) src = rec->realfd;
          }
          struct builtin_redir *rec = *redir_list;
          for (; rec; rec = rec->next) {
            if (rec->pseudofd == r->io_number) {
              if (dup2(src, rec->realfd) < 0) goto err;
              break;
            }
          }
          if (rec == 0) {
            rec = malloc(sizeof *rec);
            if (!rec) goto err;
            rec->pseudofd = r->io_number;
            rec->realfd = dup(src);
            rec->next = *redir_list;
            *redir_list = rec;
          }
        } else {
          goto file_open;
        }
      }
    } else {
    file_open:;
      int flags = get_io_flags(r->io_op);
      gprintf("attempting to open file %s with flags %d", r->filename, flags);
      /* TODO Open the specified file. */
      int fd = open(r->filename, flags, 0777);
      if (fd < 0) goto err;
      struct builtin_redir *rec = *redir_list;
      for (; rec; rec = rec->next) {
        if (rec->pseudofd == r->io_number) {
          if (move_fd(fd, rec->realfd) < 0) goto err;
          break;
        }
      }
      if (rec == 0) {
        rec = malloc(sizeof *rec);
        if (!rec) goto err;
        rec->pseudofd = r->io_number;
        rec->realfd = fd;
        rec->next = *redir_list;
        *redir_list = rec;
      }
    }
    if (0) {
    err:
      status = -1;
    }
  }
  return status;
}

/** perform the main task of io redirection (for non-builtin commands)
 *
 * @param [in]cmd the command we are performing redirections for.
 * @returns 0 on success, -1 on failure
 *
 * Unlike the builtin redirections, this is straightforward, because it
 * will only ever happen in forked child processes--and can't affect the shell
 * itself. Iterate over the list of redirections and apply each one in sequence.
 *
 * TODO
 */
static int
do_io_redirects(struct command* cmd)
{
    int status = 0;

    for (size_t i = 0; i < cmd->io_redir_count; ++i) {
        struct io_redir* r = cmd->io_redirs[i];
        if (r->io_op == OP_GREATAND || r->io_op == OP_LESSAND) {
            /* These are the operators [n]>& and [n]<&
             *
             * They are identical except that they have different default
             * values for n when omitted: 0 for <& and 1 for >&. */

            if (strcmp(r->filename, "-") == 0) {
                /* [n]>&- and [n]<&- close file descriptor [n] */
                if (close(r->io_number) < 0) {
                    goto err; /* Handle failure gracefully */
                }
            }
            else {
                /* The filename is interpreted as a file descriptor number to
                 * redirect to. For example, 2>&1 duplicates file descriptor 1
                 * onto file descriptor 2, so that file descriptor 2 now points
                 * to the same file that 1 does. */

                 /* XXX This is a very idiomatic method of converting
                  *     strings to numbers. Avoid atoi() and scanf(), due to
                  *     lack of error checking. Read the man page for strtol()!
                  *
                  *     You'll probably want to use this exact code again elsewhere in
                  *     this project...
                  */
                char* end = r->filename;
                long src = strtol(r->filename, &end, 10);

                if (*(r->filename) && !*end /* <--- this is part of the strtol idiom */
                    && src <= INT_MAX /* <--- this is *critical* bounds checking when
                                         downcasting */
                    ) {
                    /* Duplicate src to dst. */
                    if (dup2((int)src, r->io_number) < 0) {
                        goto err; /* Handle failure gracefully */
                    }
                }
                else {
                    /* XXX Syntax error--(not a valid number)--we can "recover" by
                     * attempting to open a file instead. That's what bash does.
                     *
                     * e.g. `>& file` is treated as `> file` instead of printing an error
                     */
                    goto file_open; /* XXX target is just a few lines below this */
                }
            }
        }
        else {
        file_open:;
            int flags = get_io_flags(r->io_op);
            gprintf("attempting to open file %s with flags %d", r->filename, flags);

            /* Open the specified file with the appropriate flags and mode
             *
             * XXX Note: you can supply a mode to open() even if you're not creating a
             * file. it will just ignore that argument.
             */
            int fd = open(r->filename, flags, 0777);

            if (fd < 0) {
                goto err; /* Handle failure gracefully */
            }

            /* Move the opened file descriptor to the redirection target */
            /* XXX use move_fd() */
            if (move_fd(fd, r->io_number) < 0) {
                goto err; /* Handle failure gracefully */
            }
        }
    }

    return status;

err: /* Anything that can fail should jump here. No silent errors!!! */
    status = -1;
    return status;
}


int
run_command_list(struct command_list *cl)
{
  /* These are declared outside the main loop below, so that their values
   * persist between successive commands in a pipeline. */
  struct {
    int pipe_fd; /* -1 means no upstream pipe */
    pid_t pgid;
    jid_t jid;
  } pipeline_data = {.pipe_fd = -1, .pgid = 0, .jid = -1};

  /* Loop over every command in the command list */
  for (size_t i = 0; i < cl->command_count; ++i) {
    struct command *cmd = cl->commands[i];
    /* First, handle expansions (tilde, parameter, quote removal) */
    expand_command_words(cmd);

    // clang-format off
    // Next, figure out what kind of command are we running?
    // 3 control types:
    // ';' -- foreground command, parent waits sychronously for child process
    // '&' -- background command, parent waits asynchronously for child process
    // '|' -- pipeline command, behaves as a background command, and writes stdout to a pipe
    //
    // From the perspective of child processes, foreground/background is the same; it is
    // solely a question of whether the parent waits or not after spawning child!
    //
    // Two command types:
    // External -- these are actual standalone programs that are executed with exec()
    // Builtins -- these are routines that are implemented as part of the shell, itself.
    //               take a look at builtins.c!
    //
    // Importantly, builtin commands do not fork() when they are run as
    // foreground commands. This is because they must run in the shell's own
    // execution environment (not as children) in order to modify it. For
    // example to change the shell's working directory, exit the shell, and so
    // on.
    //
    // clang-format on

    int const is_pl = cmd->ctrl_op == '|'; /* pipeline */
    int const is_bg = cmd->ctrl_op == '&'; /* background */
    int const is_fg = cmd->ctrl_op == ';'; /* foreground */
    assert(is_pl || is_bg || is_fg);       /* catch any parser errors */

    /* Prepare to read from pipeline of previous command, if exists. */
    int const upstream_pipefd = pipeline_data.pipe_fd; /* Retrieve READ side from previous pipeline */
    int const has_upstream_pipe = (upstream_pipefd >= 0);

    /* Reset pipeline_data.pipe_fd for the next iteration if this command is not a pipeline */
    if (!is_pl) {
        pipeline_data.pipe_fd = -1; /* No pipe for non-pipeline commands */
    }

    /* If the current command is a pipeline command, create a new pipe on
     * pipe_fds[].
     *
     * The write end up the pipe will be hooked up to stdout of this command
     *
     * The read end of the pipe will be stored in pipeline_data.pipe_fd for the
     * next command to use.
     *
     * See PIPE(2) for the function to call.
     *
     * Note that the initialized values of -1 should be kept if no pipe is
     * created. They indicate the lack of a pipe.
     *
     * [TODO] Create new pipe if needed
     *
     * [TODO] Handle errors that occur
     */
    int pipe_fds[2] = { -1, -1 }; /* Default: No pipe created */

    if (is_pl) {
        /* Create a new pipe */
        if (pipe(pipe_fds) < 0) {
            /* Handle pipe creation failure */
            perror("pipe");
            return -1; /* Terminate early if we can't create the pipe */
        }

        /* Save the READ end of the pipe for the next command */
        pipeline_data.pipe_fd = pipe_fds[0];

        /* The write end will be connected to the current command's stdout */
    }
    else {
        /* No pipeline; reset the pipeline_data pipe_fd */
        pipeline_data.pipe_fd = -1;
    }


    /* Grab the WRITE side of the pipeline we just created */
    int const downstream_pipefd = pipe_fds[STDOUT_FILENO];
    int const has_downstream_pipe = (downstream_pipefd >= 0);

    /* Store the READ side of the pipeline we just created. The next command
     * will need to use this */
    pipeline_data.pipe_fd = pipe_fds[STDIN_FILENO];

    /* Check if we have a builtin -- returns a function pointer of the builtin
     * function if we do, null if we don't */
    builtin_fn const builtin = get_builtin(cmd);
    int const is_builtin = !!builtin;

    pid_t child_pid = 0;

    /* Fork process if:
     * - Not a builtin command, OR
     * - Not a foreground command
     */
    int const should_fork = !is_builtin || !is_fg;
    int did_fork = 0;

    if (should_fork) {
        child_pid = fork();

        if (child_pid < 0) {
            /* Handle fork failure */
            perror("fork");
            goto err; /* Exit the loop or function with an error */
        }

        /* Set did_fork flag to indicate successful fork */
        did_fork = 1;
    }

    if (did_fork) {
        /* Assign process to a process group */
        if (setpgid(child_pid, pipeline_data.pgid) < 0) {
            if (errno == EACCES) errno = 0; /* Ignore race condition errors */
            else goto err; /* Handle other errors */
        }
      /* All of the processes in a pipeline (or single command) belong to the
       * same process group. This is how the shell manages job control. We will
       * create that here, or add the current child to an existing process group
       *
       * Initially pipeline_data.pgid is set to 0 (unset). We will asign the
       * first command in a pipline to a new process group, then store that pgid
       * for later use.
       *
       * Thoroughly read the man page for setpgid(3) and getpgid(3)!
       *
       * Note: There is a race condition in setpgid(), so that we need to call
       * it in both the parent and the child, and ignore an EACCES error if it
       * occurs.
       */

      if (setpgid(child_pid, pipeline_data.pgid) < 0) {
        if (errno == EACCES) errno = 0;
        else goto err;
      }
      if (child_pid && pipeline_data.pgid == 0) {
        /* Start of a new pipeline */
        assert(child_pid == getpgid(child_pid));
        pipeline_data.pgid = child_pid;
        pipeline_data.jid = jobs_add(child_pid);
        if (pipeline_data.jid < 0) goto err;
      }
    }

    /* Now that that's taken care of, let's actually execute the command */
    if (child_pid == 0) {
      if (is_builtin) {
        /* If we are a builtin */
        /* Set up the redir_list for virtual redirection */
        struct builtin_redir *redir_list = 0;

        if (upstream_pipefd >= 0) {
          struct builtin_redir *rec = malloc(sizeof *rec);
          if (!rec) goto err;
          rec->pseudofd = STDIN_FILENO;
          rec->realfd = upstream_pipefd;
          rec->next = redir_list;
          redir_list = rec;
        }
        if (downstream_pipefd >= 0) {
          struct builtin_redir *rec = malloc(sizeof *rec);
          if (!rec) goto err;
          rec->pseudofd = STDOUT_FILENO;
          rec->realfd = downstream_pipefd;
          rec->next = redir_list;
          redir_list = rec;
        }

        do_builtin_io_redirects(cmd, &redir_list);

        do_variable_assignment(cmd, 0);

        /* XXX Here's where we call the builtin function */
        int result = builtin(cmd, redir_list);

        /* clean up redirect list
         * i.e. Undo all "virtual" redirects */
        while (redir_list) {
          close(redir_list->realfd);
          void *tmp = redir_list;
          redir_list = redir_list->next;
          free(tmp);
        }

        params.status = result ? 127 : 0;
        /* If we forked, exit now */
        if (!is_fg) exit(params.status);

        /* Otherwise, we are running in the current shell and
         * need to clean up before falling through */
        errno = 0;
      }
      else {
          /* External command */

          /* Redirect the two standard streams overrides IF they are not set to -1 */
          if (upstream_pipefd >= 0) {
              if (move_fd(upstream_pipefd, STDIN_FILENO) < 0) {
                  err(1, 0); // Fail if unable to redirect stdin
              }
          }

          if (downstream_pipefd >= 0) {
              if (move_fd(downstream_pipefd, STDOUT_FILENO) < 0) {
                  err(1, 0); // Fail if unable to redirect stdout
              }
          }

          /* Handle the remaining redirect operators from the command */
          if (do_io_redirects(cmd) < 0) {
              err(1, 0); // Fail if I/O redirection fails
          }

          /* Perform variable assignment with export_all set to 1 */
          if (do_variable_assignment(cmd, 1) < 0) {
              err(1, 0); // Fail if variable assignment or export fails
          }

          /* Restore signals to their original values */
          if (signal_restore() < 0) {
              err(1, 0); // Fail if signal restoration fails
          }

          /* Execute the command described by cmd->words */
          execvp(cmd->words[0], cmd->words);

          /* If execvp fails */
          err(127, 0); // Exit with failure code (127)
          assert(0);   // Should not be reachable
      }

    }
    if (child_pid == 0) continue;

    /* This code is reachable only by a parent shell process after spawning
     * a child process */
    assert(child_pid > 0);

    /* Close unneeded pipe ends that we hooked up above */
    if (downstream_pipefd >= 0) close(downstream_pipefd);
    if (upstream_pipefd >= 0) close(upstream_pipefd);

    /* Whether the parent waits on the child is dependent on the control
     * operator */
    if (is_fg) {
        int fg_wait_result = wait_on_fg_pgid(pipeline_data.pgid);

        if (fg_wait_result < 0) {
            if (params.status != 127) {
                return 0;  // Use params.status directly if set correctly
            }

            warn(0);
            params.status = 127;
            return -1;
        }
    } else {
      /* Background or Pipeline */
      assert(is_bg || is_pl);
      params.bg_pid = child_pid;

      if (is_bg) {
        /* Pipelines that end with a background (&) command print a little
         * message when they spawn.
         * "[<JOBID>] <GROUPID>\n"
         */
        fprintf(stderr,
                "[%jd] %jd\n",
                (intmax_t)pipeline_data.jid,
                (intmax_t)pipeline_data.pgid);
      }
      params.status = 0;
    }

    /* Cleanup after non-pipeline cmds */
    if (!is_pl) {
      assert(pipeline_data.pipe_fd == -1);
      pipeline_data.pgid = 0;
      pipeline_data.jid = -1;
    }
  }

  return 0;
err:
  return -1;
}
