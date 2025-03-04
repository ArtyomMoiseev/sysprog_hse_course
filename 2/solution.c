#include "parser.h"

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int is_builtin(const char *cmd) {
    return (strcmp(cmd, "cd") == 0 || strcmp(cmd, "exit") == 0);
}

static int exec_builtin(struct command *cmd, struct command_line *line) {
    if (strcmp(cmd->exe, "cd") == 0) {
        if (cmd->arg_count != 1) {
            return 1;
        }
        if (chdir(cmd->args[0]) != 0) {
            return 1;
        }
        return 0;
    }
    if (strcmp(cmd->exe, "exit") == 0) {
        int exit_code = (cmd->arg_count > 0) ? atoi(cmd->args[0]) : 0;
        if (line && line->out_type == OUTPUT_TYPE_STDOUT) {
            command_line_delete(line);
            exit(exit_code);
        }
        return exit_code;
    }
    return 0;
}

static int execute_pipe(struct expr *start, struct expr **next_operator, bool background, const char *redir_file,
                        enum output_type redir_type) {
    int cmd_count = 0;
    struct expr *cur = start;
    while (cur && cur->type == EXPR_TYPE_COMMAND) {
        ++cmd_count;
        if (cur->next && cur->next->type == EXPR_TYPE_PIPE)
            cur = cur->next->next;
        else
            break;
    }

    if (cur && cur->next &&
        (cur->next->type == EXPR_TYPE_AND || cur->next->type == EXPR_TYPE_OR)) {
        *next_operator = cur->next;
    } else {
        *next_operator = NULL;
    }

    if (cmd_count == 1 && (strcmp((start->cmd.exe), "cd") == 0 || strcmp((start->cmd.exe), "exit") == 0)) {
        return exec_builtin(&start->cmd, NULL);
    }


    pid_t *pids = malloc(sizeof(pid_t) * cmd_count);
    if (!pids) {
        return 1;
    }


    int (*pipe_conv)[2] = NULL;
    if (cmd_count > 1) {
        pipe_conv = malloc(sizeof(int[2]) * (cmd_count - 1));
        if (!pipe_conv) {
            free(pids);
            return 1;
        }
        for (int i = 0; i < cmd_count - 1; i++) {
            if (pipe(pipe_conv[i]) < 0) {
                free(pids);
                free(pipe_conv);
                return 1;
            }
        }
    }

    int status = 0;
    struct expr *seg = start;
    for (int i = 0; i < cmd_count; i++) {
        struct command *cmd = &seg->cmd;
        pid_t pid = fork();
        if (pid < 0) {
            status = 1;
            break;
        }
        if (pid == 0) {
            if (i > 0) {
                if (dup2(pipe_conv[i - 1][0], STDIN_FILENO) < 0) {
                    exit(1);
                }
            }

            if (i < cmd_count - 1) {
                if (dup2(pipe_conv[i][1], STDOUT_FILENO) < 0) {
                    exit(1);
                }
            }


            if (i == cmd_count - 1 && redir_file != NULL) {
                int flags = O_WRONLY | O_CREAT;
                if (redir_type == OUTPUT_TYPE_FILE_NEW)
                    flags |= O_TRUNC;
                else
                    flags |= O_APPEND;
                int fd = open(redir_file, flags, 0644);
                if (fd < 0) {
                    exit(1);
                }
                if (dup2(fd, STDOUT_FILENO) < 0) {
                    close(fd);
                    exit(1);
                }
                close(fd);
            }

            if (pipe_conv) {
                for (int j = 0; j < cmd_count - 1; j++) {
                    close(pipe_conv[j][0]);
                    close(pipe_conv[j][1]);
                }
            }

            if (is_builtin(cmd->exe)) {
                if (strcmp(cmd->exe, "cd") == 0) {
                    fprintf(stderr, "Warning: cd in a pipeline does nothing\n");
                    exit(0);
                }
                if (strcmp(cmd->exe, "exit") == 0) {
                    int code = (cmd->arg_count > 0) ? atoi(cmd->args[0]) : 0;
                    exit(code);
                }
            }

            int argc = cmd->arg_count + 1;
            char **argv = malloc(sizeof(char *) * (argc + 1));
            if (!argv) {
                exit(1);
            }
            argv[0] = cmd->exe;
            for (uint32_t k = 0; k < cmd->arg_count; k++) {
                argv[k + 1] = cmd->args[k];
            }
            argv[argc] = NULL;
            execvp(cmd->exe, argv);
            free(argv);
            exit(1);
        }

        pids[i] = pid;
        if (seg->next && seg->next->type == EXPR_TYPE_PIPE)
            seg = seg->next->next;
        else
            seg = seg->next;
    }

    if (pipe_conv) {
        for (int i = 0; i < cmd_count - 1; i++) {
            close(pipe_conv[i][0]);
            close(pipe_conv[i][1]);
        }
        free(pipe_conv);
    }

    if (background) {
        free(pids);
        return 0;
    }

    for (int i = 0; i < cmd_count; i++) {
        int wstatus;
        waitpid(pids[i], &wstatus, 0);
        if (i == cmd_count - 1) {
            if (WIFEXITED(wstatus))
                status = WEXITSTATUS(wstatus);
            else
                status = 1;
        }
    }
    free(pids);
    return status;
}

static int execute_command_line(const struct command_line *line) {
    int prev_status = 0;
    const char *redir_file = (line->out_file != NULL) ? line->out_file : NULL;
    enum output_type redir_type = line->out_type;

    struct expr *cur = line->head;
    while (cur) {
        struct expr *next_op = NULL;
        int status = execute_pipe(cur, &next_op, line->is_background, redir_file, redir_type);
        prev_status = status;

        if (next_op) {
            enum expr_type op_type = next_op->type;
            cur = next_op->next;
            if (op_type == EXPR_TYPE_AND) {
                if (prev_status != 0) {
                    while (cur && cur->type == EXPR_TYPE_COMMAND) {
                        if (cur->next && cur->next->type == EXPR_TYPE_PIPE)
                            cur = cur->next->next;
                        else {
                            cur = cur->next;
                            break;
                        }
                    }
                }
            } else if (op_type == EXPR_TYPE_OR) {
                if (prev_status == 0) {
                    while (cur && cur->type == EXPR_TYPE_COMMAND) {
                        if (cur->next && cur->next->type == EXPR_TYPE_PIPE)
                            cur = cur->next->next;
                        else {
                            cur = cur->next;
                            break;
                        }
                    }
                }
            }
        } else {
            break;
        }
    }
    return prev_status;
}

int main(void) {
    const size_t buf_size = 1024;
    char buf[buf_size];
    ssize_t rc;
    int last_exit_status = 0;

    struct parser *p = parser_new();

    while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
        parser_feed(p, buf, (uint32_t) rc);
        struct command_line *line = NULL;
        while (true) {
            enum parser_error err = parser_pop_next(p, &line);
            if (err == PARSER_ERR_NONE && line == NULL)
                break;
            if (err != PARSER_ERR_NONE) {
                continue;
            }

            if (line->head && line->head->type == EXPR_TYPE_COMMAND &&
                line->head->next == NULL && is_builtin(line->head->cmd.exe)) {
                last_exit_status = exec_builtin(&line->head->cmd, line);
            } else {
                last_exit_status = execute_command_line(line);
            }
            command_line_delete(line);
        }
    }
    parser_delete(p);
    return last_exit_status;
}
