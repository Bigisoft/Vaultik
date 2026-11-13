#pragma once

// Runs the command-line program with --progress-pipe on a progress channel served by the test,
// as the app does for a task with administrator rights.

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "vaultik/platform/os.hpp"
#include "vaultik/platform/progress_channel.hpp"

namespace vaultik::test {

using os::ProgressServer;

struct EngineRun {
    std::vector<std::string> lines;
    std::string output;
    int code = -1;
    bool connected = false;
};

// Runs the command-line program with `args` and --progress-pipe on a channel served here.
// `on_line` sees each event line and may answer (send "cancel").
template <typename OnLine>
inline EngineRun run_engine_with_channel(std::vector<std::string> args, OnLine on_line, int connect_ms = 20000) {
    EngineRun r;
    ProgressServer server;
    std::vector<std::string> argv{VAULTIK_TEST_CLI};
    argv.insert(argv.end(), args.begin(), args.end());
    argv.insert(argv.end(), {"--progress-pipe", server.name(), "--progress-user", server.user()});
    std::atomic<bool> finished{false};
    std::thread engine([&] {
        try {
            r.code = os::run_command(argv, &r.output);
        } catch (const std::exception& e) {
            r.output = e.what();
        }
        finished = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(connect_ms);
    while (!finished && std::chrono::steady_clock::now() < deadline && !(r.connected = server.wait_connected(100))) {
    }
    if (!r.connected) r.connected = server.wait_connected(0);
    if (r.connected) {
        // Until the engine's end closes (it exited), or it exited and nothing more comes.
        std::string line;
        for (;;) {
            const bool was_finished = finished;
            const ProgressServer::Read got = server.read_line(line, 500);
            if (got == ProgressServer::Read::Closed) break;
            if (got == ProgressServer::Read::Timeout) {
                if (was_finished) break;
                continue;
            }
            r.lines.push_back(line);
            on_line(line, server);
        }
    }
    engine.join();
    return r;
}

}  // namespace vaultik::test
