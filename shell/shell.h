#ifndef SHELL_H
#define SHELL_H

struct tree_node;

/*
 * Any value assigned to this will be displayed when asking the user for a
 * command. Do not assign any value to this if its value is NULL, as this
 * indicates the session is not interactive.
 */
extern char *prompt;

/*
 * Called once when the shell starts.
 */
void initialize(void);

/*
 * Called when a command has been read from the user.
 */
void run_command(struct tree_node *n);

/* ... */

// Built-in helpers
#define CD 5863276
#define EXIT 6385204799
#define DIR_LEN 1024
#define SET 193505681
#define UNSET 210730384244
#define ENV 193490734

int *env_fd;

// calculate string hash for switch case
unsigned long hash(const char *str);

int exec_cd(struct tree_node *n);

void exec_exit(struct tree_node *n);

void exec_set(struct tree_node *n);

void exec_unset(struct tree_node *n);

void exec_env();

int exec_command(struct tree_node *n);

// Pipe and helpers
#define PIPE_RD 0
#define PIPE_WR 1
typedef enum {PIPE_START, PIPE_MIDDLE, PIPE_END} pipe_t;

void exec_pipe(struct tree_node *n);

pipe_t get_pipe_t(int index, int length);

// redirect
void exec_redirect(struct tree_node *n);

// subshell
void exec_subshell(struct tree_node *n);

// detach
void exec_detach(struct tree_node *n);

// signal handler
void signal_handler(int sig);

// sequence
void exec_sequence(struct tree_node *n);

#endif
