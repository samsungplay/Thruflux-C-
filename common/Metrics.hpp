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
        size_t totalSize = 0;
        std::atomic<size_t> bytesSent{0};
        size_t lastSnapshot = 0;
        double ewmaMbps = 0.0;
        double totalMbpsSum = 0.0;
        size_t samplesCount = 0;

        explicit SenderTransferMetrics(std::string name, const size_t size) : receiverId(std::move(name)),
                                                                              totalSize(size) {
        }
    };

    inline std::map<std::string, std::shared_ptr<SenderTransferMetrics> > senderMetrics;

    struct ReceiverMetrics {
        std::atomic<size_t> bytesReceived{0};
        size_t lastSnapshot = 0;
        double ewmaMbps = 0.0;
        double totalMbpsSum = 0.0;
        size_t samplesCount = 0;
    };

    inline ReceiverMetrics receiverMetrics;

    class Benchmarker : public std::enable_shared_from_this<Benchmarker> {
        boost::asio::steady_timer timer_;
        bool isSender_;
        double averageMbps;

    public:
        Benchmarker(boost::asio::io_context &io, const bool isSender)
            : timer_(io), isSender_(isSender) {
        }

        void start() {
            printLoop();
        }

    private:
        inline static boost::asio::io_context ioContext_;

        void printLoop() {
            timer_.expires_after(std::chrono::seconds(1));

            timer_.async_wait([self = shared_from_this()](const boost::system::error_code &ec) {
                if (ec) return;

                // std::cout << "\033[2J\033[H";

                if (self->isSender_) {
                    std::cout << "--- Active Sender Transfers ---\n";
                    if (senderMetrics.empty()) {
                        std::cout << "  (No active transfers)\n";
                    }
                    for (auto &[id, stats]: senderMetrics) {
                        size_t current = stats->bytesSent;

                        double pct = 0.0;
                        if (stats->totalSize > 0)
                            pct = static_cast<double>(current) / stats->totalSize * 100.0;

                        double mbps = current - stats->lastSnapshot;
                        stats->totalMbpsSum += mbps;
                        stats->samplesCount++;
                        double averageMbps = stats->totalMbpsSum / stats->samplesCount;

                        if (stats->samplesCount == 1) {
                            stats->ewmaMbps = mbps;
                        } else {
                            stats->ewmaMbps = (mbps * 0.3) + (stats->ewmaMbps * 0.7);
                        }

                        std::cout << " | AVG: " << Utils::sizeToReadableFormat(averageMbps) << "/s"
                                << " | EWMA: " << Utils::sizeToReadableFormat(stats->ewmaMbps) << "/s | Percent:" << pct
                                << "%\n";

                        stats->lastSnapshot = current;
                    }
                } else {
                    size_t current = receiverMetrics.bytesReceived;

                    double mbps = current - receiverMetrics.lastSnapshot;
                    receiverMetrics.totalMbpsSum += mbps;
                    receiverMetrics.samplesCount++;
                    double averageMbps = receiverMetrics.totalMbpsSum / receiverMetrics.samplesCount;

                    if (receiverMetrics.samplesCount == 1) {
                        receiverMetrics.ewmaMbps = mbps;
                    } else {
                        receiverMetrics.ewmaMbps = (mbps * 0.3) + (receiverMetrics.ewmaMbps * 0.7);
                    }

                    std::cout << "--- Receiver Stats ---\n"
                            << " Total Received: " << Utils::sizeToReadableFormat(current) << "\n"
                            << " Current Speed:  " << Utils::sizeToReadableFormat(mbps) << "/s\n"
                            << " Average Speed:  " << Utils::sizeToReadableFormat(averageMbps) << "/s\n"
                            << " EWMA Speed:   " << Utils::sizeToReadableFormat(receiverMetrics.ewmaMbps) << "/s\n"
                            << " Seconds Elapsed: " << receiverMetrics.samplesCount << "s\n";

                    receiverMetrics.lastSnapshot = current;
                }

                std::cout << std::flush;
                self->printLoop();
            });
        }
    };
}
