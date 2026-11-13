#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/platform/progress_channel.hpp"
#include "vaultik/util/error.hpp"
#include "vaultik/util/hex.hpp"
#include "vaultik/util/task_events.hpp"

#include "engine_channel.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace vaultik;
using namespace vaultik::os;
using namespace vaultik::test;

// Assertions run on the main thread only (see the tests' CMake settings): the threads below
// only record what they saw.

TEST_CASE("progress channel: only names the app makes are accepted") {
    ProgressServer server;
    CHECK(valid_progress_channel_name(server.name()));
#ifdef _WIN32
    CHECK(server.name().starts_with("\\\\.\\pipe\\vaultik-progress-"));
    CHECK(valid_progress_channel_name("\\\\.\\pipe\\vaultik-progress-0123456789abcdef0123456789abcdef"));
    for (const std::string bad : {"", "\\\\.\\pipe\\other", "\\\\.\\pipe\\vaultik-progress-", "\\\\.\\pipe\\vaultik-progress-0123",
                            "\\\\.\\pipe\\vaultik-progress-0123456789ABCDEF0123456789ABCDEF",
                            "\\\\.\\pipe\\vaultik-progress-0123456789abcdef\\x", "\\\\server\\pipe\\vaultik-progress-0123456789abcdef",
                            "C:\\vaultik-progress-0123456789abcdef", "\\\\.\\pipe\\..\\vaultik-progress-0123456789abcdef"}) {
        CAPTURE(bad);
        CHECK_FALSE(valid_progress_channel_name(bad));
    }
#else
    CHECK(server.name().find("/vaultik-progress-") != std::string::npos);
    struct stat st {};
    REQUIRE(::lstat(server.name().c_str(), &st) == 0);
    CHECK(S_ISDIR(st.st_mode));
    CHECK((st.st_mode & 0777) == 0700);
    CHECK(st.st_uid == ::getuid());
    REQUIRE(::lstat((server.name() + "/events").c_str(), &st) == 0);
    CHECK(S_ISFIFO(st.st_mode));
    CHECK((st.st_mode & 077) == 0);
    CHECK(valid_progress_channel_name("/run/user/1000/vaultik-progress-0123456789abcdef0123456789abcdef"));
    for (const std::string bad : {"", "/", "/tmp/other", "vaultik-progress-0123456789abcdef", "/tmp/vaultik-progress-0123",
                            "/tmp/vaultik-progress-0123456789abcdeg", "/tmp/vaultik-progress-0123456789abcdef/",
                            "/tmp/../etc/vaultik-progress-0123456789abcdef", "/tmp/./vaultik-progress-0123456789abcdef",
                            "//vaultik-progress-0123456789abcdef", "/tmp//vaultik-progress-0123456789abcdef",
                            "/tmp/vaultik-progress-0123456789ABCDEF"}) {
        CAPTURE(bad);
        CHECK_FALSE(valid_progress_channel_name(bad));
    }
#endif
    // The engine's end refuses such a name before touching anything.
    CHECK_THROWS_AS(ProgressClient("not-a-channel", "", 100), Error);
}

