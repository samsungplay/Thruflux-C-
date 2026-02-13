#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <CLI/App.hpp>
#include <spdlog/spdlog.h>

namespace sender {

class SenderConfig {
public:
    inline static std::vector<std::string> paths;
    inline static std::string serverUrl = "http://localhost:8080";
    inline static int maxReceivers = 4;
    inline static std::string stunServers =
        "stun://stun.l.google.com:19302,"
        "stun://stun.cloudflare.com:3478,"
        "stun://stun.bytepipe.app:3478";
    inline static std::string turnServers;
    inline static bool testTurn = false;
    inline static std::int64_t quicStreamWindowBytes = 67108864;
    inline static int quicMaxIncomingStreams = 100;
    inline static int chunkSize = 16384;
    inline static int totalConnections = 1;
    inline static int totalStreams = 8;
    inline static int udpBufferBytes = 8388608;
    inline static bool benchmark = false;
    inline static bool verbose = false;
    inline static std::int64_t quicConnWindowBytes = 512 * 1024 * 1024;

    static void initialize(int argc, char** argv) {
        CLI::App app{"Thruflux Sender"};

        app.add_option("PATHS", paths)
            ->required()
            ->check(CLI::ExistingPath);

        app.add_option("--server-url", serverUrl)->capture_default_str();
        app.add_option("--max-receivers", maxReceivers)->capture_default_str();
        app.add_option("--stun-server", stunServers)->capture_default_str();
        app.add_option("--turn-server", turnServers);
        app.add_flag("--test-turn", testTurn);
        app.add_option("--quic-stream-window-bytes", quicStreamWindowBytes)->capture_default_str();
        app.add_option("--quic-max-incoming-streams", quicMaxIncomingStreams)->capture_default_str();
        app.add_option("--quic-conn-window-bytes", quicConnWindowBytes)->capture_default_str();
        app.add_option("--chunk-size", chunkSize)->capture_default_str();
        app.add_option("--total-connections", totalConnections)->capture_default_str();
        app.add_option("--total-streams", totalStreams)->capture_default_str();
        app.add_option("--udp-buffer-bytes", udpBufferBytes)->capture_default_str();
        app.add_flag("--benchmark", benchmark);
        app.add_flag("--verbose", verbose);
        app.set_version_flag("--version", "Thruflux v0.3.0");
        try {
            app.parse(argc, argv);
        }
        catch (const CLI::ParseError &e) {
            std::exit(app.exit(e));
        }
    }
};

}
