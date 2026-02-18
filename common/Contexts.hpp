#pragma once
#include <agent.h>
#include <cstdint>
#include <lsquic_types.h>
#include <ranges>
#include <unordered_map>
#include <string>
#include <indicators/progress_bar.hpp>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
typedef int mode_t;
#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_RDWR _O_RDWR
#define S_IRUSR _S_IREAD
#define S_IWUSR _S_IWRITE
#endif


namespace common {
    inline constexpr char RECEIVER_MANIFEST_RECEIVED_ACK = 0x06;
    inline constexpr char RECEIVER_TRANSFER_COMPLETE_ACK = 0x07;
    inline static constexpr uint64_t CHUNK_SIZE = 4 * 1024 * 1024; //controls resume granularity & scheduling

    struct FileHandleCache {
        struct Entry {
            int fd = -1;
            bool open = false;
            int prev = -1;
            int next = -1;
        };

        size_t maxFds = 128;
        std::vector<std::string> paths;
        std::vector<Entry> entries;

        int head = -1;
        int tail = -1;
        size_t openCount = 0;

        FileHandleCache() = default;

        explicit FileHandleCache(size_t fileCount, size_t maxFds_ = 128) {
            reset(fileCount, maxFds_);
        }

        void reset(size_t fileCount, size_t maxFds_ = 128) {
            closeAll();
            maxFds = maxFds_;
            paths.assign(fileCount, {});
            entries.assign(fileCount, {});
            head = tail = -1;
            openCount = 0;
        }

        void registerPath(uint32_t id, std::string p) {
            if (id >= paths.size()) {
                paths.resize(id + 1);
                entries.resize(id + 1);
            }
            paths[id] = std::move(p);
        }

        int get(uint32_t id, int flags, mode_t mode = 0) {
            if (id >= entries.size() || id >= paths.size()) {
                spdlog::error("FileHandleCache: id {} out of range", id);
                return -1;
            }
            if (paths[id].empty()) {
                spdlog::error("FileHandleCache: path not registered for id {}", id);
                return -1;
            }

            Entry &e = entries[id];
            if (e.open && e.fd != -1) {
                touch((int) id);
                return e.fd;
            }

            while (openCount >= maxFds) {
                if (!evictOne()) break;
            }
            if (openCount >= maxFds) {
                spdlog::error("FileHandleCache: cannot evict (maxFds={})", maxFds);
                return -1;
            }

            const int fd = ::open(paths[id].c_str(), flags, mode);
            if (fd == -1) {
                spdlog::error("Failed to open file id {} path='{}' errno={}", id, paths[id], errno);
                return -1;
            }

            if (e.prev != -1 || e.next != -1 || head == (int) id || tail == (int) id) {
                removeFromList((int) id);
            }

            e.fd = fd;
            e.open = true;
            pushFront((int) id);
            openCount++;
            return fd;
        }

        void closeAll() {
            for (auto &e: entries) {
                if (e.open && e.fd != -1) ::close(e.fd);
                e = Entry{};
            }
            head = tail = -1;
            openCount = 0;
        }

        ~FileHandleCache() { closeAll(); }

    private:
        void removeFromList(int id) {
            if (id < 0 || (size_t) id >= entries.size()) return;
            Entry &e = entries[id];
            if (e.prev == -1 && e.next == -1 && head != id && tail != id) {
                e.prev = e.next = -1;
                return;
            }

            const int p = e.prev;
            const int n = e.next;

            if (p != -1) entries[p].next = n;
            else if (head == id) head = n;

            if (n != -1) entries[n].prev = p;
            else if (tail == id) tail = p;

            if (head == -1) tail = -1;

            e.prev = e.next = -1;
        }

        void pushFront(int id) {
            Entry &e = entries[id];
            e.prev = -1;
            e.next = head;
            if (head != -1) entries[head].prev = id;
            head = id;
            if (tail == -1) tail = id;
        }

        void touch(int id) {
            if (head == id) return;
            removeFromList(id);
            pushFront(id);
        }

        bool evictOne() {
            int id = tail;
            if (id == -1) return false;

            Entry &e = entries[id];
            if (e.open && e.fd != -1) ::close(e.fd);
            e.fd = -1;
            e.open = false;

            removeFromList(id);
            if (openCount > 0) openCount--;
            return true;
        }
    };


    struct ConnectionContext {
        NiceAgent *agent;
        guint streamId;
        mutable lsquic_conn_t *connection;
        sockaddr_storage localAddr;
        sockaddr_storage remoteAddr;
        std::chrono::steady_clock::time_point startTime;
        std::chrono::steady_clock::time_point lastTime;
        uint64_t bytesMoved = 0;
        uint64_t lastBytesMoved = 0;
        int filesMoved = 0;
        double ewmaThroughput;
        bool started = false;
        bool complete = false;
        lsquic_stream_t *manifestStream = nullptr;

        indicators::ProgressBar *progressBar;
        uint64_t skippedBytes = 0;

        enum ConnectionType { DIRECT, RELAYED };

        ConnectionType connectionType = DIRECT;
    };
}