TEST_CASE("progress channel: events reach the app, and cancel reaches the engine") {
    std::optional<ProgressServer> channel(std::in_place);
    ProgressServer& server = *channel;
    std::atomic<bool> cancel{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> listener_done{false};
    std::string connect_error;
    std::vector<bool> sent;
    std::thread engine([&] {
        try {
            ProgressClient client(server.name(), server.user(), 5000);
            connected = true;
            std::thread listener([&] {
                listen_for_cancel(client, cancel);
                listener_done = true;
            });
            sent.push_back(client.send_line(R"({"event":"progress","done":1,"total":4})"));
            sent.push_back(client.send_line(R"({"event":"progress","done":2,"total":4})"));
            // Waits for the cancel, as a task polls its flag between blocks.
            for (int i = 0; i < 500 && !cancel; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            sent.push_back(client.send_line(R"({"event":"done","status":"cancelled"})"));
            // The listener ends once the app closes its end; the client must outlive it.
            for (int i = 0; i < 500 && !listener_done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (listener_done) listener.join();
            else listener.detach();
        } catch (const std::exception& e) {
            connect_error = e.what();
        }
    });
    REQUIRE(server.wait_connected(5000));
    std::string line;
    REQUIRE(server.read_line(line, 5000) == ProgressServer::Read::Line);
    CHECK(parse_task_event(line).done == 1);
    REQUIRE(server.read_line(line, 5000) == ProgressServer::Read::Line);
    CHECK(parse_task_event(line).done == 2);
    CHECK_FALSE(cancel.load());
    // Nothing more yet: the read only times out.
    CHECK(server.read_line(line, 50) == ProgressServer::Read::Timeout);
    CHECK(server.send_line("cancel"));
    REQUIRE(server.read_line(line, 5000) == ProgressServer::Read::Line);
    const TaskEvent done = parse_task_event(line);
    CHECK(done.type == TaskEvent::Type::Done);
    CHECK(done.status == "cancelled");
    CHECK(cancel.load());
    // The app closes its end: the engine's listener ends.
    channel.reset();
    engine.join();
    CHECK(connect_error.empty());
    CHECK(connected.load());
    CHECK(listener_done.load());
    CHECK(sent == std::vector<bool>{true, true, true});
}

TEST_CASE("progress channel: when the app goes away, the engine is not cancelled and stops sending") {
    std::atomic<bool> cancel{false};
    std::atomic<bool> listener_done{false};
    std::atomic<bool> app_gone{false};
    std::string error;
    std::vector<bool> sent;
    bool broken_after = false;
    std::optional<ProgressServer> server(std::in_place);
    const std::string name = server->name();
    const std::string user = server->user();
    std::thread engine([&] {
        try {
            ProgressClient client(name, user, 5000);
            std::thread listener([&] {
                listen_for_cancel(client, cancel);
                listener_done = true;
            });
            sent.push_back(client.send_line(R"({"event":"progress","done":1,"total":2})"));
            for (int i = 0; i < 500 && !app_gone; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            // The app's end is gone: sending fails (at the latest once the pipe's buffer is
            // full), and then fails at once without trying again.
            bool failed = false;
            for (int i = 0; i < 20000 && !failed; ++i)
                failed = !client.send_line(R"({"event":"progress","done":2,"total":2,"current":"some text to fill the pipe"})", 2000);
            sent.push_back(!failed);
            broken_after = client.broken();
            sent.push_back(client.send_line(R"({"event":"done"})"));
            for (int i = 0; i < 500 && !listener_done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (listener_done) listener.join();
            else listener.detach();
        } catch (const std::exception& e) {
            error = e.what();
        }
    });
    REQUIRE(server->wait_connected(5000));
    std::string line;
    REQUIRE(server->read_line(line, 5000) == ProgressServer::Read::Line);
    server.reset();  // the app closed (or crashed)
    app_gone = true;
    engine.join();
    CHECK(error.empty());
    CHECK(sent == std::vector<bool>{true, false, false});
    CHECK(broken_after);
    CHECK(listener_done.load());
    CHECK_FALSE(cancel.load());
}

TEST_CASE("progress channel: a channel of another user is refused") {
    ProgressServer server;
#ifdef _WIN32
    const std::string other = "S-1-5-18";  // the system account
#else
    const std::string other = std::to_string(::getuid() + 1);
#endif
    CHECK_THROWS_AS(ProgressClient(server.name(), other, 3000), Error);
}

TEST_CASE("progress channel: nobody serving it") {
    std::string name;
    {
        ProgressServer gone;
        name = gone.name();
    }
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(ProgressClient(name, "", 300), Error);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
}

TEST_CASE("progress events: what the app reads from the engine's lines") {
    TaskEvent e = parse_task_event(
        R"({"event":"progress","wipe":true,"pass":2,"passes":3,"verifying":false,"done":24771565322,"total":5002670096384,)"
        R"("bytes":24771565322,"speed":41744302.08,"seconds_left":119204.4,"current":"Pass 2 of 3: 0%"})");
    CHECK(e.type == TaskEvent::Type::Progress);
    CHECK(e.has_total);
    CHECK(e.done == 24771565322ull);
    CHECK(e.total == 5002670096384ull);
    CHECK(e.pass == 2);
    CHECK(e.passes == 3);
    CHECK_FALSE(e.verifying);
    CHECK(e.speed == doctest::Approx(41744302.08));
    CHECK(e.seconds_left == doctest::Approx(119204.4));
    CHECK(e.text == "Pass 2 of 3: 0%");

    e = parse_task_event(R"({"event":"progress","verifying":true,"pass":1,"passes":1,"done":5,"total":10,"speed":0,"seconds_left":-1})");
    CHECK(e.verifying);
    CHECK(e.speed == -1);  // not known yet
    CHECK(e.seconds_left == -1);

    // A recovery drive tells only its steps; an image restore no speed.
    e = parse_task_event(R"({"event":"progress","current":"Copying Windows PE from the ADK..."})");
    CHECK(e.type == TaskEvent::Type::Progress);
    CHECK_FALSE(e.has_total);
    CHECK(e.text == "Copying Windows PE from the ADK...");
    e = parse_task_event(R"({"event":"progress","image":true,"bytes":10,"done":20,"total":10,"current":"C:"})");
    CHECK(e.has_total);
    CHECK(e.done == 10);  // never past the total
    CHECK(e.speed == -1);

    e = parse_task_event(R"({"event":"step","message":"Formatting"})");
    CHECK(e.type == TaskEvent::Type::Step);
    CHECK(e.text == "Formatting");
    e = parse_task_event(R"({"event":"in_use","volumes":["E:","F:"],"message":"E:, F: are in use"})");
    CHECK(e.type == TaskEvent::Type::InUse);
    CHECK(e.volumes == std::vector<std::string>{"E:", "F:"});
    e = parse_task_event(R"({"event":"done","status":"in_use","volumes":["E:"],"message":"E: is in use"})");
    CHECK(e.type == TaskEvent::Type::Done);
    CHECK(e.status == "in_use");
    CHECK(e.volumes == std::vector<std::string>{"E:"});
    e = parse_task_event(R"({"event":"done","status":"success","message":"Erased disk 3"})");
    CHECK(e.status == "success");
    CHECK(e.text == "Erased disk 3");
    e = parse_task_event(R"({"event":"warning","message":"slow"})");
    CHECK(e.type == TaskEvent::Type::Warning);

    for (const std::string bad : {"", "not json", "[1,2]", R"({"event":5})", R"({"event":"paused"})", R"({"no":"event"})"}) {
        CAPTURE(bad);
        CHECK(parse_task_event(bad).type == TaskEvent::Type::Unknown);
    }
    // Wrong types are ignored, not fatal.
    e = parse_task_event(R"({"event":"progress","done":"x","total":10,"pass":"two","current":7})");
    CHECK(e.type == TaskEvent::Type::Progress);
    CHECK_FALSE(e.has_total);
    CHECK(e.text.empty());
}

TEST_CASE("progress events: time left in hours and minutes") {
    CHECK_FALSE(split_time_left(-1).known);
    TimeLeft t = split_time_left(119204.4);  // was shown as 1986m44s
    CHECK(t.known);
    CHECK_FALSE(t.under_a_minute);
    CHECK(t.hours == 33);
    CHECK(t.minutes == 7);  // 1986 min 44 s, rounded up
    t = split_time_left(119160);
    CHECK(t.hours == 33);
    CHECK(t.minutes == 6);
    t = split_time_left(30);
    CHECK(t.known);
    CHECK(t.under_a_minute);
    t = split_time_left(0);
    CHECK(t.under_a_minute);
    t = split_time_left(61);
    CHECK(t.hours == 0);
    CHECK(t.minutes == 2);
    t = split_time_left(3600);
    CHECK(t.hours == 1);
    CHECK(t.minutes == 0);
    t = split_time_left(3599);
    CHECK(t.hours == 1);
    CHECK(t.minutes == 0);
}

TEST_CASE("progress events: speed and time left for events without them") {
    RateEstimator r;
    CHECK(r.speed() == -1);
    r.add(0, 1000, 0);
    CHECK(r.speed() == -1);
    r.add(100, 1000, 2);
    CHECK(r.speed() == doctest::Approx(50));
    CHECK(r.seconds_left() == doctest::Approx(18));
    r.add(500, 1000, 10);
    CHECK(r.speed() == doctest::Approx(50));
    // Only the last half minute counts.
    r.add(500, 1000, 50);
    r.add(600, 1000, 60);
    CHECK(r.speed() == doctest::Approx(10));
    CHECK(r.seconds_left() == doctest::Approx(40));
    r.add(1000, 1000, 70);
    CHECK(r.seconds_left() == 0);
    // Another part (another total): started again.
    r.add(10, 2000, 61);
    CHECK(r.speed() == -1);
}

TEST_CASE("progress channel: an engine that connects after the app stopped waiting for it is still taken") {
    ProgressServer server;
    std::atomic<bool> stop{false};
    std::atomic<bool> got_cancel{false};
    std::string error;
    std::thread engine([&] {
        // Connects late, as an engine whose password prompt (pkexec) was answered late.
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        try {
            ProgressClient client(server.name(), server.user(), 5000);
            client.send_line(R"({"event":"progress","done":1,"total":2})");
            std::atomic<bool> cancel{false};
            std::thread listener([&] { listen_for_cancel(client, cancel); });
            for (int i = 0; i < 500 && !cancel; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            got_cancel = cancel.load();
            for (int i = 0; i < 500 && !stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            listener.detach();  // ends once the app's end closes; `client` outlives it below
            for (int i = 0; i < 50; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } catch (const std::exception& e) {
            error = e.what();
        }
    });
    int slow_calls = 0;
    const EngineWait w = wait_for_engine(
        server, [] { return std::optional<int>(); }, [] { return false; }, 200, [&] { ++slow_calls; });
    CHECK(w.connected);
    CHECK_FALSE(w.code.has_value());
    CHECK(slow_calls == 1);  // told once that it is slow, and the wait went on
    std::string line;
    REQUIRE(server.read_line(line, 5000) == ProgressServer::Read::Line);
    CHECK(parse_task_event(line).done == 1);
    CHECK(server.send_line("cancel"));  // and it can still be stopped
    for (int i = 0; i < 500 && !got_cancel; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stop = true;
    engine.join();
    CHECK(error.empty());
    CHECK(got_cancel.load());
}

TEST_CASE("progress channel: the wait for the engine ends when it ended, or when the window went away") {
    ProgressServer server;
    SUBCASE("it ended without connecting") {
        int polls = 0;
        bool slow = false;
        const EngineWait w = wait_for_engine(
            server, [&]() -> std::optional<int> { return ++polls >= 3 ? std::optional<int>(126) : std::nullopt; },
            [] { return false; }, -1, [&] { slow = true; });
        CHECK_FALSE(w.connected);
        CHECK(w.code == std::optional<int>(126));
        CHECK_FALSE(slow);
    }
    SUBCASE("aborted") {
        int polls = 0;
        const EngineWait w = wait_for_engine(
            server, [] { return std::optional<int>(); }, [&] { return ++polls > 2; }, -1, nullptr);
        CHECK_FALSE(w.connected);
        CHECK_FALSE(w.code.has_value());
    }
    SUBCASE("how it ended cannot be read: the error comes through") {
        CHECK_THROWS_AS(wait_for_engine(
                            server, []() -> std::optional<int> { throw Error("cannot read how the program ended"); },
                            [] { return false; }, -1, nullptr),
                        Error);
    }
}

TEST_CASE("progress channel: the engine keeps at most 4 KiB of a line, and a flood does not stop its listener") {
    std::optional<ProgressServer> channel(std::in_place);
    ProgressServer& server = *channel;
    std::vector<std::string> lines;
    std::atomic<bool> cancel{false};
    std::atomic<bool> flood_done{false};
    std::atomic<bool> listener_done{false};
    std::string error;
    std::thread engine([&] {
        try {
            ProgressClient client(server.name(), server.user(), 5000);
            std::string line;
            for (int i = 0; i < 3 && client.read_line(line); ++i) lines.push_back(line);
            std::thread listener([&] {
                listen_for_cancel(client, cancel);
                listener_done = true;
            });
            for (int i = 0; i < 3000 && !cancel; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            for (int i = 0; i < 3000 && !flood_done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            for (int i = 0; i < 500 && !listener_done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (listener_done) listener.join();
            else listener.detach();
        } catch (const std::exception& e) {
            error = e.what();
        }
    });
    REQUIRE(server.wait_connected(5000));
    // A line of 10000 bytes (only its first 4 KiB are kept), one just at the limit, and a short one.
    CHECK(server.send_line(std::string(10000, 'x')));
    CHECK(server.send_line(std::string(kMaxControlLine, 'y')));
    CHECK(server.send_line("hello"));
    // Then 4 MiB without a line end, and a cancel after it: taken, and nothing ran out of memory.
    std::string flood(4u << 20, 'z');
    flood += "\ncancel";
    CHECK(server.send_line(flood));
    for (int i = 0; i < 3000 && !cancel; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    flood_done = true;
    // The engine's end goes on until the app closes its end.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    channel.reset();
    engine.join();
    CHECK(error.empty());
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == std::string(kMaxControlLine, 'x'));
    CHECK(lines[1] == std::string(kMaxControlLine, 'y'));
    CHECK(lines[2] == "hello");
    CHECK(cancel.load());
    CHECK(listener_done.load());
}

#ifndef _WIN32
TEST_CASE("progress channel: a link to the channel's folder is not followed") {
    ProgressServer server;
    std::uint8_t rnd[16];
    random_bytes(rnd);
    const std::string link = server.name().substr(0, server.name().rfind('/') + 1) + "vaultik-progress-" + to_hex(ByteSpan(rnd, sizeof rnd));
    REQUIRE(::symlink(server.name().c_str(), link.c_str()) == 0);
    CHECK(valid_progress_channel_name(link));
    CHECK_THROWS_AS(ProgressClient(link, server.user(), 500), Error);
    ::unlink(link.c_str());
    CHECK_FALSE(server.wait_connected(0));
}
#endif

TEST_CASE("progress channel: the user a root engine expects") {
#ifdef _WIN32
    CHECK(progress_channel_user("S-1-5-21-1", nullptr) == "S-1-5-21-1");
    CHECK(progress_channel_user("", nullptr).empty());
#else
    CHECK(progress_channel_user("1000", nullptr) == "1000");
    CHECK(progress_channel_user("", nullptr).empty());
    // Under pkexec: the user that ran it, and no other.
    CHECK(progress_channel_user("", "1000") == "1000");
    CHECK(progress_channel_user("1000", "1000") == "1000");
    CHECK_THROWS_AS(progress_channel_user("0", "1000"), Error);
    CHECK_THROWS_AS(progress_channel_user("1001", "1000"), Error);
#endif
}

#ifdef _WIN32
TEST_CASE("progress channel: a process other than the engine started is dropped") {
    ProgressServer server;
    // Expects another process (the system's): this test's own client is a foreign one.
    server.expect_client(4);
    std::atomic<int> step{0};
    std::vector<bool> sent;
    std::string error;
    std::thread clients([&] {
        try {
            {
                ProgressClient foreign(server.name(), server.user(), 5000);
                step = 1;
                for (int i = 0; i < 500 && step < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                sent.push_back(foreign.send_line("{\"event\":\"done\",\"status\":\"success\"}", 1000));
            }
            // The expected one connects next.
            ProgressClient engine(server.name(), server.user(), 5000);
            sent.push_back(engine.send_line("{\"event\":\"progress\",\"done\":1,\"total\":2}"));
            step = 3;
            for (int i = 0; i < 500 && step < 4; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } catch (const std::exception& e) {
            error = e.what();
        }
    });
    for (int i = 0; i < 500 && step < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_FALSE(server.wait_connected(500));
    CHECK(server.rejected_clients() == 1);
    std::string line;
    CHECK(server.read_line(line, 50) == ProgressServer::Read::Timeout);  // nothing of the foreign one
    CHECK_FALSE(server.send_line("cancel"));
    server.expect_client(GetCurrentProcessId());
    step = 2;
    REQUIRE(server.wait_connected(5000));
    REQUIRE(server.read_line(line, 5000) == ProgressServer::Read::Line);
    CHECK(parse_task_event(line).done == 1);
    step = 4;
    clients.join();
    CHECK(error.empty());
    CHECK(sent == std::vector<bool>{false, true});
    CHECK(server.rejected_clients() == 1);
}

TEST_CASE("progress channel: an engine whose process id is not known takes no client") {
    ProgressServer server;
    // 0: the process id of the engine could not be read. Even an engine said to have ended does
    // not make a client acceptable then.
    int asked = 0;
    server.expect_client(0, [&] {
        ++asked;
        return true;
    });
    std::atomic<int> step{0};
    bool sent = true;
    std::string error;
    std::thread client([&] {
        try {
            ProgressClient c(server.name(), server.user(), 5000);
            step = 1;
            for (int i = 0; i < 500 && step < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            sent = c.send_line("{\"event\":\"done\",\"status\":\"success\"}", 1000);
        } catch (const std::exception& e) {
            error = e.what();
        }
    });
    for (int i = 0; i < 500 && step < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_FALSE(server.wait_connected(500));
    CHECK(server.rejected_clients() == 1);
    std::string line;
    CHECK(server.read_line(line, 50) == ProgressServer::Read::Timeout);
    CHECK_FALSE(server.send_line("cancel"));
    step = 2;
    client.join();
    CHECK(error.empty());
    CHECK_FALSE(sent);
    CHECK(asked == 0);
}

TEST_CASE("elevated process: an exit code that cannot be read is an error, not success") {
    // A handle that may only be waited on: its exit code cannot be read.
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
    REQUIRE(h != nullptr);
    CHECK_THROWS_AS(os::process_exit_code(h), Error);
    CloseHandle(h);
    CHECK(os::process_exit_code(GetCurrentProcess()) == static_cast<int>(STILL_ACTIVE));
}
#endif


// ---- the engine itself, through its --progress-pipe option ---------------------------------------

TEST_CASE("progress channel: the engine sends its result there") {
    // A disk that is not there: the engine connects, then reports why it stopped.
    const EngineRun r = run_engine_with_channel({"disk", "wipe", "99999", "--confirm", "ERASE DISK 99999"},
                                                [](const std::string&, ProgressServer&) {});
    CAPTURE(r.output);
    CHECK(r.connected);
    CHECK(r.code == 1);
    REQUIRE_FALSE(r.lines.empty());
    const TaskEvent last = parse_task_event(r.lines.back());
    CHECK(last.type == TaskEvent::Type::Done);
    CHECK(last.status == "failed");
    CHECK_FALSE(last.text.empty());
    // Not on its standard output (no --json there).
    CHECK(r.output.find("\"event\"") == std::string::npos);
}

TEST_CASE("progress channel: the engine refuses a name that is not a channel and says so") {
    std::string output;
#ifdef _WIN32
    const std::string bad = "\\\\.\\pipe\\something-else";
#else
    const std::string bad = "/tmp/something-else";
#endif
    const int code = os::run_command({VAULTIK_TEST_CLI, "disk", "wipe", "99999", "--confirm", "ERASE DISK 99999", "--progress-pipe", bad},
                                     &output);
    CAPTURE(output);
#ifdef _WIN32
    CHECK(code == 1);  // it ran without the channel (disk 99999 is not there)
#else
    CHECK(code == 7);  // it stopped at once
#endif
    CHECK(output.find("not a Vaultik progress channel") != std::string::npos);
}

#ifndef _WIN32
TEST_CASE("progress channel: a root engine that cannot use its channel stops at once and writes nothing") {
    // Under pkexec it has no window of its own: running on without the channel, nobody would see
    // it run or be able to stop it.
    namespace fs = std::filesystem;
    std::uint8_t rnd[16];
    random_bytes(rnd);
    const fs::path dir = fs::temp_directory_path() / ("vaultik-channel-none-" + to_hex(ByteSpan(rnd, 8)));
    // A channel name of the right form that nobody serves.
    const std::string missing = (fs::temp_directory_path() / ("vaultik-progress-" + to_hex(ByteSpan(rnd, sizeof rnd)))).string();
    REQUIRE(valid_progress_channel_name(missing));
    const std::string saved = os::get_env("VAULTIK_PASSWORD");
    os::set_env("VAULTIK_PASSWORD", "a test password");
    std::string output;
    const int code = os::run_command({VAULTIK_TEST_CLI, "init", "--repo", (dir / "repo").string(), "--progress-pipe", missing}, &output);
    os::set_env("VAULTIK_PASSWORD", saved);
    CAPTURE(output);
    CHECK(code == 7);
    CHECK(output.find("cannot use the app's progress channel") != std::string::npos);
    CHECK(output.find("nothing was done") != std::string::npos);
    CHECK_FALSE(fs::exists(dir));  // no repository was made
    CHECK_FALSE(fs::exists(missing));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("progress channel: the wait for an engine does not keep a processor busy when the channel failed") {
    ProgressServer server;
    // Gone: every wait for the engine fails at once from now on.
    REQUIRE(::unlink((server.name() + "/control").c_str()) == 0);
    CHECK_FALSE(server.wait_connected(0));
    int polls = 0;
    const auto start = std::chrono::steady_clock::now();
    const EngineWait w = wait_for_engine(
        server,
        [&]() -> std::optional<int> {
            ++polls;
            return std::nullopt;
        },
        [&] { return std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(1000); }, -1, nullptr);
    CHECK_FALSE(w.connected);
    CHECK_FALSE(w.code.has_value());
    CAPTURE(polls);
    CHECK(polls >= 2);
    CHECK(polls <= 10);  // about five times a second, not in a busy loop
}

namespace {

int open_descriptors() {
    int n = 0;
    for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator("/proc/self/fd")) ++n;
    return n;
}

}  // namespace

TEST_CASE("progress channel: an engine end that cannot be opened leaves no descriptor open") {
    namespace fs = std::filesystem;
    std::uint8_t rnd[16];
    random_bytes(rnd);
    const std::string name = (fs::temp_directory_path() / ("vaultik-progress-" + to_hex(ByteSpan(rnd, sizeof rnd)))).string();
    REQUIRE(::mkdir(name.c_str(), 0700) == 0);
    REQUIRE(::chmod(name.c_str(), 0700) == 0);
    REQUIRE(::mkfifo((name + "/control").c_str(), 0600) == 0);
    SUBCASE("the second one is missing") {
        const int before = open_descriptors();
        CHECK_THROWS_AS(ProgressClient(name, "", 200), Error);
        CHECK(open_descriptors() == before);
    }
    SUBCASE("the second one is not a pipe") {
        const int f = ::open((name + "/events").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        REQUIRE(f >= 0);
        ::close(f);
        const int before = open_descriptors();
        CHECK_THROWS_WITH_AS(ProgressClient(name, "", 200), doctest::Contains("not a private pipe"), Error);
        CHECK(open_descriptors() == before);
    }
    std::error_code ec;
    fs::remove_all(name, ec);
}
#endif

TEST_CASE("progress channel: a question the engine would ask fails with an error the app shows") {
    // Its console is hidden: a password prompt would wait there forever.
    namespace fs = std::filesystem;
    std::uint8_t rnd[8];
    random_bytes(rnd);
    const fs::path dir = fs::temp_directory_path() / ("vaultik-channel-prompt-" + to_hex(ByteSpan(rnd, sizeof rnd)));
    const std::string saved = os::get_env("VAULTIK_PASSWORD");
    os::set_env("VAULTIK_PASSWORD", "");
    const auto start = std::chrono::steady_clock::now();
    const EngineRun r = run_engine_with_channel({"init", "--repo", (dir / "repo").string()}, [](const std::string&, ProgressServer&) {});
    os::set_env("VAULTIK_PASSWORD", saved);
    std::error_code ec;
    fs::remove_all(dir, ec);
    CAPTURE(r.output);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(30));
    CHECK(r.connected);
    CHECK(r.code == 1);
    REQUIRE_FALSE(r.lines.empty());
    const TaskEvent last = parse_task_event(r.lines.back());
    CHECK(last.type == TaskEvent::Type::Done);
    CHECK(last.status == "failed");
    CHECK(last.text.find("password") != std::string::npos);
    CHECK(last.text.find("cannot answer questions") != std::string::npos);
}
