#pragma once
#include<nice.h>
#include <lsquic.h>
#include <openssl/ssl.h>

#include "SenderConfig.hpp"
#include <sys/mman.h>
#include "../common/Stream.hpp"
#include "SenderContexts.hpp"

namespace sender {
    class SenderStream : public common::Stream {
        //unlike receiver, sender's progress reporter should run persistently
        static void watchProgress() {
            g_timeout_add_full(G_PRIORITY_HIGH, 1000, [](gpointer data)-> gboolean {
                for (const auto &context: connectionContexts_) {
                    if (!context->started || context->complete) continue;

                    const auto now = std::chrono::high_resolution_clock::now();
                    if (context->lastTime.time_since_epoch().count() == 0) {
                        context->lastTime = now;
                        return G_SOURCE_CONTINUE;
                    }

                    std::chrono::duration<double> elapsed = now - context->startTime;
                    std::chrono::duration<double> delta = now - context->lastTime;

                    const double elapsedSeconds = elapsed.count();
                    const double deltaSeconds = delta.count();

                    const double instantThroughput =
                            (context->bytesMoved - context->lastBytesMoved) / deltaSeconds;
                    const double averageThroughput = context->bytesMoved / elapsedSeconds;
                    const double ewmaThroughput = context->ewmaThroughput == 0
                                                      ? instantThroughput
                                                      : 0.2 * instantThroughput + 0.8 * context->
                                                        ewmaThroughput;

                    const double percent = (static_cast<double>(context->bytesMoved) /
                                            senderPersistentContext.totalExpectedBytes) * 100.0;


                    context->lastTime = now;
                    context->lastBytesMoved = context->bytesMoved;
                    spdlog::info(
                        "Receiver : {} | Instant Throughput: {}/s | EWMA Throughput: {}/s |  Elapsed: {}s | Average Throughput: {}/s | Total Sent: {} | Progress: {}% | "
                        "Files {}/{}",
                        static_cast<SenderConnectionContext *>(context)->receiverId,
                        common::Utils::sizeToReadableFormat(instantThroughput),
                        common::Utils::sizeToReadableFormat(ewmaThroughput), elapsedSeconds,
                        common::Utils::sizeToReadableFormat(averageThroughput),
                        common::Utils::sizeToReadableFormat(context->bytesMoved), percent,
                        context->filesMoved,
                        senderPersistentContext.totalExpectedFilesCount
                    );
                }
                return G_SOURCE_CONTINUE;
            }, nullptr, nullptr);
        }

