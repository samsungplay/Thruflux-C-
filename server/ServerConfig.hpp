#pragma once

#include <string>
#include <CLI/App.hpp>

namespace server {

    class ServerConfig {
    public:
        inline static int port = 8080;
        inline static int maxSessions = 1000;
        inline static int maxReceiversPerSender = 10;
        inline static int maxMessageBytes = 65536;
        inline static int wsConnectionsPerMin = 30;
        inline static int wsConnectionsBurst = 10;
        inline static int wsMessagesPerSec = 50;
        inline static int wsMessagesBurst = 100;
        inline static int sessionCreatesPerMin = 10;
        inline static int sessionCreatesBurst = 5;
        inline static int maxWsConnections = 2000;
        inline static unsigned short wsIdleTimeout = 600;
        inline static int sessionTimeout = 86400;
        inline static std::string turnServer;
        inline static std::string turnStaticAuthSecret;
        inline static int turnStaticCredTtl = 600;

        static int initialize(int argc, char **argv) {
            CLI::App app{"Thruflux Server"};
            app.add_option("--port", port)->capture_default_str();
            app.add_option("--max-sessions", maxSessions)->capture_default_str();
            app.add_option("--max-receivers-per-sender", maxReceiversPerSender)->capture_default_str();
            app.add_option("--max-message-bytes", maxMessageBytes)->capture_default_str();
            app.add_option("--ws-connections-per-min", wsConnectionsPerMin)->capture_default_str();
            app.add_option("--ws-connections-burst", wsConnectionsBurst)->capture_default_str();
            app.add_option("--ws-messages-per-sec", wsMessagesPerSec)->capture_default_str();
            app.add_option("--ws-messages-burst", wsMessagesBurst)->capture_default_str();
            app.add_option("--session-creates-per-min", sessionCreatesPerMin)->capture_default_str();
            app.add_option("--session-creates-burst", sessionCreatesBurst)->capture_default_str();
            app.add_option("--max-ws-connections", maxWsConnections)->capture_default_str();
            app.add_option("--ws-idle-timeout", wsIdleTimeout)->capture_default_str();
            app.add_option("--session-timeout", sessionTimeout)->capture_default_str();
            app.add_option("--turn-server", turnServer);
            app.add_option("--turn-static-auth-secret", turnStaticAuthSecret);
            app.add_option("--turn-static-cred-ttl", turnStaticCredTtl)->capture_default_str();
            app.set_version_flag("--version", "Thruflux Server v0.3.0");
            try {
                app.parse(argc, argv);
            }
            catch (CLI::ParseError &e) {
                std::exit(app.exit(e));
            }
            return 0;
        }

    };



}