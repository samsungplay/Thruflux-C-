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

#include <system_error>
#include <filesystem>
#include <cstdint>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/types.h>
#include <sys/stat.h>
// posix_fallocate is available on Linux; on macOS it's available but sometimes behaves differently.
// We'll try it when available, otherwise fall back.
#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>   // posix_fallocate
#endif
#endif
#endif


static bool thruflux_preallocate_file_best_effort(const std::filesystem::path &path, uint64_t sizeBytes) {
    if (sizeBytes == 0) return true;

#ifdef _WIN32
    // Best-effort: set file length to sizeBytes (doesn't guarantee physical allocation like posix_fallocate).
    // Still helps reduce metadata churn during streaming.
    HANDLE h = CreateFileW(
        path.wstring().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(sizeBytes);

    bool ok = true;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) ok = false;
    if (ok && !SetEndOfFile(h)) ok = false;

    CloseHandle(h);
    return ok;

#else
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return false;

    bool ok = false;

#if defined(__linux__)
    // Standard Linux approach
    ok = (::posix_fallocate(fd, 0, static_cast<off_t>(sizeBytes)) == 0);
#elif defined(__APPLE__)
    // macOS specific pre-allocation
    fstore_t store = {F_ALLOCATECONTIG | F_ALLOCATEALL, F_PEOFPOSMODE, 0, static_cast<off_t>(sizeBytes), 0};
    // Try to allocate contiguous space first
    if (::fcntl(fd, F_PREALLOCATE, &store) == -1) {
        // Fallback to non-contiguous allocation if contiguous fails
        store.fst_flags = F_ALLOCATEALL;
        if (::fcntl(fd, F_PREALLOCATE, &store) == -1) {
            ok = false;
        } else {
            ok = true;
        }
    } else {
        ok = true;
    }
#endif

    // Final fallback: ftruncate to ensure the file size is set even if preallocation fails
    if (!ok) {
        ok = (::ftruncate(fd, static_cast<off_t>(sizeBytes)) == 0);
    }

    ::close(fd);
    return ok;
#endif
}


namespace receiver {
    static constexpr uint64_t PREALLOC_THRESHOLD = 64ull * 1024 * 1024; // 64 MiB
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

                if (!ReceiverConfig::overwrite) {
                    std::error_code ec;
                    uint64_t existing = 0;
                    if (std::filesystem::exists(full, ec) && !ec) {
                        existing = static_cast<uint64_t>(std::filesystem::file_size(full, ec));
                        if (ec) existing = 0;
                    }

                    if (sz >= PREALLOC_THRESHOLD && existing != sz) {
                        (void)thruflux_preallocate_file_best_effort(full, sz);
                    }
                } else {
                    if (sz >= PREALLOC_THRESHOLD) {
                        (void)thruflux_preallocate_file_best_effort(full, sz);
                    }
                }

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
        uint8_t writeBuffer[256 * 1024];

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
