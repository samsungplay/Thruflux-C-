#pragma once
#include <agent.h>
#include <cstdint>
#include <lsquic_types.h>
#include <ranges>
#include <unordered_map>
#include <string>
#include <indicators/progress_bar.hpp>
#include <spdlog/spdlog.h>
#include <sys/fcntl.h>


namespace common {
    inline constexpr char RECEIVER_MANIFEST_RECEIVED_ACK = 0x06;
    inline constexpr char RECEIVER_TRANSFER_COMPLETE_ACK = 0x07;

    struct FileHandleCache {
        struct Entry {
            int fd;
            uint64_t lastUsed;
        };

        std::unordered_map<uint32_t, Entry> openFiles;
        std::unordered_map<uint32_t, std::string> paths;
        const size_t MAX_FDS = 64;
        uint64_t accessCounter = 0;

        void registerPath(uint32_t id, const std::string &p) {
            paths[id] = p;
        }

        int get(uint32_t id, int flags, mode_t mode = 0) {
            accessCounter++;
            if (openFiles.contains(id)) {
                openFiles[id].lastUsed = accessCounter;
                return openFiles[id].fd;
            }

            if (openFiles.size() >= MAX_FDS) {
                uint32_t evictId = 0;
                uint64_t minT = UINT64_MAX;
                for (const auto &[fd, lastUsed]: openFiles | std::views::values) {
                    if (lastUsed < minT) {
                        minT = lastUsed;
                        evictId = fd;
                    }
                }
                close(openFiles[evictId].fd);
                openFiles.erase(evictId);
            }

            const int fd = open(paths[id].c_str(), flags, mode);
            if (fd == -1) {
                spdlog::error("Failed to open file: {}, {}", id, errno);
                return -1;
            }

            openFiles[id] = {fd, accessCounter};

            return fd;
        }

        ~FileHandleCache() {
            for (auto &[fd, lastUsed]: openFiles | std::views::values) {
                close(fd);
            }
        }
    };


    struct UiRow {
        indicators::ProgressBar progressBar;

       explicit UiRow(std::string prefix) : progressBar(
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
        ) {
        }
    };

    struct ConnectionContext {
        NiceAgent *agent;
        guint streamId;
        mutable lsquic_conn_t *connection;
        sockaddr_storage localAddr;
        sockaddr_storage remoteAddr;
        std::chrono::high_resolution_clock::time_point startTime;
        std::chrono::high_resolution_clock::time_point lastTime;
        std::chrono::high_resolution_clock::time_point endTime;
        uint64_t bytesMoved = 0;
        uint64_t lastBytesMoved = 0;
        int filesMoved = 0;
        double ewmaThroughput;
        bool started = false;
        bool complete = false;
        lsquic_stream_t *manifestStream = nullptr;
        std::shared_ptr<UiRow> uiRow = nullptr;
        indicators::ProgressBar* progressBar;

        void initializeUI(std::string prefix) {
            uiRow = std::make_shared<UiRow>(prefix);
        }
    };
}