        inline static lsquic_stream_if streamCallbacks = {
            .on_new_conn = [](void *streamIfCtx, lsquic_conn_t *connection) -> lsquic_conn_ctx * {
                auto *ctx = static_cast<SenderConnectionContext *>(lsquic_conn_get_peer_ctx(connection, nullptr));
                ctx->connection = connection;
                //open manifest stream
                lsquic_conn_make_stream(connection);
                spdlog::info("QUIC connection established");
                return reinterpret_cast<lsquic_conn_ctx *>(ctx);
            },
            .on_conn_closed = [](lsquic_conn_t *connection) {
                auto *ctx = reinterpret_cast<SenderConnectionContext *>(lsquic_conn_get_ctx(connection));
                spdlog::info("QUIC Connection closed");;
                lsquic_conn_set_ctx(connection, nullptr);
                if (ctx) {
                    if (ctx->complete) {
                        spdlog::info("Transfer completed for receiver {}", ctx->receiverId);
                    } else {
                        spdlog::error("Transfer failed for receiver {}", ctx->receiverId);
                    }
                    std::erase(connectionContexts_, ctx);
                    ctx->connection = nullptr;
                    delete ctx;
                }
            },

            .on_new_stream = [](void *stream_if_ctx, lsquic_stream_t *stream) -> lsquic_stream_ctx * {
                auto *connCtx = reinterpret_cast<SenderConnectionContext *>(lsquic_conn_get_ctx(
                    lsquic_stream_conn(stream)));
                lsquic_stream_wantwrite(stream, 1);

                auto *ctx = new SenderStreamContext();

                ctx->connectionContext = connCtx;

                if (!connCtx->manifestStreamCreated) {
                    ctx->isManifestStream = true;
                    connCtx->manifestStreamCreated = true;
                } else {
                    ctx->isManifestStream = false;
                    if (!ctx->loadNextChunk()) {
                        lsquic_stream_shutdown(stream, 1);
                    }
                }

                return reinterpret_cast<lsquic_stream_ctx *>(ctx);
            },
            .on_read = [](lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
                auto *ctx = reinterpret_cast<SenderStreamContext *>(h);
                auto *connCtx = reinterpret_cast<SenderConnectionContext *>(lsquic_conn_get_ctx(
                    lsquic_stream_conn(stream)));

                if (ctx->isManifestStream) {
                    uint8_t buf[1];
                    const auto nr = lsquic_stream_read(stream,buf,1);

                    if (nr == 1) {
                        if (buf[0] == common::RECEIVER_MANIFEST_RECEIVED_ACK) {
                            //Time to blast data!
                            if (!connCtx->started) {
                                connCtx->started = true;
                                connCtx->startTime = std::chrono::high_resolution_clock::now();
                            }
                            //Open all the streams
                            for (int i = 0; i < SenderConfig::totalStreams; i++) {
                                lsquic_conn_make_stream(connCtx->connection);
                            }
                            //save the manifest stream for reading future ack
                            connCtx->manifestStream = stream;
                            lsquic_stream_wantread(stream,0);
                        }
                        else if (buf[0] == common::RECEIVER_TRANSFER_COMPLETE_ACK) {
                            connCtx->complete = true;
                            connCtx->endTime = std::chrono::high_resolution_clock::now();
                            lsquic_stream_shutdown(stream,0);
                            if (connCtx->connection) {
                                lsquic_conn_close(connCtx->connection);
                                connCtx->connection = nullptr;
                            }
                        }
                    }
                }
            },
            .on_write = [](lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {

                auto *ctx = reinterpret_cast<SenderStreamContext *>(h);


                if (!ctx->typeByteSent) {
                    uint8_t tag = ctx->isManifestStream ? 0x00 : 0x01;
                    ssize_t nw = lsquic_stream_write(stream, &tag, 1);
                    if (nw > 0) {
                        ctx->typeByteSent = true;
                    } else {
                        return;
                    }
                    return;

                }

                auto *connCtx = reinterpret_cast<SenderConnectionContext *>(lsquic_conn_get_ctx(
                    lsquic_stream_conn(stream)));


                if (ctx->isManifestStream) {
                    size_t total = senderPersistentContext.manifestBlob.size();
                    size_t sent = connCtx->manifestSent;
                    if (sent < total) {
                        ssize_t nw = lsquic_stream_write(stream, senderPersistentContext.manifestBlob.data() + sent,
                                                         total - sent);
                        if (nw > 0) connCtx->manifestSent += nw;
                    }
                    if (connCtx->manifestSent == total) {
                        lsquic_stream_flush(stream);
                        //wait for manifest ack
                        lsquic_stream_wantread(stream,1);
                        //half-close the write side
                        lsquic_stream_shutdown(stream, 1);
                    }
                    return;
                }


                while (true) {
                    if (ctx->sendingHeader) {
                        size_t remaining = 16 - ctx->headerSent;
                        ssize_t nw = lsquic_stream_write(stream, ctx->headerBuf + ctx->headerSent, remaining);
                        if (nw <= 0) {
                            return;
                        }
                        ctx->headerSent += nw;
                        if (ctx->headerSent == 16) {
                            ctx->sendingHeader = false;
                        } else {
                            return;
                        }
                    } else {
                        spdlog::info("DATA LOOP");
                        if (!ctx->currentMmap) {
                            spdlog::error("Unexpected error: currentMmap missing for QUIC connection");
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
                        connCtx->bytesMoved += nw;

                        if (ctx->bytesSent >= ctx->len) {
                            ctx->currentMmap = nullptr;
                            if (!ctx->loadNextChunk()) {
                                lsquic_stream_shutdown(stream, 1);
                                //now wait for receiver ACK
                                lsquic_stream_wantread(connCtx->manifestStream,1);
                                return;
                            }
                        }
                    }
                }
            },
            .on_close = [](lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
                const auto *ctx = reinterpret_cast<SenderStreamContext *>(h);
                delete ctx;
            },
            .on_hsk_done = [](lsquic_conn_t *connection, enum lsquic_hsk_status status) {
                if (status == LSQ_HSK_OK || status == LSQ_HSK_RESUMED_OK) {
                    spdlog::info("QUIC Handshake Successful");
                } else {
                    spdlog::error("QUIC Handshake Failed");
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


        static void startTransfer(NiceAgent *agent, const guint streamId,
                                  std::string receiverId) {
            setAndVerifySocketBuffers(agent, streamId, 1, SenderConfig::udpBufferBytes);

            NiceCandidate *local = nullptr, *remote = nullptr;
            if (!nice_agent_get_selected_pair(agent, streamId, 1, &local, &remote)) {
                spdlog::error("QUIC connection not ready");
                return;
            }


            auto *ctx = new SenderConnectionContext();
            ctx->agent = agent;
            ctx->streamId = streamId;
            ctx->receiverId = receiverId;

            nice_address_copy_to_sockaddr(&local->base_addr, reinterpret_cast<sockaddr *>(&ctx->localAddr));
            nice_address_copy_to_sockaddr(&remote->addr, reinterpret_cast<sockaddr *>(&ctx->remoteAddr));


            connectionContexts_.push_back(ctx);


            nice_agent_attach_recv(agent, streamId, 1, common::ThreadManager::getContext(),
                                   [](NiceAgent *agent, guint stream_id, guint component_id,
                                      guint len, gchar *buf, gpointer user_data) {
                                       auto *c = static_cast<common::ConnectionContext *>(user_data);

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


            g_timeout_add(0, engineTick, nullptr);
            watchProgress();
        }


        static void disposeReceiverConnection(std::string_view receiverId) {
            for (const auto *ctx: connectionContexts_) {
                if (((SenderConnectionContext *) ctx)->receiverId == receiverId) {
                    if (ctx->connection) {
                        lsquic_conn_close(ctx->connection);
                        ctx->connection = nullptr;
                    }
                    break;
                }
            }

            process();
        }
    };
}
