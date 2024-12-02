#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <errno.h>
#include <stddef.h>
#include "signal.h"

static void
interrupting_signal_handler(int signo)
{
  /* Its only job is to interrupt system calls--like read()--when
   * certain signals arrive--like Ctrl-C.
   */
}

static struct sigaction ignore_action = {.sa_handler = SIG_IGN},
                        interrupt_action = {.sa_handler =
                                                interrupting_signal_handler},
                        old_sigtstp, old_sigint, old_sigttou;

/* Ignore certain signals.
 * 
 * @returns 0 on succes, -1 on failure
 *
 *
 * The list of signals to ignore:
 *   - SIGTSTP
 *   - SIGINT
 *   - SIGTTOU
 *
 * Should be called immediately on entry to main() 
 *
 * Saves old signal dispositions for a later call to signal_restore()
 */
int
signal_init(void)
{
    // Ignore SIGTSTP and save the old action
    if (sigaction(SIGTSTP, &ignore_action, &old_sigtstp) < 0) {
        return -1;
    }

    // Ignore SIGTTOU and save the old action
    if (sigaction(SIGTTOU, &ignore_action, &old_sigttou) < 0) {
        return -1;
    }

    // Ignore SIGINT and save the old action
    if (sigaction(SIGINT, &ignore_action, &old_sigint) < 0) {
        return -1;
    }

    return 0;  // Return success
}

/** enable signal to interrupt blocking syscalls (read/getline, etc) 
 *
 * @returns 0 on succes, -1 on failure
 *
 * does not save old signal disposition
 */
int
signal_enable_interrupt(int sig)
{
    // Use the pre-defined `interrupt_action` struct for interrupting system calls
    if (sigaction(sig, &interrupt_action, NULL) < 0) {
        errno = EINVAL;  // Set errno for better error reporting
        return -1;       // Return failure if unable to set signal action
    }

    return 0;  // Return success if signal action is set correctly
}

/** ignore a signal
 *
 * @returns 0 on success, -1 on failure
 *
 * does not save old signal disposition
 */
int
signal_ignore(int sig)
{
    // Use the pre-defined `ignore_action` struct for ignoring the signal
    if (sigaction(sig, &ignore_action, NULL) < 0) {
        errno = EINVAL;  // Set errno for better error reporting
        return -1;       // Return failure if unable to set signal action
    }

    return 0;  // Return success if signal is set to be ignored correctly
}

/** Restores signal dispositions to what they were when bigshell was invoked
 *
 * @returns 0 on success, -1 on failure
 *
 */
int
signal_restore(void)
{
    // Restore SIGTSTP
    if (sigaction(SIGTSTP, &old_sigtstp, NULL) < 0) {
        return -1;
    }

    // Restore SIGTTOU
    if (sigaction(SIGTTOU, &old_sigttou, NULL) < 0) {
        return -1;
    }

    // Restore SIGINT
    if (sigaction(SIGINT, &old_sigint, NULL) < 0) {
        return -1;
    }

    return 0;  // Success
}
