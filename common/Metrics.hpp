#pragma once
#include <deque>
#include <map>
#include <string>
#include <iostream>
#include <iomanip>
#include <boost/asio.hpp>

namespace common {
    struct SenderTransferMetrics {
        std::string receiverId;
        std::atomic<size_t> bytesSent{0};
        size_t lastSnapshot = 0;
        double ewmaThroughput = 0;
        bool started = false;
        int filesSent = 0;

        std::chrono::time_point<std::chrono::high_resolution_clock> startedTime;
        std::chrono::time_point<std::chrono::high_resolution_clock> lastTime;

        explicit SenderTransferMetrics(std::string name) : receiverId(std::move(name)){
        }
    };

    inline std::map<std::string, std::shared_ptr<SenderTransferMetrics> > senderMetrics;

    struct ReceiverMetrics {
        std::atomic<size_t> bytesReceived{0};
        size_t lastSnapshot = 0;
        double ewmaThroughput = 0;
        bool started = false;
        int filesReceived = 0;
        std::chrono::time_point<std::chrono::high_resolution_clock> startedTime;
        std::chrono::time_point<std::chrono::high_resolution_clock> lastTime;
    };

    inline ReceiverMetrics receiverMetrics;
}
