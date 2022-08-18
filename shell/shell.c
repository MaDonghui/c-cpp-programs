#include "parser/ast.h"
#include "shell.h"
#include "sys/types.h"
#include <sys/wait.h>
#include "unistd.h"
#include "stdio.h"
#include "signal.h"
#include "stdlib.h"
#include "string.h"
#include "fcntl.h"

void initialize(void) {
    /* This code will be called once at startup */

    signal(SIGINT, signal_handler);

    if (prompt)
        prompt = "vush$ ";
}

void run_command(node_t *node) {
    /* Print parsed input for testing - comment this when running the tests! */
    // print_tree(node);

    switch (node->type) {
        // adding ' to avoid label error
        case NODE_COMMAND:
            exec_command(node);
            break;
        case NODE_PIPE:
            exec_pipe(node);
            break;
        case NODE_REDIRECT:
            exec_redirect(node);
            break;
        case NODE_SUBSHELL:
            exec_subshell(node);
            break;
        case NODE_DETACH:
            exec_detach(node);
            break;
        case NODE_SEQUENCE:
            exec_sequence(node);
            break;
    }

    if (prompt)
        prompt = "vush$ ";
}

// commands / build-ins
int exec_command(node_t *node) {
    char *program = node->command.program;
    char **argv = node->command.argv;

    switch (hash(program)) {
        case CD:
            exec_cd(node);
            break;
        case EXIT:
            exec_exit(node);
            break;
        case SET:
            exec_set(node);
            break;
        case UNSET:
            exec_set(node);
            break;
        case ENV:
            exec_env();
            break;

        default:;
            // common commands
            pid_t pid = fork();

            if (pid == 0) {
                if (execvp(program, argv) == -1) perror("execvp error\n");
            } else {
                wait(NULL);
            }
    }

    return 0;
}

int exec_cd(node_t *node) {
    // Args Err
    if (node->command.argc > 2) {
        perror("Invalid Arguments Format.\n");
        return -1;
    }

    // cd / err
    if (chdir(node->command.argv[1]) != 0) {
        perror("cd failed\n");
        return -1;
    }

    // print
    char new_dir[DIR_LEN];
    getcwd(new_dir, DIR_LEN);

    return 0;
}

void exec_exit(node_t *node) {
    if (node->command.argc < 2) {
        exit(0);
    } else {
        exit(atoi(node->command.argv[1]));
    }
}

void exec_set(node_t *node) {
    printf("%s", node->command.argv[1]);
    return;
}

void exec_unset(node_t *node) {
    printf("%s", node->command.argv[1]);
    return;
}

void exec_env() {
    // place holder
    return;
}


void exec_pipe(node_t *node) {
    // using two fd and switch them as in / out possible?
    // prepare all pipes
    // each index should be two int containing two fds, representing a pipe
    unsigned int num_commands = node->pipe.n_parts;
    unsigned int num_pipes = num_commands - 1;

    int *pipes[num_pipes];
    for (unsigned int i = 0; i < num_pipes; ++i) {
        pipes[i] = (int *) malloc(2 * sizeof(int));
        if (pipe(pipes[i]) < 0) {
            perror("Multiple Pipes creating error\n");
        }
    }

    // fork into child processes, i = command index
    for (unsigned int i = 0; i < num_commands; ++i) {
        // Child section
        if (fork() == 0) {
            pipe_t curr_pipe = get_pipe_t(i, num_commands);
            switch (curr_pipe) {
                case PIPE_START:
                    dup2(pipes[i][PIPE_WR], STDOUT_FILENO);         // write to pipe instead of stdout
                    break;

                case PIPE_END:
                    dup2(pipes[i - 1][PIPE_RD], STDIN_FILENO);      // read from last pipe
                    break;

                case PIPE_MIDDLE:
                    dup2(pipes[i - 1][PIPE_RD], STDIN_FILENO);      // read from last pipe
                    dup2(pipes[i][PIPE_WR], STDOUT_FILENO);         // write to pipe instead of stdout
                    break;

                default:
                    perror("hmm, no one suppose to be here, check pipe_t maybe\n");
            }
            // because dup2 is duplicate? close the original unused ones
            for (unsigned int j = 0; j < num_pipes; ++j) {
                close(pipes[j][PIPE_RD]);
                close(pipes[j][PIPE_WR]);
            }

            run_command(node->pipe.parts[i]);
            exit(0);
        }
    }
    // Parent section
    // close all pipe ends in parents as well
    for (unsigned int i = 0; i < num_pipes; ++i) {
        close(pipes[i][PIPE_RD]);
        close(pipes[i][PIPE_WR]);
        free(pipes[i]);
    }
    // wait children to finish // may cause issue, not sure if child of child will count
    for (unsigned int i = 0; i < num_commands; ++i) {
        wait(NULL);
    }
}

