#pragma once
#include <string>
#include <uwebsockets/App.h>

namespace common {
    struct SocketUserData {
        std::string id;
        std::string role;
    };
    using Session = uWS::WebSocket<false, true, SocketUserData>;
}
