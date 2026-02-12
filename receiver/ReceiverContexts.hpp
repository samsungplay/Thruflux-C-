#pragma once
#include "../common/Contexts.hpp"

namespace receiver {


    struct ReceiverConnectionContext : common::ConnectionContext {
        common::FileHandleCache cache;
        std::vector<uint8_t> manifestBuf;
        bool manifestParsed = false;
        uint64_t totalExpectedBytes = 0;
        int totalExpectedFilesCount = 0;
        std::vector<uint64_t> fileSizes;
        std::vector<uint64_t> perFileBytesWritten;
        bool pendingManifestAck = false;
        bool pendingCompleteAck = false;


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
            spdlog::info("Manifest parsed. {} files. totalExpectedBytes: {}", count, totalExpectedBytes);
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
        alignas(4096) uint8_t writeBuffer[256 * 1024];

    };
}