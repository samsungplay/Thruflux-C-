#pragma once
#include <random>

#include "Payloads.hpp"
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <boost/url.hpp>

#include "IceHandler.hpp"
#include <indicators/progress_bar.hpp>

namespace common {
    class Utils {
    public:

        static uint64_t fnv1a64(const uint8_t* data, const size_t len) {
            uint64_t hash = 1469598103934665603ull;
            for (size_t i = 0; i < len; ++i) {
                hash ^= data[i];
                hash *= 1099511628211ull;
            }
            return hash;
        }

        static size_t ceilDiv(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

        static bool getBit(const std::vector<uint8_t> &bm, uint64_t idx) {
            return (bm[idx >> 3] >> (idx & 7)) & 1;
        }

        static void setBit(std::vector<uint8_t> &bm, uint64_t idx) {
            bm[idx >> 3] |= uint8_t(1u << (idx & 7));
        }

        static std::unique_ptr<indicators::ProgressBar> createProgressBarUniquePtr(const std::string prefix) {
            return std::unique_ptr<indicators::ProgressBar>(new indicators::ProgressBar(
                indicators::option::BarWidth(32),
                indicators::option::Start("["),
                indicators::option::Fill("■"),
                indicators::option::Lead("■"),
                indicators::option::Remainder("."),
                indicators::option::End("]"),
                indicators::option::ShowPercentage(true),
                indicators::option::ShowElapsedTime(true),
                indicators::option::ShowRemainingTime(false),
                indicators::option::PrefixText(std::move(prefix)),
                indicators::option::MaxProgress(100),
                indicators::option::ForegroundColor(indicators::Color::cyan)
            ));
        }

        static indicators::ProgressBar createProgressBar(const std::string prefix) {
            return indicators::ProgressBar{
                indicators::option::BarWidth(32),
                indicators::option::Start("["),
                indicators::option::Fill("■"),
                indicators::option::Lead("■"),
                indicators::option::Remainder("."),
                indicators::option::End("]"),
                indicators::option::ShowPercentage(true),
                indicators::option::ShowElapsedTime(true),
                indicators::option::ShowRemainingTime(false),
                indicators::option::PrefixText(std::move(prefix)),
                indicators::option::MaxProgress(100),
                indicators::option::ForegroundColor(indicators::Color::cyan)
            };
        }

        static std::string sizeToReadableFormat(const double size) {
            if (size <= 0.0) return "0 B";
            if (size < 1024.0) return std::format("{:.0f} B", size);
            int exp = static_cast<int>(std::log(size) / std::log(1024));
            static const std::string units = "KMGTPE";
            exp = std::min(exp, static_cast<int>(units.length()));
            return std::format("{:.2f} {}B", size / std::pow(1024, exp), units[exp - 1]);
        }

        static std::optional<StunServer> toStunServer(const std::string_view raw) {
            auto parsedUrl = boost::urls::parse_uri(raw);
            if (!parsedUrl) {
                return std::nullopt;
            }
            const boost::urls::url_view stunUrl = *parsedUrl;
            return StunServer{
                .host = stunUrl.host(),
                .port = stunUrl.has_port() ? stunUrl.port_number() : 3478,
            };
        }

        static std::optional<TurnServer> toTurnServer(const std::string_view raw,
                                                      std::string user = "",
                                                      std::string pass = ""
        ) {
            auto parsedUrl = boost::urls::parse_uri(raw);
            if (!parsedUrl) {
                return std::nullopt;
            }
            const boost::urls::url_view turnUrl = *parsedUrl;
            int port = 3478;
            if (turnUrl.scheme() == "turns") {
                port = 5349;
            }
            if (turnUrl.has_port()) {
                port = turnUrl.port_number();
            }

            std::string username;
            std::string password;

            if (turnUrl.has_userinfo()) {
                username = turnUrl.user();
                password = turnUrl.password();
            }

            if (!user.empty() && username.empty()) username = std::move(user);
            if (!pass.empty() && password.empty()) password = std::move(pass);

            return TurnServer{
                .host = turnUrl.host(),
                .port = port,
                .username = username,
                .password = password
            };
        }

        static std::string toWebSocketURL(const std::string_view url) {
            auto parsedUrl = boost::urls::parse_uri(url);
            if (!parsedUrl) {
                return "";
            }
            boost::urls::url wsUrl = *parsedUrl;

            wsUrl.set_scheme(wsUrl.scheme() == "http" ? "ws" : "wss");
            wsUrl.set_path("/ws");
            return wsUrl.c_str();
        }

        static std::string generateNanoId(size_t length = 21) {
            static constexpr char alphabet[] = "_-0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
            static std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<std::size_t> dist{0, sizeof(alphabet) - 2};

            std::string id;

            id.reserve(length);

            for (size_t i = 0; i < length; i++) {
                id += alphabet[dist(rng)];
            }

            return id;
        }

        static std::string generateJoinCode() {
            static constexpr std::array ALPHANUMERIC = {
                'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
                'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
                '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'
            };
            thread_local std::mt19937_64 rng{std::random_device{}()};
            std::uniform_int_distribution<std::size_t> dist(0, ALPHANUMERIC.size() - 1);
            std::string code;
            code.reserve(19);

            for (int i = 0; i < 16; i++) {
                code.push_back(ALPHANUMERIC[dist(rng)]);
                if ((i + 1) % 4 == 0 && i != 15) {
                    code.push_back('-');
                }
            }
            return code;
        }

        static std::string encodeToBase64(const unsigned char *bytes, const unsigned int len) {
            const std::size_t outLen = 4 * ((len + 2) / 3);
            std::string out(outLen, '\0');

            const int written = EVP_EncodeBlock(
                reinterpret_cast<unsigned char *>(out.data()),
                bytes,
                static_cast<int>(len)
            );
            if (written < 0) {
                throw std::runtime_error("Could not generate turn credentials: base64 encoding failed!");
            }

            out.resize(written);

            return out;
        }

        static TurnCredentialsPayload generateTurnCredentials(const std::string &turnUrl, const std::string &secret,
                                                              const std::string &userId,
                                                              const std::uint64_t seconds) {
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            const std::uint64_t expiry = now + seconds;

            const std::string username = std::to_string(expiry) + ":" + userId;

            unsigned int macLen = 0;
            unsigned char mac[EVP_MAX_MD_SIZE];

            const auto *key = reinterpret_cast<const unsigned char *>(secret.data());
            const auto *message = reinterpret_cast<const unsigned char *>(username.data());
            if (const auto *result = HMAC(EVP_sha1(), key, static_cast<int>(secret.size()), message,
                                          static_cast<int>(username.size()), mac, &macLen); !result || macLen == 0) {
                throw std::runtime_error("Could not generate turn credentials: HMAC SHA1 failed!");
            }

            const std::string password = encodeToBase64(mac, macLen);
            return TurnCredentialsPayload{
                .username = username,
                .password = password,
                .turnUrl = turnUrl,
            };
        }
    };
}
