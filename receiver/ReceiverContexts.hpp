#pragma once
#include <blake3.h>

#include "ReceiverConfig.hpp"
#include "../common/Contexts.hpp"
#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(__popcnt)
static inline int popcount32(unsigned int x) {
    return (int) __popcnt(x);
}
#else
static int popcount32(unsigned int x) {
    return __builtin_popcount(x);
}
#endif


namespace receiver {
    static constexpr size_t IO_BUF = 4 * 1024 * 1024;
    static constexpr size_t POOL_COUNT = 128;

    struct Buf {
        std::unique_ptr<uint8_t[]> p;
        size_t cap = 0;
    };

    struct WriteJob {
        uint32_t fileId;
        uint64_t off;
        uint8_t *data;
        uint32_t len;
        // return buffer to pool:
        Buf *bufOwner;
    };

    class BoundedPipeline {
    public:
        explicit BoundedPipeline(size_t bufCount, size_t bufSize)
            : bufSize_(bufSize) {
            bufs_.reserve(bufCount);
            for (size_t i = 0; i < bufCount; ++i) {
                auto b = std::make_unique<Buf>();
                b->p = std::unique_ptr<uint8_t[]>(new uint8_t[bufSize_]);
                b->cap = bufSize_;
                free_.push_back(b.get());
                bufs_.push_back(std::move(b));
            }
        }
        void notifyAll() { cv_.notify_all(); }

        // producer (QUIC thread) acquires a buffer (non-blocking)
        Buf *tryAcquireBuf() {
            std::lock_guard<std::mutex> lk(mu_);
            if (free_.empty()) return nullptr;
            Buf *b = free_.back();
            free_.pop_back();
            return b;
        }

        void releaseBuf(Buf *b) {
            std::lock_guard<std::mutex> lk(mu_);
            free_.push_back(b);
            cv_.notify_one();
        }

        // enqueue a write job (non-blocking if we ensure it never exceeds cap)
        bool tryEnqueue(const WriteJob &j) {
            std::lock_guard<std::mutex> lk(mu_);
            if (q_.size() >= cap_) return false;
            q_.push_back(j);
            cv_.notify_one();
            return true;
        }

