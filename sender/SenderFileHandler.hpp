#pragma once
#include "../common/Payloads.hpp"
#include <filesystem>
#include <spdlog/spdlog.h>

#include "../common/Utils.hpp"

namespace sender {
    class SenderFileHandler {
    public:
        static common::CreateTransferSessionPayload generateCreateTransferSessionPayload(
            const std::vector<std::string> &paths, const int maxReceivers) {
            std::vector<std::string> filePaths;
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
                            spdlog::info("Scanning files... {} file(s), {}", filesCount,
                                         common::Utils::sizeToReadableFormat(size));
                        }
                    }
                }
            }

            return common::CreateTransferSessionPayload{
                .maxReceivers = maxReceivers,
                .paths = std::move(filePaths),
                .totalSize = size
            };
        }
    };
}
