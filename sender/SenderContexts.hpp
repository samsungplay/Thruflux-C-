#pragma once
#include <indicators/dynamic_progress.hpp>
#include <sys/mman.h>

#include "../common/Contexts.hpp"
#include "../common/Stream.hpp"

namespace sender {

    struct MmapEntry {
        int fd = -1;
        uint8_t *ptr = nullptr;
        size_t size = 0;
        std::list<uint32_t>::iterator lruIt;

        MmapEntry(const std::string &path, size_t sz) : size(sz) {
            fd = open(path.c_str(), O_RDONLY);
            if (fd != -1) {
                ptr = static_cast<uint8_t *>(mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0));
                if (ptr == MAP_FAILED) {
                    spdlog::error("Failed to map file: {}", path);
                    ptr = nullptr;
                    close(fd);
                    fd = -1;
                } else {
                    madvise(ptr, size, MADV_SEQUENTIAL);
                }
            }
        }

        ~MmapEntry() {
            if (ptr) munmap(ptr, size);
            if (fd != -1) close(fd);
        }
    };

    struct FileInfo {
        uint32_t id;
        uint64_t size;
        std::string path;
        std::string relativePath;
    };


    struct SenderPersistentContext {
        std::string joinCode;
        std::uint64_t totalExpectedBytes;
        int totalExpectedFilesCount;
        std::vector<FileInfo> files;
        std::vector<uint8_t> manifestBlob;
        std::unordered_map<uint32_t, std::shared_ptr<MmapEntry> > mmaps;
        std::list<uint32_t> lruList;
        const size_t MAX_MMAPS = 16;
        int globalCnt =0;
        std::vector<std::shared_ptr<common::UiRow>> uiRows;
        indicators::DynamicProgress<indicators::ProgressBar> progressBars;

        void buildManifest(const std::vector<std::string>& paths) {

            std::vector<std::string> filePaths;
            std::vector<std::string> absoluteFilePaths;
            std::uint64_t size = 0;
            int filesCount = 0;
            spdlog::info("Scanning files... {} file(s), {}", filesCount, common::Utils::sizeToReadableFormat(size));

            for (auto &path: paths) {
                std::filesystem::path root(path);
                if (!std::filesystem::exists(root)) {
                    continue;
                }
                if (std::filesystem::is_regular_file(root)) {
                    size += std::filesystem::file_size(root);
                    filesCount++;
                    auto u8 = root.filename().generic_u8string();
                    filePaths.push_back(std::string(u8.begin(), u8.end()));
                    u8 = root.generic_u8string();
                    absoluteFilePaths.push_back(std::string(u8.begin(), u8.end()));

                    spdlog::info("Scanning files... {} file(s), {}", filesCount,
                                 common::Utils::sizeToReadableFormat(size));
                } else {
                    for (const auto &entry: std::filesystem::recursive_directory_iterator(root)) {
                        if (entry.is_regular_file()) {
                            size += std::filesystem::file_size(entry.path());
                            filesCount++;

                            auto relativePath = entry.path().lexically_relative(root.parent_path());
                            auto u8 = relativePath.generic_u8string();
                            filePaths.push_back(std::string(u8.begin(), u8.end()));
                            u8 = entry.path().generic_u8string();
                            absoluteFilePaths.push_back(std::string(u8.begin(), u8.end()));
                            spdlog::info("Scanning files... {} file(s), {}", filesCount,
                                         common::Utils::sizeToReadableFormat(size));
                        }
                    }
                }
            }

            totalExpectedBytes = size;
            totalExpectedFilesCount = filesCount;

            files.clear();
            for (int i = 0; i < absoluteFilePaths.size(); i++) {
                std::uint64_t size = std::filesystem::file_size(absoluteFilePaths[i]);
                files.push_back({
                    static_cast<uint32_t>(i), size,
                    absoluteFilePaths[i], filePaths[i]
                });
            }
            const uint32_t count = files.size();
            manifestBlob.resize(4);
            memcpy(manifestBlob.data(), &count, 4);
            for (const auto &f: files) {
                size_t old = manifestBlob.size();
                uint16_t nl = f.relativePath.size();
                manifestBlob.resize(old + 14 + nl);
                uint8_t *p = manifestBlob.data() + old;
                memcpy(p, &f.id, 4);
                p += 4;
                memcpy(p, &f.size, 8);
                p += 8;
                memcpy(p, &nl, 2);
                p += 2;
                memcpy(p, f.relativePath.data(), nl);
            }
        }

        std::shared_ptr<MmapEntry> getMmap(uint32_t id) {
            auto it = mmaps.find(id);
            if (it != mmaps.end()) {
                lruList.splice(lruList.begin(), lruList, it->second->lruIt);
                return it->second;
            }
            if (mmaps.size() >= MAX_MMAPS) {
                uint32_t evict = lruList.back();
                mmaps.erase(evict);
                lruList.pop_back();
            }
            auto mmapEntry = std::make_shared<MmapEntry>(files[id].path, files[id].size);
            if (mmapEntry->ptr) {
                lruList.push_front(id);
                mmapEntry->lruIt = lruList.begin();
                mmaps[id] = mmapEntry;
                return mmapEntry;
            }
            return nullptr;
        }

        void addUiRow(const std::shared_ptr<common::UiRow> uiRow) {
            uiRows.push_back(std::move(uiRow));
            progressBars.push_back(uiRow->progressBar);
        }
    };

    inline SenderPersistentContext senderPersistentContext;


     //1 connection = 1 transfer = 1 receiver
    struct SenderConnectionContext : common::ConnectionContext {
        std::string receiverId;
        bool manifestStreamCreated = false;
        size_t currentFileIndex = 0;
        uint64_t currentFileOffset = 0;
        bool manifestCreated = false;
        size_t manifestSent = 0;

    };

    struct SenderStreamContext {
        SenderConnectionContext* connectionContext = nullptr;
        bool typeByteSent = false;
        bool isManifestStream = false;
        std::shared_ptr<MmapEntry> currentMmap;
        uint32_t fileId = 0;
        uint64_t offset = 0;
        uint32_t len = 0;
        uint32_t bytesSent = 0;
        uint8_t headerBuf[16];
        bool sendingHeader = false;
        uint8_t headerSent = 0;
        int id = 0;

        bool loadNextChunk() {

            while (true) {
                if (connectionContext->currentFileIndex >= senderPersistentContext.files.size()) {
                    return false;
                }

                auto &f = senderPersistentContext.files[connectionContext->currentFileIndex];
                const uint64_t off = connectionContext->currentFileOffset;

                connectionContext->currentFileOffset += 1024 * 1024;

                if (off >= f.size) {
                    connectionContext->currentFileIndex++;
                    connectionContext->filesMoved++;
                    connectionContext->currentFileOffset = 0;
                    continue;
                }

                fileId = f.id;
                offset = off;
                len = std::min(static_cast<uint64_t>(1024) * 1024, f.size - off);
                bytesSent = 0;

                currentMmap = senderPersistentContext.getMmap(fileId);

                if (!currentMmap) {
                    spdlog::error("Failed to mmap file ID {}: {}", fileId, f.path);
                    return false;
                }

                memcpy(headerBuf, &offset, 8);
                memcpy(headerBuf + 8, &len, 4);
                memcpy(headerBuf + 12, &fileId, 4);

                sendingHeader = true;
                headerSent = 0;
                return true;
            }
        }
    };
}
