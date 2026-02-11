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
        inline static std::string joinCode_;
        inline static std::vector<std::string> absolutePaths_;
        inline static std::vector<std::string> relativePaths_;
        inline static std::uint64_t totalExpectedBytes_;
        inline static int totalExpectedFilesCount_;

    public:

        static void setTotalExpectedFilesCount(int count) {
            totalExpectedFilesCount_ = count;
        }
        static void setTotalExpectedBytes(std::uint64_t bytes) {
            totalExpectedBytes_ = bytes;
        }

        static void setAbsolutePaths(std::vector<std::string> paths) {
            absolutePaths_ = std::move(paths);
        }
        static void setRelativePaths(std::vector<std::string> paths) {
            relativePaths_ = std::move(paths);
        }

        static std::vector<std::string>& getAbsolutePaths() {
            return absolutePaths_;
        }

        static int getTotalExpectedFilesCount() {
            return totalExpectedFilesCount_;
        }

        static std::vector<std::string>& getRelativePaths() {
            return relativePaths_;
        }

        static std::uint64_t getTotalExpectedBytes() {
            return totalExpectedBytes_;
        }

        static void addReceiver(std::string receiverId) {
            receivers_.insert_or_assign(receiverId, std::make_shared<ReceiverInfo>(std::move(receiverId)));
        }

        static void setJoinCode(std::string joinCode) {
            joinCode_ = std::move(joinCode);
        }

        static std::string getJoinCode() {
            return joinCode_;
        }

        static std::vector<std::shared_ptr<ReceiverInfo> > getReceivers() {
            std::vector<std::shared_ptr<ReceiverInfo> > snapshot;
            for (const auto &info: receivers_ | std::views::values) {
                snapshot.push_back(info);
            }
            return snapshot;
        }

        static std::shared_ptr<ReceiverInfo> getReceiverInfo(const std::string &receiverId) {
            const auto it = receivers_.find(receiverId);
            if (it == receivers_.end()) {
                return nullptr;
            }
            return it->second;
        }

        static void removeReceiver(const std::string &receiverId) {
            receivers_.erase(receiverId);
        }
    };
}