void exec_redirect(node_t *node) {
    // back up the original, undirected IO
    const int STDIN = dup(STDIN_FILENO);
    const int STDOUT = dup(STDOUT_FILENO);
    const int STDERR = dup(STDERR_FILENO);

    int fd_in, fd_out;
    switch (node->redirect.mode) {
        case REDIRECT_DUP:
            dup2(node->redirect.fd2, node->redirect.fd);
            break;
        case REDIRECT_INPUT:
            fd_in = open(node->redirect.target, O_RDONLY);
            dup2(fd_in, STDIN_FILENO);
            close(fd_in);
            break;
        case REDIRECT_OUTPUT:
            // read only, trim to 0 length, create if no exists, USER GROUP read write access
            fd_out = open(node->redirect.target, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
            dup2(fd_out, STDOUT_FILENO);
            close(fd_out);
            break;
        case REDIRECT_APPEND:
            // write only, append mode, no trim, create if not exists, USER GROUP read write access
            fd_out = open(node->redirect.target, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
            dup2(fd_out, STDOUT_FILENO);
            close(fd_out);
            break;
    }

    run_command(node->redirect.child);

    // recover
    //BUG: 2>&1 >/dev/null cat _tmpfile1 /_idontexist: FIXED oh ffs missing a comma
    switch (node->redirect.mode) {
        case REDIRECT_DUP:
            dup2(STDIN, STDIN_FILENO);
            dup2(STDERR, STDERR_FILENO);
            dup2(STDOUT, STDOUT_FILENO);
            break;
        case REDIRECT_INPUT:
            dup2(STDIN, STDIN_FILENO);
            break;
        case REDIRECT_OUTPUT:
            dup2(STDOUT, STDOUT_FILENO);
            break;
        case REDIRECT_APPEND:
            dup2(STDOUT, STDOUT_FILENO);
            break;
    }

    // clean up these duplicated ones
    close(STDIN);
    close(STDOUT);
    close(STDERR);
    close(fd_in);
    close(fd_out);
}

void exec_subshell(node_t *node) {
    if (fork() == 0) {
        run_command(node->subshell.child);
        exit(0);
    } else {
        // should check multiple child usability, if fails, swap to waitpid
        wait(NULL);
    }
}

void exec_detach(node_t *node) {
    // sink in to a new process
    if (fork() == 0) {
        run_command(node->detach.child);
        exit(0);
    }
}

// pwd -L; cd /; pwd -L
void exec_sequence(node_t *node) {
    run_command(node->sequence.first);
    run_command(node->sequence.second);
}

// helpers
unsigned long hash(const char *str) {
    unsigned long hash = 5381;
    int c;

    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

// base on the current command index, decide the current pipe position
pipe_t get_pipe_t(int index, int length) {
    if (index == 0) return PIPE_START;
    if (index == length - 1) return PIPE_END;
    return PIPE_MIDDLE;
}

void signal_handler(int sig) {
    switch (sig) {
        case SIGINT:
            printf("Caught Signal: SIGINT\n");
            break;
        default:
            printf("Caught Signal: %i", sig);
    }
}
