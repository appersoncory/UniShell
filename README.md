# UniShell

UniShell is a POSIX-like shell program developed as part of an academic project for Operating Systems I. It implements fundamental shell features such as command parsing, I/O redirection, process and job control, and signal handling, offering a deeper understanding of system-level programming in Unix-like environments.

## Features

- **Command Execution**: Supports both built-in and external commands.
  - Built-in commands include `cd`, `exit`, and `unset`.
- **I/O Redirection**: Handles operators like `>`, `<`, `>>`, and `<>`.
- **Pipelines**: Execute multiple commands in sequence with `|`.
- **Job Control**: Manage foreground and background processes.
- **Signal Handling**: Proper handling of signals like `SIGINT` and `SIGTSTP`.
- **Variable Expansion**: Implements tilde and parameter expansion.

## Learning Objectives

The project was designed to:
- Explore Unix process and signal APIs.
- Implement job control concepts such as process groups and foreground/background tasks.
- Work with advanced concepts like redirection, piping, and variable expansion.

## Project Structure

UniShell is structured into modular components:
- **Main Loop**: Manages the REPL (Read-Evaluate-Print Loop) for user input.
- **Parser**: Breaks input into tokens and constructs commands.
- **Runner**: Executes parsed commands and manages job control.
- **Wait**: Handles foreground and background process waiting.
- **Expand**: Performs word and parameter expansion.
- **Jobs**: Manages the job table for background processes.

## Example Usage

# Running an external command
ls -l

# Piping commands
cat file.txt | grep "pattern"

# Redirecting I/O
echo "Hello, UniShell!" > output.txt

# Background jobs
sleep 10 &
