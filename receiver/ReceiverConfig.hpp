#pragma once

#include <CLI/App.hpp>
#include <CLI/Validators.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace receiver {
    class ReceiverConfig {
    public:
        inline static std::string joinCode;
        inline static std::string out = ".";
        inline static std::string serverUrl = "http://localhost:8080";

        inline static std::string stunServers = "stun://stun.cloudflare.com:3478";
        inline static std::string turnServers;
        inline static bool forceTurn = false;

        inline static std::int64_t quicConnWindowBytes = 256LL * 1024 * 1024;
        inline static std::int64_t quicStreamWindowBytes = 32LL * 1024 * 1024;
        inline static int quicMaxStreams = 100;

        inline static int totalStreams = 4;

        inline static bool overwrite = false;

        inline static int udpBufferBytes = 8 * 1024 * 1024;

        static void initialize(int argc, char **argv) {
            CLI::App app{"Thruflux Receiver"};

            const auto isHttpUrl = CLI::Validator(
                [](const std::string &s) -> std::string {
                    if (s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0) return {};
                    return "must start with http:// or https://";
                },
                "HTTP_URL"
            );

            const auto isStunUrl = CLI::Validator(
                [](const std::string &s) -> std::string {
                    if (s.empty()) return "cannot be empty";
                    const bool ok =
                            (s.rfind("stun://", 0) == 0) ||
                            (s.rfind("stuns://", 0) == 0);
                    if (!ok) return "must start with stun:// or stuns://";

                    return {};
                },
                "STUN_URL"
            );

            const auto isTurnUrl = CLI::Validator(
                [](const std::string &s) -> std::string {
                    if (s.empty()) return {};

                    const bool ok =
                            (s.rfind("turn://", 0) == 0) ||
                            (s.rfind("turns://", 0) == 0);
                    if (!ok) return "must start with turn://, turns://, or turn:";

                    return {};
                },
                "TURN_URL"
            );


            constexpr std::int64_t KiB = 1024;
            constexpr std::int64_t MiB = 1024 * KiB;
            constexpr std::int64_t GiB = 1024 * MiB;

            app.add_option("JOIN_CODE", joinCode, "Join code for the transfer")
                    ->required();

            app.add_option("--out", out, "Output directory")
                    ->check(CLI::ExistingDirectory)
                    ->capture_default_str();

            app.add_option("--server-url", serverUrl, "HTTP(S) URL of signaling server")
                    ->check(isHttpUrl)
                    ->capture_default_str();

            app.add_option("--stun-server", stunServers, "STUN server URL")
                    ->check(isStunUrl)
                    ->capture_default_str();

            app.add_option("--turn-server", turnServers,
                           "TURN server URL (optional). Example: turn://user:pass@turn.example.com:3478")
                    ->check(isTurnUrl);

            app.add_flag("--force-turn", forceTurn, "Force TURN relay");

            app.add_option("--quic-conn-window-bytes", quicConnWindowBytes,
                           "Initial QUIC connection flow-control window (bytes)")
                    ->check(CLI::Range(1 * MiB, 8 * GiB))
                    ->capture_default_str();

            app.add_option("--quic-stream-window-bytes", quicStreamWindowBytes,
                           "Initial QUIC stream flow-control window (bytes)")
                    ->check(CLI::Range(256 * KiB, 2 * GiB))
                    ->capture_default_str();

            app.add_option("--quic-max-streams", quicMaxStreams,
                           "Max QUIC streams allowed")
                    ->check(CLI::Range(1, 100000))
                    ->capture_default_str();

            app.add_option("--total-streams", totalStreams,
                           "Concurrent data streams to open. Increasing this does not necessarily accelerate transfers.")
                    ->check(CLI::Range(1, 1024))
                    ->capture_default_str();


            app.add_flag("--overwrite", overwrite, "Overwrite existing files (disable resume)");

            app.add_option("--udp-buffer-bytes", udpBufferBytes,
                           "UDP socket buffer size (bytes)")
                    ->check(CLI::Range(256 * 1024, 256 * 1024 * 1024))
                    ->capture_default_str();

            app.set_version_flag("--version", "Thruflux v0.3.0");

            app.parse_complete_callback([&]() {
                if (forceTurn && turnServers.empty()) {
                    throw CLI::ValidationError("--force-turn",
                                               "requires --turn-server to be set");
                }

                if (quicConnWindowBytes < quicStreamWindowBytes) {
                    throw CLI::ValidationError("--quic-conn-window-bytes",
                                               "must be >= --quic-stream-window-bytes");
                }

                if (totalStreams > quicMaxStreams) {
                    throw CLI::ValidationError("--total-streams",
                                               "must be <= --quic-max-incoming-streams");
                }

                if (udpBufferBytes < 1024 * 1024) {
                    spdlog::warn("udp-buffer-bytes is < 1MiB; this may limit throughput");
                }
            });

            try {
                app.parse(argc, argv);
            } catch (const CLI::ParseError &e) {
                std::exit(app.exit(e));
            }
        }
    };
}
