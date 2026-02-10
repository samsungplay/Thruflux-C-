#pragma once
#include "../common/Payloads.hpp"

namespace sender {
    struct ReceiverInfo {
        std::atomic<int> files{0};
        std::atomic<int> resumed{0};
        std::atomic<float> percent{0.0f};
        std::atomic<std::uint64_t> ratePerSecond{0};
        std::atomic<std::uint64_t> eta{0};

        mutable std::mutex string_mutex;
        std::string receiverId;
        std::string status;
        std::string link;

        explicit ReceiverInfo(std::string id) : receiverId(std::move(id)), status("CONNECTING"), link("?") {
        }

        std::string getReceiverId() const {
            std::lock_guard lock(string_mutex);
            return receiverId;
        }

        std::string getStatus() const {
            std::lock_guard lock(string_mutex);
            return status;
        }

        std::string getLink() const {
            std::lock_guard lock(string_mutex);
            return link;
        }

        void setStatus(std::string newStatus) {
            std::lock_guard lock(string_mutex);
            status = std::move(newStatus);
        }

        int getFiles() const {
            return files.load();
        }

        int getResumed() const {
            return resumed.load();
        }

        float getPercent() const {
            return percent.load();
        }

        std::uint64_t getRatePerSecond() const {
            return ratePerSecond.load();
        }

        std::uint64_t getEta() const {
            return eta.load();
        }

        void setFiles(int val) {
            files.store(val);
        }

        void setPercent(float val) {
            percent.store(val);
        }
    };


    class SenderStateHolder {
        inline static common::CreateTransferSessionPayload manifest_;
        inline static std::unordered_map<std::string, std::shared_ptr<ReceiverInfo> > receivers_;
        inline static std::mutex mutex;
        inline static std::string joinCode_;

    public:
        static void setManifest(common::CreateTransferSessionPayload manifest) {
            std::lock_guard lock(mutex);
            manifest_ = std::move(manifest);
        }

        static common::CreateTransferSessionPayload manifest() {
            std::lock_guard lock(mutex);
            return manifest_;
        }

        static void addReceiver(std::string receiverId) {
            std::lock_guard lock(mutex);
            receivers_.insert_or_assign(receiverId, std::make_shared<ReceiverInfo>(std::move(receiverId)));
        }

        static void setJoinCode(std::string joinCode) {
            std::lock_guard lock(mutex);
            joinCode_ = std::move(joinCode);
        }

        static std::string getJoinCode() {
            std::lock_guard lock(mutex);
            return joinCode_;
        }

        static std::vector<std::shared_ptr<ReceiverInfo> > getReceivers() {
            std::lock_guard lock(mutex);
            std::vector<std::shared_ptr<ReceiverInfo> > snapshot;
            for (const auto &info: receivers_ | std::views::values) {
                snapshot.push_back(info);
            }
            return snapshot;
        }

        static std::shared_ptr<ReceiverInfo> getReceiverInfo(const std::string &receiverId) {
            std::lock_guard lock(mutex);
            const auto it = receivers_.find(receiverId);
            if (it == receivers_.end()) {
                return nullptr;
            }
            return it->second;
        }

        static void removeReceiver(const std::string &receiverId) {
            std::lock_guard lock(mutex);
            receivers_.erase(receiverId);
        }
    };
}
