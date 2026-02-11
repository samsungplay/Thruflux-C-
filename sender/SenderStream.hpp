#pragma once
#include<nice.h>
#include <lsquic.h>
#include <openssl/ssl.h>

#include "SenderConfig.hpp"
#include <sys/mman.h>

#include "../common/Metrics.hpp"
#include "../common/Stream.hpp"

namespace sender {
    struct FileContext {
        int fd = -1;
        uint8_t *mappedRegion = nullptr;
        size_t fileSize = 0;
        std::atomic<uint64_t> nextOffset{0};

        static constexpr size_t CHUNK_SIZE = 1024 * 1024;

        explicit FileContext(const std::string &path) {
            fd = open(path.c_str(), O_RDONLY);
            if (fd == -1) {
                spdlog::error("Failed to open file {}", path.c_str());
                return;
            }

            struct stat statResult;
            if (fstat(fd, &statResult) == -1) {
                close(fd);
                fd = -1;
                return;
            }
            fileSize = statResult.st_size;

            mappedRegion = (uint8_t *) mmap(nullptr, fileSize, PROT_READ, MAP_SHARED, fd, 0);
            if (mappedRegion == MAP_FAILED) {
                spdlog::error("Failed to map file {}", path.c_str());
                close(fd);
                fd = -1;
                mappedRegion = nullptr;
                return;
            }

            madvise(mappedRegion, fileSize, MADV_SEQUENTIAL);
            madvise(mappedRegion, fileSize, MADV_WILLNEED);
        }

        ~FileContext() {
            if (mappedRegion) {
                munmap(mappedRegion, fileSize);
                mappedRegion = nullptr;
            }
            if (fd != -1) {
                close(fd);
                fd = -1;
            }
        }
    };

    struct SenderQuicConnectionContext : common::QuicConnectionContext {
        std::string receiverId;
        std::shared_ptr<FileContext> file;
        std::shared_ptr<common::SenderTransferMetrics> senderMetrics;
    };

    struct SenderContext {
        std::shared_ptr<FileContext> file;
        uint64_t chunkStartOffset = 0;
        uint32_t chunkLength = 0;
        uint32_t chunkBytesSent = 0;

        uint8_t headerBuf[16];

        bool sendingHeader = false;

        uint8_t headerBytesSent = 0;

        bool getNextChunk() {
            if (!file || !file->mappedRegion) {
                return false;
            }
            const uint64_t offset = file->nextOffset.fetch_add(FileContext::CHUNK_SIZE);
            //sent them all!
            if (offset >= file->fileSize) return false;

            chunkStartOffset = offset;

            const uint64_t remaining = file->fileSize - offset;
            chunkLength = std::min(static_cast<uint64_t>(FileContext::CHUNK_SIZE), remaining);

            memcpy(headerBuf, &chunkStartOffset, sizeof(uint64_t));
            memcpy(headerBuf + sizeof(uint64_t), &chunkLength, sizeof(uint32_t));
            memset(headerBuf + 12, 0, 4);

            sendingHeader = true;
            headerBytesSent = 0;
            chunkBytesSent = 0;
            return true;
        }
    };


    class SenderStream : public common::Stream {
        inline static lsquic_stream_if streamCallbacks = {
            .on_new_conn = [](void *streamIfCtx, lsquic_conn_t *connection) -> lsquic_conn_ctx * {
                auto *ctx = static_cast<common::QuicConnectionContext *>(lsquic_conn_get_peer_ctx(connection, nullptr));
                ctx->connection = connection;
                lsquic_conn_make_stream(connection);
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
                ctx->file = connCtx->file;


                if (!ctx->getNextChunk()) {
                    lsquic_stream_shutdown(stream, 1);
                }

                return reinterpret_cast<lsquic_stream_ctx *>(ctx);
            },
            .on_read = [](lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
                char buf[1];
                if (lsquic_stream_read(stream, buf, 1) == 0) {
                    lsquic_stream_close(stream);
                }
            },
            .on_write = [](lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
                auto *ctx = reinterpret_cast<SenderContext *>(h);
                auto *connCtx = reinterpret_cast<SenderQuicConnectionContext *>(lsquic_conn_get_ctx(
                    lsquic_stream_conn(stream)));
                if (!ctx->file || !ctx->file->mappedRegion) {
                    lsquic_stream_close(stream);
                    return;
                }
                while (true) {
                    if (ctx->sendingHeader) {
                        size_t remaining = 16 - ctx->headerBytesSent;
                        ssize_t nw = lsquic_stream_write(stream, ctx->headerBuf + ctx->headerBytesSent, remaining);
                        if (nw <= 0) {
                            return;
                        }
                        ctx->headerBytesSent += nw;

                        if (ctx->headerBytesSent == 16) {
                            ctx->sendingHeader = false;
                        } else {
                            return;
                        }
                    } else {
                        size_t remaining = ctx->chunkLength - ctx->chunkBytesSent;
                        const uint8_t *ptr = ctx->file->mappedRegion + ctx->chunkStartOffset +
                                             ctx->chunkBytesSent;
                        ssize_t nw = lsquic_stream_write(stream, ptr, remaining);
                        if (nw <= 0) {
                            return;
                        }

                        ctx->chunkBytesSent += nw;
                        connCtx->senderMetrics->bytesSent += nw;


                        if (ctx->chunkBytesSent >= ctx->chunkLength) {
                            if (!ctx->getNextChunk()) {
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
        static void startTransfer(NiceAgent *agent, const guint streamId, GMainContext *niceContext, const int n,
                                  std::string receiverId) {
            size_t fileSize = std::filesystem::file_size(SenderConfig::paths[0]);
            const auto transferStats = std::make_shared<common::SenderTransferMetrics>(receiverId, fileSize);
            common::senderMetrics[receiverId] = transferStats;
            const auto fileContext = std::make_shared<FileContext>(SenderConfig::paths[0]);;

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
                ctx->file = fileContext;
                ctx->senderMetrics = transferStats;

                nice_address_copy_to_sockaddr(&local->base_addr, reinterpret_cast<sockaddr *>(&ctx->localAddr));
                nice_address_copy_to_sockaddr(&remote->addr, reinterpret_cast<sockaddr *>(&ctx->remoteAddr));


                GSource *source = g_source_new(&common::quic_funcs, sizeof(common::QuicProcessSource));
                ((common::QuicProcessSource *) source)->ctx = ctx;

                g_source_set_priority(source, G_PRIORITY_HIGH);
                g_source_attach(source, niceContext);
                g_source_unref(source);


                connectionContexts_.push_back(ctx);


                nice_agent_attach_recv(agent, streamId, i, niceContext,
                                       [](NiceAgent *agent, guint stream_id, guint component_id,
                                          guint len, gchar *buf, gpointer user_data) {
                                           auto *c = static_cast<common::QuicConnectionContext *>(user_data);

                                           lsquic_engine_packet_in(engine_, (unsigned char *) buf, len,
                                                                   (sockaddr *) &c->localAddr,
                                                                   (sockaddr *) &c->remoteAddr,
                                                                   c, 0);

                                           c->tickScheduled = true;
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

            process();

            GSource *source = g_timeout_source_new(0);
            g_source_set_callback(source, engineTick, nullptr, nullptr);
            g_source_attach(source, niceContext);
            g_source_unref(source);
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
