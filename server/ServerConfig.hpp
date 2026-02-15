#pragma once

#include <string>
#include <CLI/App.hpp>

namespace server {

    class ServerConfig {
    public:
        inline static int port = 8080;
        inline static int maxSessions = 1000;
        inline static int maxReceiversPerSender = 10;
        inline static unsigned int maxMessageBytes = 65536;
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
            app.add_option("--port", port, "Port to run the server on.")->capture_default_str();
            app.add_option("--max-sessions", maxSessions, "Max number of concurrent transfer sessions.")->capture_default_str();
            app.add_option("--max-receivers-per-sender", maxReceiversPerSender, "Max number of receivers to allow per sender")->capture_default_str();
            app.add_option("--max-message-bytes", maxMessageBytes, "Max message size of a single websocket payload in bytes")->capture_default_str();
            app.add_option("--ws-connections-per-min", wsConnectionsPerMin, "New websocket connections allowed per minute")->capture_default_str();
            app.add_option("--ws-connections-burst", wsConnectionsBurst, "New burst websocket connections allowed per minute")->capture_default_str();
            app.add_option("--ws-messages-per-sec", wsMessagesPerSec, "Websocket messages allowed per second")->capture_default_str();
            app.add_option("--ws-messages-burst", wsMessagesBurst, "Burst websocket messages allowed per second")->capture_default_str();
            app.add_option("--max-ws-connections", maxWsConnections, "Max concurrent websocket connections (includes senders and receivers).")->capture_default_str();
            app.add_option("--ws-idle-timeout", wsIdleTimeout, "Max websocket idle timeout in seconds")->capture_default_str();
            app.add_option("--session-timeout", sessionTimeout, "Max lifetime of a transfer session in seconds. When expired, transfers automatically abort.")->capture_default_str();
            app.add_option("--turn-server", turnServer, "Turn server that will be provided to the client, along with static auth secret if set. Used for REST-based TURN.");
            app.add_option("--turn-static-auth-secret", turnStaticAuthSecret, "Turn static auth secret to be provided to the client, along with turn server url if set. Used for REST-based TURN.");
            app.add_option("--turn-static-cred-ttl", turnStaticCredTtl, "Turn static auth secret credentials lifetime in seconds. Used for REST-based TURN.")->capture_default_str();
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