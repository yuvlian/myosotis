// Synchronous WinHTTP wrapper. No deps beyond winhttp.dll.
// Used by the Request patch: blocks the calling (game) thread, POSTs JSON,
// returns the response body. For a private/local server the latency is fine.

#pragma once
#include <string>
#include <string_view>

namespace myosotis::http {

struct Response {
    int status = 0;          // HTTP status code; 0 on transport failure
    std::string body;
    std::string error;       // empty on success
};

// Synchronous POST. `url` must be absolute ("https://host/path?query").
// `body` is sent as-is with Content-Type: application/json. Adds optional
// `X-Expected-Packet-Id: <id>` header when present.
Response post(const std::string& url,
              const std::string& body,
              const std::string& expected_packet_id = {});

// Synchronous GET. Used for the serverinfos redirect.
Response get(const std::string& url);

}  // namespace myosotis::http
