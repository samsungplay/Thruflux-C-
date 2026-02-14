#pragma once
#include "ReceiverConfig.hpp"
#include "../common/Contexts.hpp"

namespace receiver {
    struct ReceiverConnectionContext : common::ConnectionContext {
        common::FileHandleCache cache;
        std::vector<uint8_t> manifestBuf;
        bool manifestParsed = false;
        bool manifestReceiveStartMessagePrinted = false;
        uint64_t totalExpectedBytes = 0;
        int totalExpectedFilesCount = 0;
        std::vector<uint64_t> fileSizes;
        std::vector<uint64_t> perFileBytesWritten;
        bool pendingManifestAck = false;
        bool pendingCompleteAck = false;
        std::unique_ptr<indicators::ProgressBar> progressBar;
        std::vector<uint64_t> fileChunkBase;
        uint64_t totalChunks = 0;
        std::vector<uint8_t> resumeBitmap;
        std::string resumeBitmapPath;
        size_t manifestAckSent = 0;
        std::chrono::high_resolution_clock::time_point lastResumeBitmapFlush{};
        bool isResumeBitmapDirty = false;

        void createProgressBar(std::string prefix) {
            progressBar = common::Utils::createProgressBarUniquePtr(prefix);
        };


        void parseManifest() {
            uint8_t *p = manifestBuf.data();
            uint32_t count;
            memcpy(&count, p, 4);
            p += 4;
            fileSizes.resize(count);
            perFileBytesWritten.resize(count, 0);

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


            fileChunkBase.resize(fileSizes.size());
            totalChunks = 0;
            for (int id = 0; id < fileSizes.size(); id++) {
                fileChunkBase[id] = totalChunks;
                totalChunks += common::Utils::ceilDiv(fileSizes[id], common::CHUNK_SIZE);
            }

            const size_t bitmapBytes = (totalChunks + 7) / 8;
            resumeBitmap.assign(bitmapBytes, 0);
            const auto manifestHash = common::Utils::fnv1a64(manifestBuf.data(), manifestBuf.size());
            const auto bitmapPath = std::filesystem::path(ReceiverConfig::out) / (
                                        ".thruflux_resume_" + std::to_string(manifestHash) + ".bm");
            resumeBitmapPath = bitmapPath.string();

            if (ReceiverConfig::overwrite) {
                deleteResumeBitmap();
            } else {
                bool loaded = false;
                if (std::filesystem::exists(bitmapPath)) {
                    std::error_code ec;
                    const auto sz = std::filesystem::file_size(bitmapPath, ec);
                    if (!ec && sz == bitmapBytes) {
                        std::ifstream in(bitmapPath, std::ios::binary);
                        if (in) {
                            in.read(reinterpret_cast<char *>(resumeBitmap.data()),
                                    static_cast<std::streamsize>(bitmapBytes));
                            if (in.gcount() == static_cast<std::streamsize>(bitmapBytes)) {
                                loaded = true;
                            } else {
                                resumeBitmap.assign(bitmapBytes, 0);
                            }
                        }
                    }
                }

                if (loaded) {
                    uint64_t resumedChunks = 0;
                    for (uint8_t b: resumeBitmap) {
                        resumedChunks += __builtin_popcount(b);
                    }
                    const double percent =
                            totalChunks == 0 ? 0.0 : (static_cast<double>(resumedChunks) / totalChunks) * 100.0;

                    const auto resumedBytes = std::min(resumedChunks * common::CHUNK_SIZE, totalExpectedBytes);

                    spdlog::info(
                        "Auto-resuming: {:.2f}% already present ({} / {}). Pass flag --overwrite to disable.",
                        percent,
                        common::Utils::sizeToReadableFormat(resumedBytes),
                        common::Utils::sizeToReadableFormat(totalExpectedBytes)
                    );
                }
            }


            spdlog::info("Manifest unsealed: {} file(s) , Total size: {}", count,
                         common::Utils::sizeToReadableFormat(totalExpectedBytes));
        }

        void maybeSaveBitmap(bool force = false) {
            if (!isResumeBitmapDirty) return;

            const auto now = std::chrono::high_resolution_clock::now();
            const bool timeOk =
                    (lastResumeBitmapFlush.time_since_epoch().count() == 0) ||
                    (std::chrono::duration<double>(now - lastResumeBitmapFlush).count() >= 2.0);

            if (!force && !timeOk) return;

            const std::string tmpPath = resumeBitmapPath + ".tmp";


            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (!out) return;
            out.write(reinterpret_cast<const char *>(resumeBitmap.data()),
                      static_cast<std::streamsize>(resumeBitmap.size()));
            out.flush();


            std::error_code ec;
            std::filesystem::rename(tmpPath, resumeBitmapPath, ec);
            if (ec) {
                std::filesystem::remove(resumeBitmapPath, ec);
                ec.clear();
                std::filesystem::rename(tmpPath, resumeBitmapPath, ec);
            }

            isResumeBitmapDirty = false;
            lastResumeBitmapFlush = now;
        }


        void deleteResumeBitmap() const {
            if (resumeBitmapPath.empty()) return;
            std::error_code ec;
            if (std::filesystem::exists(resumeBitmapPath)) {
                std::filesystem::remove(resumeBitmapPath, ec);
                if (ec) {
                    spdlog::warn("Failed to delete resume bitmap: {} ({})",
                                 resumeBitmapPath, ec.message());
                }
            }
            const std::string tmpPath = resumeBitmapPath + ".tmp";
            if (std::filesystem::exists(tmpPath)) {
                std::filesystem::remove(tmpPath, ec);
                if (ec) {
                    spdlog::warn("Failed to delete resume bitmap: {} ({})",
                                 tmpPath, ec.message());
                }
            }
        }
    };

    struct ReceiverStreamContext {
        enum StreamType { UNKNOWN, MANIFEST, DATA } type = UNKNOWN;

        bool readingHeader = true;
        uint8_t headerBuf[16];
        uint8_t headerBytesRead = 0;
        uint64_t chunkOffset = 0;
        uint32_t chunkLength = 0;
        uint32_t bodyBytesRead = 0;
        uint32_t fileId = 0;
        uint8_t writeBuffer[1024 * 1024];
    };
}
