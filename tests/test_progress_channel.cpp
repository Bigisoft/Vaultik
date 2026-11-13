#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "vaultik/platform/os.hpp"
#include "vaultik/platform/progress_channel.hpp"
#include "vaultik/util/task_events.hpp"

#include "engine_channel.hpp"

#ifndef _WIN32
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
    CHECK(code == 1);
    CHECK(output.find("not a Vaultik progress channel") != std::string::npos);
}
