#pragma once

// Browser sign-in (OAuth 2.0 for native apps, RFC 8252): the service's sign-in page redirects the
// browser to http://localhost:<port>/?code=..., where a tiny one-shot web server waits.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace vaultik::os {

class LoopbackServer {
public:
    // Listens on 127.0.0.1 and ::1 at `port` (0: any free port). Throws Error when it cannot.
    explicit LoopbackServer(int port = 0);
    ~LoopbackServer();
    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    int port() const { return port_; }

    // Waits for the browser to arrive with query parameters (requests without any, such as
    // /favicon.ico, are answered and ignored), answers with a page saying `done_message`, and
    // returns the parameters. Throws Error when `cancelled` returns true or the time runs out.
    std::map<std::string, std::string> wait(std::chrono::seconds timeout, const std::function<bool()>& cancelled,
                                            const std::string& done_message);

private:
    int port_ = 0;
    std::uintptr_t sockets_[2] = {~std::uintptr_t{0}, ~std::uintptr_t{0}};
};

// Opens `url` in the default browser.
void open_in_browser(const std::string& url);

}  // namespace vaultik::os