        // consumer (disk thread) waits for a job
        bool popWait(WriteJob &out, bool &stopFlag) {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stopFlag || !q_.empty(); });
            if (stopFlag && q_.empty()) return false;
            out = q_.front();
            q_.pop_front();
            return true;
        }

        void setCapacity(size_t cap) { cap_ = cap; }

        size_t queued() const {
            std::lock_guard<std::mutex> lk(mu_);
            return q_.size();
        }

        size_t freeCount() const {
            std::lock_guard<std::mutex> lk(mu_);
            return free_.size();
        }

    private:
        size_t bufSize_;
        mutable std::mutex mu_;
        std::condition_variable cv_;

        std::vector<std::unique_ptr<Buf> > bufs_;
        std::vector<Buf *> free_;
        std::deque<WriteJob> q_;
        size_t cap_ = 0;
    };


    struct ReceiverConnectionContext : common::ConnectionContext {
        std::chrono::steady_clock::time_point lastResumeFlush{};
        bool resumeDirty = false;
        common::FileHandleCache cache;
        std::vector<uint8_t> manifestBuf;
        bool manifestParsed = false;
        uint64_t totalExpectedBytes = 0;
        int totalExpectedFilesCount = 0;
        std::vector<uint64_t> fileSizes;
        bool pendingManifestAck = false;
        bool pendingCompleteAck = false;
        std::unique_ptr<indicators::ProgressBar> progressBar;
        uint32_t resumeFileId = 0;
        uint64_t resumeOffset = 0;
        std::string resumeStatePath;
        int manifestAckSent = 0;

        std::unique_ptr<receiver::BoundedPipeline> pipe;
        std::thread diskThread;
        std::atomic<bool> stopDisk{false};

        lsquic_stream_t *dataStream = nullptr;
        std::atomic<bool> readPaused{false};

        uint32_t curWriteFileId = 0;
        uint64_t curWriteOff = 0;

        indicators::ProgressBar manifestProgressBar{
            indicators::option::BarWidth{0},
            indicators::option::Start{""},
            indicators::option::End{""},
            indicators::option::ShowPercentage{false},
            indicators::option::PrefixText{"Fetching catalogue.. "},
            indicators::option::PostfixText{" received 0B"},
            indicators::option::ForegroundColor{indicators::Color::white}
        };
        std::chrono::steady_clock::time_point lastManifestProgressPrint{};

        void createProgressBar(std::string prefix) {
            progressBar = common::Utils::createProgressBarUniquePtr(prefix);
        };


        void parseManifest() {
            uint8_t *p = manifestBuf.data();
            uint32_t count;
            memcpy(&count, p, 4);
            cache.reset(count);
            p += 4;
            fileSizes.resize(count);

            for (int i = 0; i < count; i++) {
                uint32_t id;
                memcpy(&id, p, 4);
                p += 4;
                uint64_t sz;
                memcpy(&sz, p, 8);
                fileSizes[id] = sz;
                totalExpectedBytes += sz;
                totalExpectedFilesCount++;
                p += 8;
                uint16_t l;
                memcpy(&l, p, 2);
                p += 2;
                std::string relativePath(reinterpret_cast<char *>(p), l);
                p += l;

                std::filesystem::path full = std::filesystem::path(ReceiverConfig::out) / relativePath;
                std::filesystem::create_directories(full.parent_path());
                cache.registerPath(id, full.string());
            }


            const auto manifestHash = common::Utils::fnv1a64(manifestBuf.data(), manifestBuf.size());
            auto statePath = std::filesystem::path(ReceiverConfig::out) /
                             (".thruflux_resume_" + std::to_string(manifestHash) + ".state");
            resumeStatePath = statePath.string();

            if (ReceiverConfig::overwrite) {
                std::error_code ec;
                std::filesystem::remove(statePath, ec);
                resumeFileId = 0;
                resumeOffset = 0;
            } else {
                if (std::filesystem::exists(statePath)) {
                    resumeFileId = 0;
                    resumeOffset = 0;
                    {
                        std::ifstream in(statePath, std::ios::binary);
                        if (in) {
                            uint32_t fid = 0;
                            uint64_t off = 0;
                            in.read(reinterpret_cast<char *>(&fid), sizeof(fid));
                            in.read(reinterpret_cast<char *>(&off), sizeof(off));

                            const bool ok =
                                    in.good() &&
                                    in.gcount() == static_cast<std::streamsize>(sizeof(off));

                            if (ok && fid < fileSizes.size()) {
                                resumeFileId = fid;
                                resumeOffset = std::min<uint64_t>(off, fileSizes[fid]);
                            }
                        }
                    }

                    while (resumeFileId < fileSizes.size() && resumeOffset >= fileSizes[resumeFileId]) {
                        resumeOffset = 0;
                        ++resumeFileId;
                    }
                    if (resumeFileId >= fileSizes.size()) {
                        resumeFileId = static_cast<uint32_t>(fileSizes.size());
                        resumeOffset = 0;
                    }

                    uint64_t resumedBytes = 0;
                    for (uint32_t id = 0; id < resumeFileId && id < fileSizes.size(); ++id) {
                        resumedBytes += fileSizes[id];
                    }
                    resumedBytes += resumeOffset;

                    const int resumedFiles = resumeFileId;

                    bytesMoved = resumedBytes;
                    lastBytesMoved = resumedBytes;
                    skippedBytes = resumedBytes;
                    filesMoved = resumedFiles;
                }
            }

            curWriteFileId = resumeFileId;
            curWriteOff = resumeOffset;
            pipe = std::make_unique<receiver::BoundedPipeline>(POOL_COUNT, IO_BUF);
            pipe->setCapacity(POOL_COUNT);

            stopDisk = false;
            diskThread = std::thread([this] {
                receiver::WriteJob job{};
                while (true) {
                    bool stop = stopDisk.load(std::memory_order_relaxed);
                    if (!pipe->popWait(job, stop)) break;

                    llfio::file_handle *fh = cache.acquire(job.fileId, true);
                    if (!fh) {
                        stopDisk = true;
                        pipe->releaseBuf(job.bufOwner);
                        break;
                    }

                    llfio::byte_io_handle::const_buffer_type reqBuf({
                        reinterpret_cast<const llfio::byte *>(job.data),
                        static_cast<size_t>(job.len)
                    });

                    llfio::file_handle::io_request<llfio::file_handle::const_buffers_type> req(
                        llfio::file_handle::const_buffers_type{&reqBuf, 1},
                        job.off
                    );

                    auto result = fh->write(req);
                    cache.release(job.fileId);

                    if (!result || result.bytes_transferred() < job.len) {
                        stopDisk = true;
                        pipe->releaseBuf(job.bufOwner);
                        break;
                    }

                    bytesMoved += job.len;
                    resumeFileId = job.fileId;
                    resumeOffset = job.off + job.len;
                    resumeDirty = true;

                    pipe->releaseBuf(job.bufOwner);

                    while (resumeFileId < fileSizes.size() && resumeOffset >= fileSizes[resumeFileId]) {
                        filesMoved++;
                        resumeOffset = 0;
                        resumeFileId++;
                    }

                    if (!complete && filesMoved >= totalExpectedFilesCount) {
                        complete = true;
                        pendingCompleteAck = true;
                    }
                }
            });


            spdlog::info("Manifest unsealed: {} file(s) , Total size: {}", count,
                         common::Utils::sizeToReadableFormat(totalExpectedBytes));
        }

        void maybeSaveResumeState(bool force = false) {
            if (!resumeDirty) return;

            auto now = std::chrono::steady_clock::now();
            bool timeOk = (lastResumeFlush.time_since_epoch().count() == 0) ||
                          (std::chrono::duration<double>(now - lastResumeFlush).count() >= 1.0);
            if (!force && !timeOk) return;

            const std::string tmp = resumeStatePath + ".tmp";
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return;

            out.write(reinterpret_cast<const char *>(&resumeFileId), sizeof(resumeFileId));
            out.write(reinterpret_cast<const char *>(&resumeOffset), sizeof(resumeOffset));
            out.flush();

            std::error_code ec;
            std::filesystem::rename(tmp, resumeStatePath, ec);
            if (ec) {
                std::filesystem::remove(resumeStatePath, ec);
                ec.clear();
                std::filesystem::rename(tmp, resumeStatePath, ec);
            }

            resumeDirty = false;
            lastResumeFlush = now;
        }
    };

    struct ReceiverStreamContext {
        enum StreamType { UNKNOWN, MANIFEST, DATA } type = UNKNOWN;

        uint32_t curFileId = 0;
        uint64_t curOff = 0;
        uint64_t curSize = 0;
        uint32_t pinnedFileId = UINT32_MAX;
        llfio::file_handle *pinnedHandle = nullptr;
        uint8_t writeBuffer[common::CHUNK_SIZE];

        bool openFile(ReceiverConnectionContext *connCtx, uint32_t fileId) {
            if (fileId >= connCtx->fileSizes.size()) return false;
            curFileId = fileId;
            curSize = connCtx->fileSizes[fileId];

            if (pinnedFileId != fileId) {
                if (pinnedFileId != UINT32_MAX) connCtx->cache.release(pinnedFileId);
                pinnedFileId = fileId;
                pinnedHandle = connCtx->cache.acquire(fileId, true);
                if (!pinnedHandle) return false;
            }
            return true;
        }
    };
}
