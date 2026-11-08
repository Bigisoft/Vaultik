#include <doctest/doctest.h>

#include <chrono>

#include "vaultik/util/error.hpp"
#include "vaultik/util/resources.hpp"

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
