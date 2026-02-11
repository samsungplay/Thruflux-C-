#pragma once
#include<nice.h>
#include <lsquic.h>
#include <openssl/ssl.h>

#include "SenderConfig.hpp"
#include <sys/mman.h>

#include "../common/Metrics.hpp"
#include "../common/Stream.hpp"
#include "SenderStateHolder.hpp"

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

    struct TransferState {
        struct FileInfo {
            uint32_t id;
            uint64_t size;
            std::string path;
            std::string relativePath;
        };

        bool receiverReady = false;
        std::vector<lsquic_stream_t *> pendingStreams;

        std::vector<FileInfo> files;
        std::vector<uint8_t> manifestBlob;

        size_t currentFileIndex = 0;
        uint64_t currentFileOffset = 0;
        bool manifestCreated = false;
        size_t manifestSent = 0;


        std::unordered_map<uint32_t, std::shared_ptr<MmapEntry> > mmaps;
        std::list<uint32_t> lruList;
        const size_t MAX_MMAPS = 16;

        void init(const std::vector<std::string> &paths, const std::vector<std::string> &relativePaths) {
            files.clear();
            for (int i = 0; i < paths.size(); i++) {
                std::uint64_t size = std::filesystem::file_size(paths[i]);
                files.push_back({
                    static_cast<uint32_t>(i), size,
                    paths[i], relativePaths[i]
                });
            }
            uint32_t count = files.size();
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
            auto mf = std::make_shared<MmapEntry>(files[id].path, files[id].size);
            if (mf->ptr) {
                lruList.push_front(id);
                mf->lruIt = lruList.begin();
                mmaps[id] = mf;
                return mf;
            }
            return nullptr;
        }
    };


    struct SenderQuicConnectionContext : common::QuicConnectionContext {
        std::string receiverId;
        std::shared_ptr<TransferState> state;
        std::shared_ptr<common::SenderTransferMetrics> senderMetrics;
        bool controlStreamCreated = false;
    };

    struct SenderContext {
        bool typeByteSent = false;
        bool isControlStream = false;
        std::shared_ptr<TransferState> state;
        std::shared_ptr<MmapEntry> currentMmap;
        std::shared_ptr<common::SenderTransferMetrics> senderMetrics;
        uint32_t fileId = 0;
        uint64_t offset = 0;
        uint32_t len = 0;
        uint32_t bytesSent = 0;
        uint8_t headerBuf[16];
        bool sendingHeader = false;
        uint8_t headerSent = 0;


        bool loadNextChunk() {
            while (true) {
                if (state->currentFileIndex >= state->files.size()) {
                    return false;
                }

                auto &f = state->files[state->currentFileIndex];
                uint64_t off = state->currentFileOffset;

                state->currentFileOffset += 1024 * 1024;

                if (off >= f.size) {
                    state->currentFileIndex++;
                    senderMetrics->filesSent++;
                    state->currentFileOffset = 0;
                    continue;
                }

                fileId = f.id;
                offset = off;
                len = std::min(static_cast<uint64_t>(1024) * 1024, f.size - off);
                bytesSent = 0;

                currentMmap = state->getMmap(fileId);

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


    class SenderStream : public common::Stream {
        //unlike receiver, sender's progress reporter should run persistently
        static void watchProgress() {
            g_timeout_add_full(G_PRIORITY_HIGH, 1000, [](gpointer data)-> gboolean {
                for (const auto &[receiverId, senderStats]: common::senderMetrics) {
                    if (!senderStats->started) continue;
                    const auto now = std::chrono::high_resolution_clock::now();
                    if (senderStats->lastTime.time_since_epoch().count() == 0) {
                        senderStats->lastTime = now;
                        return G_SOURCE_CONTINUE;
                    }

                    std::chrono::duration<double> elapsed = now - senderStats->startedTime;
                    std::chrono::duration<double> delta = now - senderStats->lastTime;

                    const double elapsedSeconds = elapsed.count();
                    const double deltaSeconds = delta.count();

                    const double instantThroughput =
                            (senderStats->bytesSent - senderStats->lastSnapshot) / deltaSeconds;
                    const double averageThroughput = senderStats->bytesSent / elapsedSeconds;
                    const double ewmaThroughput = senderStats->ewmaThroughput == 0
                                                      ? instantThroughput
                                                      : 0.2 * instantThroughput + 0.8 * senderStats->
                                                        ewmaThroughput;

                    const double percent = (static_cast<double>(senderStats->bytesSent) /
                                            SenderStateHolder::getTotalExpectedBytes()) * 100.0;


                    senderStats->lastTime = now;
                    senderStats->lastSnapshot = senderStats->bytesSent;

                    spdlog::info(
                        "Receiver : {} | Instant Throughput: {}/s | EWMA Throughput: {}/s |  Elapsed: {}s | Average Throughput: {}/s | Total Sent: {} | Progress: {}% | "
                        "Files {}/{}",
                        receiverId,
                        common::Utils::sizeToReadableFormat(instantThroughput),
                        common::Utils::sizeToReadableFormat(ewmaThroughput), elapsedSeconds,
                        common::Utils::sizeToReadableFormat(averageThroughput),
                        common::Utils::sizeToReadableFormat(senderStats->bytesSent), percent,
                        senderStats->filesSent,
                        SenderStateHolder::getTotalExpectedFilesCount()
                    );
                }
                return G_SOURCE_CONTINUE;
            }, nullptr, nullptr);
        }

        inline static lsquic_stream_if streamCallbacks = {
            .on_new_conn = [](void *streamIfCtx, lsquic_conn_t *connection) -> lsquic_conn_ctx * {
                auto *ctx = static_cast<SenderQuicConnectionContext *>(lsquic_conn_get_peer_ctx(connection, nullptr));
                ctx->connection = connection;
                lsquic_conn_make_stream(connection);
                if (ctx->componentId == 1) {
                    //open control stream
                    lsquic_conn_make_stream(connection);
                }
                spdlog::info("QUIC connection established on ICE Component {}", ctx->componentId);

                return reinterpret_cast<lsquic_conn_ctx *>(ctx);
            },
            .on_conn_closed = [](lsquic_conn_t *connection) {
                auto *ctx = reinterpret_cast<common::QuicConnectionContext *>(lsquic_conn_get_ctx(connection));
                spdlog::info("QUIC Connection closed for ICE component {}", ctx ? ctx->componentId : 0);;

                lsquic_conn_set_ctx(connection, nullptr);
                if (ctx) {
                    std::erase(connectionContexts_, ctx);
                    ctx->connection = nullptr;
                    delete ctx;
                }
            },

            .on_new_stream = [](void *stream_if_ctx, lsquic_stream_t *stream) -> lsquic_stream_ctx * {
                auto *connCtx = reinterpret_cast<SenderQuicConnectionContext *>(lsquic_conn_get_ctx(
                    lsquic_stream_conn(stream)));


                lsquic_stream_wantwrite(stream, 1);
                auto *ctx = new SenderContext();
                ctx->state = connCtx->state;
                ctx->senderMetrics = connCtx->senderMetrics;

                if (connCtx->componentId == 1 && !connCtx->controlStreamCreated) {
                    ctx->isControlStream = true;
                    connCtx->controlStreamCreated = true;
                } else {
                    ctx->isControlStream = false;

                    if (!ctx->loadNextChunk()) {
                        lsquic_stream_shutdown(stream, 1);
                    }
                }


                return reinterpret_cast<lsquic_stream_ctx *>(ctx);
            },
            .on_read = [](lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
                auto *ctx = reinterpret_cast<SenderContext *>(h);
                auto *connCtx = reinterpret_cast<SenderQuicConnectionContext *>(lsquic_conn_get_ctx(
                    lsquic_stream_conn(stream)));

                if (ctx->isControlStream) {
                    uint8_t buf[1];
                    if (lsquic_stream_read(stream, buf, 1) == 1) {
                        if (buf[0] == 0x06) {
                            //Receiver ACK received. Time to blast data!
                            if (!connCtx->senderMetrics->started) {
                                connCtx->senderMetrics->started = true;
                                connCtx->senderMetrics->startedTime = std::chrono::high_resolution_clock::now();
                            }
                            ctx->state->receiverReady = true;
                            for (auto *s: ctx->state->pendingStreams) {
                                lsquic_stream_wantwrite(s, 1);
                            }
                            ctx->state->pendingStreams.clear();
                        }
                    }
                }
            },
            .on_write = [](lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
                auto *ctx = reinterpret_cast<SenderContext *>(h);


                if (!ctx->typeByteSent) {
                    uint8_t tag = ctx->isControlStream ? 0x00 : 0x01;
                    ssize_t nw = lsquic_stream_write(stream, &tag, 1);
                    if (nw > 0) {
                        ctx->typeByteSent = true;
                    } else {
                        return;
                    }
                    return;
                }

                auto *connCtx = reinterpret_cast<SenderQuicConnectionContext *>(lsquic_conn_get_ctx(
                    lsquic_stream_conn(stream)));

                if (ctx->isControlStream) {
                    size_t total = ctx->state->manifestBlob.size();
                    size_t sent = ctx->state->manifestSent;
                    if (sent < total) {
                        ssize_t nw = lsquic_stream_write(stream, ctx->state->manifestBlob.data() + sent, total - sent);
                        if (nw > 0) ctx->state->manifestSent += nw;
                    }

                    if (ctx->state->manifestSent == total) {
                        lsquic_stream_flush(stream);
                        lsquic_stream_shutdown(stream, 1);
                        lsquic_stream_wantread(stream, 1);
                    }
                    return;
                }

                //queue up streams until control stream handshake completes
                if (!ctx->state->receiverReady) {
                    ctx->state->pendingStreams.push_back(stream);
                    lsquic_stream_wantwrite(stream, 0);
                    return;
                }


                static constexpr int BATCH_SIZE = 256 * 1024;
                int bytesWrittenThisTick = 0;

                while (true) {
                    if (bytesWrittenThisTick >= BATCH_SIZE) {
                        lsquic_stream_wantwrite(stream, 1);
                        return;
                    }

                    if (ctx->sendingHeader) {
                        size_t remaining = 16 - ctx->headerSent;
                        ssize_t nw = lsquic_stream_write(stream, ctx->headerBuf + ctx->headerSent, remaining);
                        if (nw <= 0) {
                            return;
                        }
                        ctx->headerSent += nw;
                        bytesWrittenThisTick += nw;

                        if (ctx->headerSent == 16) {
                            ctx->sendingHeader = false;
                        } else {
                            return;
                        }
                    } else {
                        if (!ctx->currentMmap) {
                            spdlog::error("Unexpected error: currentMmap missing for QUIC connection {}",
                                          connCtx->componentId);
                            lsquic_stream_close(stream);
                            return;
                        }
                        size_t remaining = ctx->len - ctx->bytesSent;
                        const uint8_t *ptr = ctx->currentMmap->ptr + ctx->offset +
                                             ctx->bytesSent;
                        ssize_t nw = lsquic_stream_write(stream, ptr, remaining);
                        if (nw <= 0) {
                            return;
                        }

                        ctx->bytesSent += nw;
                        connCtx->senderMetrics->bytesSent += nw;
                        bytesWrittenThisTick += nw;


                        if (ctx->bytesSent >= ctx->len) {
                            ctx->currentMmap = nullptr;
                            if (!ctx->loadNextChunk()) {
                                lsquic_stream_shutdown(stream, 1);
                                lsquic_stream_wantwrite(stream, 0);
                                return;
                            }
                        }
                    }
                }
            },
            .on_close = [](lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
                const auto *ctx = reinterpret_cast<SenderContext *>(h);

                delete ctx;
            },
            .on_hsk_done = [](lsquic_conn_t *connection, enum lsquic_hsk_status status) {
                auto *ctx = reinterpret_cast<common::QuicConnectionContext *>(lsquic_conn_get_ctx(connection));
                if (status == LSQ_HSK_OK || status == LSQ_HSK_RESUMED_OK) {
                    spdlog::info("QUIC Handshake Successful on ICE Component {}", ctx->componentId);
                } else {
                    spdlog::error("QUIC Handshake Failed on ICE Component {}", ctx->componentId);
                }
            },
            .on_conncloseframe_received = [](lsquic_conn_t *c, int app_error, uint64_t error_code, const char *reason,
                                             int reason_len) {
                spdlog::error("PEER REJECTED CONNECTION! Code: {}, Reason: {:.{}s}",
                              error_code, reason, reason_len);
            },


        };

    public:
        static void initialize() {
            // common::init_lsquic_logging();
            sslCtx_ = createSslCtx();
            lsquic_global_init(LSQUIC_GLOBAL_CLIENT);
            lsquic_engine_settings settings;
            lsquic_engine_init_settings(&settings, 0);
            settings.es_versions = (1 << LSQVER_I001);
            settings.es_cc_algo = 2;
            settings.es_init_max_data = SenderConfig::quicConnWindowBytes;
            settings.es_init_max_streams_uni = SenderConfig::quicMaxIncomingStreams;
            settings.es_init_max_streams_bidi = SenderConfig::quicMaxIncomingStreams;
            settings.es_idle_conn_to = 15000000;
            settings.es_init_max_stream_data_uni = SenderConfig::quicStreamWindowBytes;
            settings.es_init_max_stream_data_bidi_local = SenderConfig::quicStreamWindowBytes;
            settings.es_init_max_stream_data_bidi_remote = SenderConfig::quicStreamWindowBytes;
            settings.es_handshake_to = 30000000;
            settings.es_allow_migration = 0;


            char err_buf[256];
            if (0 != lsquic_engine_check_settings(&settings, 0, err_buf, sizeof(err_buf))) {
                spdlog::error("Invalid lsquic engine settings: {}", err_buf);
                return;
            }
            lsquic_engine_api api = {};
            api.ea_settings = &settings;
            api.ea_stream_if = &streamCallbacks;
            api.ea_packets_out = sendPackets;
            api.ea_get_ssl_ctx = getSslCtx;
            engine_ = lsquic_engine_new(0, &api);

            spdlog::info("LSQUIC Engine Successfully Initialized. {}", engine_ == nullptr);
        }


        //assume this method will be invoked from gmainloop thread
        static void startTransfer(NiceAgent *agent, const guint streamId, const int n,
                                  std::string receiverId) {
            const auto transferStats = std::make_shared<common::SenderTransferMetrics>(receiverId);
            common::senderMetrics[receiverId] = transferStats;
            const auto state = std::make_shared<TransferState>();
            state->init(SenderStateHolder::getAbsolutePaths(), SenderStateHolder::getRelativePaths());

            for (int i = 1; i <= n; i++) {
                setAndVerifySocketBuffers(agent, streamId, i, SenderConfig::udpBufferBytes);

                NiceCandidate *local = nullptr, *remote = nullptr;
                if (!nice_agent_get_selected_pair(agent, streamId, i, &local, &remote)) {
                    spdlog::error("ICE component {} is not ready for QUIC connection", i);
                    continue;
                }


                auto *ctx = new SenderQuicConnectionContext();
                ctx->agent = agent;
                ctx->streamId = streamId;
                ctx->componentId = i;
                ctx->receiverId = receiverId;
                ctx->state = state;
                ctx->senderMetrics = transferStats;

                nice_address_copy_to_sockaddr(&local->base_addr, reinterpret_cast<sockaddr *>(&ctx->localAddr));
                nice_address_copy_to_sockaddr(&remote->addr, reinterpret_cast<sockaddr *>(&ctx->remoteAddr));


                connectionContexts_.push_back(ctx);


                nice_agent_attach_recv(agent, streamId, i, common::ThreadManager::getContext(),
                                       [](NiceAgent *agent, guint stream_id, guint component_id,
                                          guint len, gchar *buf, gpointer user_data) {
                                           auto *c = static_cast<common::QuicConnectionContext *>(user_data);

                                           lsquic_engine_packet_in(engine_, (unsigned char *) buf, len,
                                                                   (sockaddr *) &c->localAddr,
                                                                   (sockaddr *) &c->remoteAddr,
                                                                   c, 0);

                                           static int packetsSinceProcess = 0;
                                           if (++packetsSinceProcess >= 64) {
                                               packetsSinceProcess = 0;
                                               process();
                                           }
                                       },
                                       ctx
                );


                lsquic_engine_connect(
                    engine_,
                    LSQVER_I001,
                    reinterpret_cast<const sockaddr *>(&ctx->localAddr),
                    reinterpret_cast<const sockaddr *>(&ctx->remoteAddr),
                    ctx,
                    nullptr,
                    "thruflux.local", 0, nullptr, 0, nullptr, 0
                );
            }

            g_timeout_add(0, engineTick, nullptr);
            watchProgress();
        }


        static void disposeReceiverConnection(std::string_view receiverId) {
            std::vector<lsquic_conn_t *> toClose;
            common::senderMetrics.erase(std::string(receiverId));

            for (const auto *ctx: connectionContexts_) {
                if (((SenderQuicConnectionContext *) ctx)->receiverId == receiverId) {
                    toClose.push_back(ctx->connection);
                }
            }

            for (auto *conn: toClose) {
                lsquic_conn_close(conn);
            }
            process();
        }
    };
}
