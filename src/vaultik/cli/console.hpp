#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vaultik::cli {

void init_console();

void out(std::string_view text);  // stdout, UTF-8
void err(std::string_view text);  // stderr, UTF-8

// Single-line status on stderr that overwrites itself; no-op when stderr is not a console.
void status(std::string_view text);
void clear_status();

// Reads a line from the console without echo. Throws when stdin is not a console.
std::string read_password(std::string_view prompt);
// Reads a line typed at the console (shown as typed). Throws when there is no console and stdin
// has ended.
std::string read_line(std::string_view prompt);

std::string format_bytes(std::uint64_t bytes);
std::string format_duration(double seconds);

}  // namespace vaultik::cli
