#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vaultik::cli {

void init_console();

// POSIX: true once SIGINT, SIGTERM or SIGHUP arrived (Ctrl+C, "systemctl --user stop", a closed
// terminal). The run then stops cleanly at the next check, releasing its repository lock; a
// second signal ends the program at once. Windows: always false.
bool interrupted();

void out(std::string_view text);  // stdout, UTF-8
void err(std::string_view text);  // stderr, UTF-8

// Single-line status on stderr that overwrites itself; no-op when stderr is not a console.
void status(std::string_view text);
void clear_status();

// Reads a line from the console without echo. Throws when stdin is not a console, with
// `without_console` as the message when given (it says where else the secret can come from).
std::string read_password(std::string_view prompt, std::string_view without_console = {});
// Reads a line typed at the console (shown as typed). Throws when there is no console and stdin
// has ended.
std::string read_line(std::string_view prompt);

// From now on read_password and read_line ask nothing and throw an Error saying `why` (with
// the message of read_password's `without_console`, or the default one, which says where else
// a password can come from). For a run whose console window is hidden (started from the app
// with a progress channel): a prompt there would wait forever.
void disable_prompts(std::string why);
// Throws that Error when prompts were disabled, with `without_console` before the reason.
void check_prompts_allowed(std::string_view without_console);
// The message for a read_line prompt that cannot be shown.
std::string prompt_question(std::string_view prompt);

std::string format_bytes(std::uint64_t bytes);
std::string format_duration(double seconds);

}  // namespace vaultik::cli
