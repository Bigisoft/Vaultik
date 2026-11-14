#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

#include "vaultik/util/error.hpp"
#include "vaultik/util/keep_awake.hpp"
#include "vaultik/util/resources.hpp"
#include "vaultik/util/resources_detail.hpp"

using namespace vaultik;

TEST_CASE("parse_size accepts binary units, B/iB and /s suffixes") {
    CHECK(parse_size("1") == 1);
    CHECK(parse_size("1048576") == 1048576);
    CHECK(parse_size("512K") == 512ull * 1024);
    CHECK(parse_size("64m") == 64ull << 20);
    CHECK(parse_size("64MB") == 64ull << 20);
    CHECK(parse_size("64MiB") == 64ull << 20);
    CHECK(parse_size("50M/s") == 50ull << 20);
    CHECK(parse_size("1.5G") == 3ull << 29);
    CHECK(parse_size("2T") == 2ull << 40);
    CHECK_THROWS_AS(parse_size(""), Error);
    CHECK_THROWS_AS(parse_size("M"), Error);
    CHECK_THROWS_AS(parse_size("10X"), Error);
    CHECK_THROWS_AS(parse_size("0"), Error);
    CHECK_THROWS_AS(parse_size("1.2.3M"), Error);
}

TEST_CASE("parse_percent range") {
    CHECK(parse_percent("1") == 1);
    CHECK(parse_percent("50%") == 50);
    CHECK(parse_percent("100") == 100);
    CHECK_THROWS_AS(parse_percent("0"), Error);
    CHECK_THROWS_AS(parse_percent("101"), Error);
    CHECK_THROWS_AS(parse_percent("abc"), Error);
}

TEST_CASE("priority presets and overrides") {
    const ResourcePolicy bg = priority_preset("background");
    CHECK(bg.cpu == CpuPriority::Idle);
    CHECK(bg.io == IoPriority::Background);
    CHECK(bg.memory == MemoryPriority::Low);

    const ResourcePolicy low = priority_preset("LOW");
    CHECK(low.cpu == CpuPriority::Low);
    CHECK(low.io == IoPriority::Low);
    CHECK(low.memory == MemoryPriority::Low);

    CHECK(priority_preset("normal").is_default());
    CHECK(priority_preset("high").cpu == CpuPriority::High);
    CHECK_THROWS_AS(priority_preset("turbo"), Error);

    CHECK(parse_cpu_priority("idle") == CpuPriority::Idle);
    CHECK(parse_io_priority("background") == IoPriority::Background);
    CHECK(parse_memory_priority("normal") == MemoryPriority::Normal);
    CHECK_THROWS_AS(parse_cpu_priority("realtime"), Error);
}

TEST_CASE("memory cap below the minimum is rejected") {
    ResourcePolicy p;
    p.max_memory = 64ull << 20;
    CHECK_THROWS_AS(apply_resource_policy(p), Error);
}

TEST_CASE("read throttle holds the configured rate") {
    ResourcePolicy p;
    p.read_rate = 8ull << 20;  // 8 MiB/s
    apply_resource_policy(p);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 8; ++i) throttle_read(1u << 20);  // 8 MiB in total: about one second
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(secs > 0.8);
    CHECK(secs < 1.6);

    apply_resource_policy(ResourcePolicy{});
    const auto start2 = std::chrono::steady_clock::now();
    for (int i = 0; i < 64; ++i) throttle_read(1u << 20);  // unlimited again
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - start2).count() < 0.1);
}

TEST_CASE("CPU cap duty cycle counts every thread, also above one core") {
    using detail::cpu_cap_delay;
    CHECK(cpu_cap_delay(0.0, 1.0, 8.0) == 0.0);  // no cap
    // One core's worth (25 % of four): 2 s of CPU in 1 s of wall time is 1 s ahead.
    CHECK(cpu_cap_delay(1.0, 1.0, 2.0) == doctest::Approx(1.0));
    // Two cores' worth: 3 s of CPU (three busy threads) in 1 s is half a second ahead.
    CHECK(cpu_cap_delay(2.0, 1.0, 3.0) == doctest::Approx(0.5));
    CHECK(cpu_cap_delay(2.0, 1.0, 2.0) == 0.0);    // exactly at the cap
    CHECK(cpu_cap_delay(0.5, 2.0, 0.5) == 0.0);    // below it
    CHECK(cpu_cap_delay(0.5, 1.0, 0.501) == 0.0);  // under 5 ms is not worth a sleep
}

namespace {

// Counts what KeepAwake asks of the system.
struct FakeRequester : vaultik::PowerRequester {
    int* acquired;
    int* released;
    bool works;
    std::string* reason;
    FakeRequester(int* a, int* r, bool w, std::string* why) : acquired(a), released(r), works(w), reason(why) {}
    bool acquire(const std::string& why) override {
        ++*acquired;
        *reason = why;
        return works;
    }
    void release() override { ++*released; }
};

}  // namespace

TEST_CASE("keep awake: the power request is released on every way out of a run") {
    int acquired = 0, released = 0;
    std::string reason;
    const auto fake = [&](bool works = true) {
        return std::make_unique<FakeRequester>(&acquired, &released, works, &reason);
    };
    {
        vaultik::KeepAwake awake("backup of Docs", fake());
        CHECK(awake.active());
        CHECK(acquired == 1);
        CHECK(released == 0);
    }
    CHECK(released == 1);
    CHECK(reason == "backup of Docs");

    // An exception out of the run.
    try {
        vaultik::KeepAwake awake("check", fake());
        throw std::runtime_error("the drive went away");
    } catch (const std::runtime_error&) {
    }
    CHECK(acquired == 2);
    CHECK(released == 2);

    // An early return.
    const auto run = [&](bool early) {
        vaultik::KeepAwake awake("prune", fake());
        if (early) return 1;
        return 0;
    };
    CHECK(run(true) == 1);
    CHECK(run(false) == 0);
    CHECK(released == 4);

    // A system without a way to keep awake: nothing to release, the run goes on.
    {
        vaultik::KeepAwake awake("restore", fake(false));
        CHECK_FALSE(awake.active());
    }
    CHECK(acquired == 5);
    CHECK(released == 4);

    // The real one never throws, whether or not the system offers one.
    { vaultik::KeepAwake awake("Vaultik test"); }
}
