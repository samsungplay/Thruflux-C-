#pragma once
#include <boost/asio/thread_pool.hpp>

namespace common {
    class Worker {
        inline static boost::asio::thread_pool backgroundWorker_{1};
    public:
        static boost::asio::thread_pool &backgroundWorker() {
            return backgroundWorker_;
        }

    };
}
