#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

static int
execute_command_line(const struct command_line *line)
{
    if (line->head == NULL)
        return 0;

    int last_status = 0;

    const struct expr *expr = line->head;

    while (expr != NULL) {
        while (expr != NULL && expr->type != EXPR_TYPE_COMMAND) {
            expr = expr->next;
        }
        if (expr == NULL)
          break;

        const struct expr *pipeline_start = expr;
        const struct expr *iter = expr;
        int num_cmds = 1;
        while (iter->next && iter->next->type == EXPR_TYPE_PIPE) {
            iter = iter->next->next;
            num_cmds++;
            if (iter == NULL) break;
        }
        const struct expr *pipeline_end = iter;
        const struct expr *next_expr = pipeline_end ? pipeline_end->next : NULL;

        if (num_cmds == 1) {
            const struct command *cmd = &pipeline_start->cmd;
            if (strcmp(cmd->exe, "cd") == 0) {
                if (cmd->arg_count > 0) {
                    if (chdir(cmd->args[0]) != 0) {
                        last_status = 1;
                    } else {
                        last_status = 0;
                    }
                } else {
                    last_status = 0;
                }
            }
            else if (strcmp(cmd->exe, "exit") == 0) {
                int code = 0;
                if (cmd->arg_count > 0) code = atoi(cmd->args[0]);
                _exit(code);
            }
            else {
                pid_t pid = fork();
                if (pid == 0) {
                    if (line->out_type != OUTPUT_TYPE_STDOUT) {
                        int fd;
                        if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
                            fd = open(line->out_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
                        } else {
                            fd = open(line->out_file, O_CREAT | O_APPEND | O_WRONLY, 0644);
                        }
                        if (fd < 0) {
                            _exit(1);
                        }
                        dup2(fd, STDOUT_FILENO);
                        close(fd);
                    }
                    size_t argc = cmd->arg_count;
                    char **argv = malloc((argc + 2) * sizeof(char*));
                    if (!argv) {
                        _exit(1);
                    }
                    argv[0] = cmd->exe;
                    for (size_t i = 0; i < argc; ++i) argv[i+1] = cmd->args[i];
                    argv[argc+1] = NULL;
                    execvp(cmd->exe, argv);
                    _exit(1);
                } else if (pid < 0) {
                    last_status = 1;
                } else {
                    int status;
                    waitpid(pid, &status, 0);
                    if (WIFEXITED(status)) last_status = WEXITSTATUS(status);
                    else if (WIFSIGNALED(status)) last_status = 128 + WTERMSIG(status);
                    else last_status = 1;
                }
            }
        }
        else
        {
            int prev_fd = -1;
            pid_t *pids = malloc(num_cmds * sizeof(pid_t));
            if (!pids) {
                return 1;
            }
            const struct expr *current = pipeline_start;
            for (int i = 0; i < num_cmds; ++i) pids[i] = -1;
            for (int i = 0; i < num_cmds; ++i) {
                int pipefd[2];
                if (i < num_cmds - 1 && pipe(pipefd) < 0) {
                    break;
                }
                pid_t cpid = fork();
                if (cpid == 0) {
                    if (prev_fd != -1) dup2(prev_fd, STDIN_FILENO);
                    if (i < num_cmds - 1) dup2(pipefd[1], STDOUT_FILENO);
                    else if (line->out_type != OUTPUT_TYPE_STDOUT) {
                        int fd;
                        if (line->out_type == OUTPUT_TYPE_FILE_NEW)
                            fd = open(line->out_file, O_CREAT|O_TRUNC|O_WRONLY,0644);
                        else
                            fd = open(line->out_file, O_CREAT|O_APPEND|O_WRONLY,0644);
                        if (fd < 0) {_exit(1); }
                        dup2(fd, STDOUT_FILENO);
                        close(fd);
                    }
                    if (prev_fd != -1) close(prev_fd);
                    if (i < num_cmds - 1) { close(pipefd[0]); close(pipefd[1]); }
                    const struct command *cmd = &current->cmd;
                    if (strcmp(cmd->exe, "cd") == 0) {
                        if (cmd->arg_count > 0) chdir(cmd->args[0]);
                        _exit(0);
                    }
                    else if (strcmp(cmd->exe, "exit") == 0) {
                        int code = 0;
                        if (cmd->arg_count > 0) code = atoi(cmd->args[0]);
                        _exit(code);
                    }
                    else
                    {
                        size_t argc = cmd->arg_count;
                        char **argv = malloc((argc+2)*sizeof(char*));
                        if (!argv) { _exit(1); }
                        argv[0] = cmd->exe;
                        for (size_t j = 0; j < argc; ++j) argv[j+1] = cmd->args[j];
                        argv[argc+1] = NULL;
                        execvp(cmd->exe, argv);
                        _exit(1);
                    }
                } else if (cpid < 0) {
                    if (i < num_cmds - 1) { close(pipefd[0]); close(pipefd[1]); }
                } else {
                    pids[i] = cpid;
                    if (prev_fd != -1) close(prev_fd);
                    if (i < num_cmds - 1) { close(pipefd[1]); prev_fd = pipefd[0]; }
                    if (current->next && current->next->type == EXPR_TYPE_PIPE)
                        current = current->next->next;
                }
            }
            if (prev_fd != -1) close(prev_fd);
            last_status = 0;
            for (int i = 0; i < num_cmds; ++i) {
                if (pids[i] == -1) continue;
                int status;
                waitpid(pids[i], &status, 0);
                if (i == num_cmds - 1) {
                    if (WIFEXITED(status)) last_status = WEXITSTATUS(status);
                    else if (WIFSIGNALED(status)) last_status = 128 + WTERMSIG(status);
                    else last_status = 1;
                }
            }
            free(pids);
        }

        if (next_expr && next_expr->type == EXPR_TYPE_AND) {
            if (last_status != 0) {
                const struct expr *skip = next_expr;
                while (skip && skip->type != EXPR_TYPE_OR) skip = skip->next;
                expr = skip;
                continue;
            } else {
                expr = next_expr->next;
                continue;
            }
        } else if (next_expr && next_expr->type == EXPR_TYPE_OR) {
            if (last_status == 0) {
                const struct expr *skip = next_expr;
                while (skip && skip->type != EXPR_TYPE_AND) skip = skip->next;
                expr = skip;
                continue;
            } else {
                expr = next_expr->next;
                continue;
            }
        } else {
            expr = next_expr;
            continue;
        }
    }
    return last_status;
}

int
main(void)
{
	const size_t buf_size = 1024;
	char buf[buf_size];
	int rc;
    int last_status = 0;
	struct parser *p = parser_new();
	while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
		parser_feed(p, buf, rc);
		struct command_line *line = NULL;
		while (true) {
			enum parser_error err = parser_pop_next(p, &line);
			if (err == PARSER_ERR_NONE && line == NULL)
				break;
			if (err != PARSER_ERR_NONE) {
				printf("Error: %d\n", (int)err);
				continue;
			}
			last_status = execute_command_line(line);
			command_line_delete(line);
		}
	}
	parser_delete(p);
    return last_status;
}
