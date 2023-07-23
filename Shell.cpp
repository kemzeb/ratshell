/*
 * Copyright (c) 2023, Kemal Zebari <kemalzebra@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Shell.h"
#include "AST.h"
#include "FileDescription.h"
#include "Parser.h"
#include "Value.h"
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <variant>
#include <vector>

namespace RatShell {

namespace {

bool apply_redirections(std::vector<std::shared_ptr<RedirectionValue>> const& redirections, FileDescriptionCollector& fds, SavedFileDescriptions& saved_fds)
{
    std::vector<std::pair<int, int>> dups;
    FileDescriptionCollector fds_to_be_closed;

    for (auto const& redir : redirections) {
        auto fd = redir->io_number;
        auto const& redir_variant = redir->redir_variant;

        // Save fd so that we may restore it.
        saved_fds.add(fd);

        switch (redir->action) {
        case RedirectionValue::Action::Open: {
            auto const& data = std::get<RedirectionValue::PathData>(redir_variant);
            auto path = data.path;
            auto flags = data.flags;

            // Open a file using the given path.
            auto path_fd = open(path.c_str(), flags, 0666);
            if (path_fd < 0) {
                perror("open");
                return false;
            }

            fds.add(path_fd);
            dups.push_back({ path_fd, fd });
            break;
        }
        case RedirectionValue::Action::Close:
            fds_to_be_closed.add(fd);
            break;
        case RedirectionValue::Action::InputDup:
        case RedirectionValue::Action::OutputDup: {
            auto const& right_fd = std::get<int>(redir_variant);
            int flags = fcntl(right_fd, F_GETFL);

            if (flags < 0) {
                perror("fcntl");
                return false;
            }

            auto access = flags & O_ACCMODE;

            if (redir->action == RedirectionValue::Action::InputDup && access == O_WRONLY) {
                perror("not open for input");
                return false;
            }
            if (redir->action == RedirectionValue::Action::OutputDup && access == O_RDONLY) {
                perror("not open for output");
                return false;
            }

            dups.push_back({ right_fd, fd });
            break;
        }
        }
    }

    // Perform redirections.
    for (auto& dup : dups) {
        auto path_fd = dup.first;
        auto fd_to_redirect = dup.second;

        // Redirect fd to path_fd.
        if (dup2(path_fd, fd_to_redirect) < 0) {
            perror("dup2");
            return false;
        }
    }

    // Close the desired fds.
    fds_to_be_closed.collect();

    return true;
}

}

int Shell::run_single_line(std::string_view input)
{
    if (input.length() <= 1)
        return 0;

    auto node = parse(input);

    if (!node)
        return 0;

    if (node->is_syntax_error()) {
        auto err_node = std::static_pointer_cast<AST::SyntaxError>(node);
        print_error(err_node->error_message(), Error::SyntaxError);
        return 1;
    }

    auto value = node->eval();

    if (value->is_command()) {
        auto cmd = std::static_pointer_cast<CommandValue>(value);
        return run_command(cmd);
    }
    if (value->is_list()) {
        auto cmds = std::static_pointer_cast<CommandListValue>(value);
        return run_commands(cmds);
    }

    return 0;
}

int Shell::run_command(std::shared_ptr<CommandValue> const& cmd)
{
    FileDescriptionCollector fds;
    SavedFileDescriptions saved_fds;

    if (!apply_redirections(cmd->redirections, fds, saved_fds))
        return 1;

    auto pid = fork();
    if (pid < 0) {
        /// NOTE: The POSIX spec does not mention what exit code to return when fork() fails.
        return 1;
    }

    if (pid == 0) {
        fds.collect();
        return execute_process(cmd->argv);
    }

    int status {};
    wait(&status);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    return 0;
}

int Shell::run_commands(std::shared_ptr<CommandListValue> const& cmd_list)
{
    if (!cmd_list || cmd_list->cmds.empty())
        return 0;

    int rc = 0;

    if (cmd_list->is_a_pipe_sequence) {
        FileDescriptionCollector open_fds;
        SavedFileDescriptions saved_fds;
        int pipe_fds[2];

        // Initialize the first command.
        if (pipe2(pipe_fds, O_CLOEXEC) < 0) {
            perror("pipe");
            return 1;
        }

        /// NOTE: Just in case of any errors, we add any pipe-opened fds so that the
        //  destructor can close them when returning. In normal execution, they
        // shouldn't both be closed right away.
        open_fds.add(pipe_fds[0]);
        open_fds.add(pipe_fds[1]);

        saved_fds.add(STDOUT_FILENO);

        // Redirect stdout to write end of pipe.
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
            perror("dup2");
            return 1;
        }

        open_fds.clear();
        close(pipe_fds[1]);

        // this?
        auto first_cmd = cmd_list->cmds.front();
        run_command(first_cmd);

        // Restore stdout after execution completes.
        saved_fds.restore();

        for (size_t i = 1; i < cmd_list->cmds.size(); i++) {
            // Save read end of pipe in case we pipe again.
            auto pipe_read_fd = pipe_fds[0];

            // Add read end of pipe to collector in case of errors.
            open_fds.add(pipe_fds[0]);

            // If we aren't the last command, we should pipe again.
            if (i + 1 != cmd_list->cmds.size()) {
                if (pipe2(pipe_fds, O_CLOEXEC) < 0) {
                    perror("pipe");
                    return 1;
                }

                open_fds.add(pipe_fds[0]);
                open_fds.add(pipe_fds[1]);

                saved_fds.add(STDOUT_FILENO);

                // Redirect stdout to write end of pipe.
                if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
                    perror("dup2");
                    return 1;
                }

                close(pipe_fds[1]);
            }

            saved_fds.add(STDIN_FILENO);

            // Redirect stdin to read end of pipe.
            if (dup2(pipe_read_fd, STDIN_FILENO) < 0) {
                perror("dup2");
                return 1;
            }

            open_fds.clear();
            close(pipe_read_fd);

            auto next_cmd = cmd_list->cmds.at(i);
            rc = run_command(next_cmd);

            saved_fds.restore();
        }
    }

    return rc;
}
void Shell::print_error(std::string const& message, Error error)
{
    switch (error) {
    case Error::General:
        std::cerr << "ratsh (error): " + message + "\n";
        break;
    case Error::SyntaxError:
        std::cerr << "ratsh (syntax error): " + message + "\n";
        break;
    }
}

std::shared_ptr<AST::Node> Shell::parse(std::string_view input) const
{
    Parser parser { input };
    return parser.parse();
}

int Shell::execute_process(std::vector<std::string> const& argv)
{
    if (argv.empty())
        return 0;

    auto const* executable_path = argv[0].c_str();
    std::vector<char*> c_strings {};

    c_strings.reserve(argv.size());

    for (auto const& str : argv) {
        c_strings.push_back(const_cast<char*>(str.c_str()));
    }
    c_strings.push_back(NULL);

    execvp(executable_path, c_strings.data());
    exit(errno == ENOENT ? 127 : 126);
}

}