#pragma once
#include <glib/gtypes.h>
#include<nice.h>
#include <lsquic.h>
#include <openssl/ssl.h>
#include <gio/gio.h>
#include <spdlog/spdlog.h>

#include "Utils.hpp"

#include <chrono>
#include <boost/asio/steady_timer.hpp>

namespace common {
    static int spdlog_log_buf(void *ctx, const char *buf, size_t len) {
        std::string msg(buf, len);

        if (!msg.empty() && msg.back() == '\n') {
            msg.pop_back();
        }

        spdlog::info("[LSQUIC] {}", msg);

        return 0;
    }

    static const lsquic_logger_if spdlog_logger_if = {
        .log_buf = spdlog_log_buf,
    };

    static void init_lsquic_logging() {
        lsquic_set_log_level("debug");

        lsquic_logger_init(&spdlog_logger_if, NULL, LLTS_NONE);
    }

    class Stream {
    protected:
        inline static lsquic_engine_t *engine_;
        inline static std::vector<ConnectionContext *> connectionContexts_;
        inline static SSL_CTX *sslCtx_ = nullptr;


        static gboolean engineTick(gpointer data) {
            if (!engine_) {
                return G_SOURCE_REMOVE;
            }

            process();

            int diff;

            if (lsquic_engine_earliest_adv_tick(engine_, &diff)) {
                if (diff <= 0) {
                    g_idle_add_full(G_PRIORITY_DEFAULT, engineTick, nullptr, nullptr);
                } else {
                    guint interval = (guint) ((diff + 999) / 1000);
                    g_timeout_add_full(G_PRIORITY_DEFAULT, interval, engineTick, nullptr, nullptr);
                }
            } else {
                g_timeout_add_full(G_PRIORITY_DEFAULT, 100, engineTick, nullptr, nullptr);
            }

            return G_SOURCE_REMOVE;
        }


        static SSL_CTX *getSslCtx(void *peer_ctx, const struct sockaddr *unused) {
            return sslCtx_;
        }

        static SSL_CTX *createSslCtx() {
            const SSL_METHOD *method = TLS_method();
            SSL_CTX *ctx = SSL_CTX_new(method);

            if (!ctx) {
                spdlog::error("Failed to create self-signed certificate for the QUIC connection");
                return nullptr;
            }

            SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
            SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

            static constexpr unsigned char alpn[] = {8, 't', 'h', 'r', 'u', 'f', 'l', 'u', 'x'};

            if (SSL_CTX_set_alpn_protos(ctx, alpn, sizeof(alpn)) != 0) {
                spdlog::error("Failed to set ALPN protocol");
            }

            return ctx;
        }

    public:
        static int sendPackets(void *packetsOutCtx, const lsquic_out_spec *specs, unsigned nSpecs) {
            if (nSpecs == 0) {
                return 0;
            }

            unsigned totalSent = 0;
            unsigned i = 0;

            static constexpr unsigned MAX_BATCH = 64;
            static constexpr unsigned MAX_VECTORS = MAX_BATCH * 4;
            NiceOutputMessage niceMessages[MAX_BATCH];
            GOutputVector niceVectors[MAX_VECTORS];

            while (i < nSpecs) {
                const auto *ctx = static_cast<ConnectionContext *>(
                    specs[i].conn_ctx ? specs[i].conn_ctx : specs[i].peer_ctx
                );
                if (!ctx) {
                    i++;
                    continue;
                }
                unsigned batchSize = 0;
                unsigned vecIdx = 0;
                while (i + batchSize < nSpecs && batchSize < MAX_BATCH) {
                    const auto &spec = specs[i + batchSize];

                    const void *nextCtxPtr = spec.conn_ctx ? spec.conn_ctx : spec.peer_ctx;
                    if (nextCtxPtr != ctx) {
                        break;
                    }

                    if (vecIdx + spec.iovlen > MAX_VECTORS) {
                        break;
                    }

                    niceMessages[batchSize].buffers = &niceVectors[vecIdx];
                    niceMessages[batchSize].n_buffers = spec.iovlen;

                    for (int k = 0; k < spec.iovlen; ++k) {
                        niceVectors[vecIdx].buffer = spec.iov[k].iov_base;
                        niceVectors[vecIdx].size = spec.iov[k].iov_len;
                        vecIdx++;
                    }

                    batchSize++;
                }

                const int nSent = nice_agent_send_messages_nonblocking(
                    ctx->agent,
                    ctx->streamId,
                    1,
                    niceMessages,
                    batchSize,
                    nullptr,
                    nullptr
                );


                if (nSent < 0) {
                    break;
                }

                totalSent += nSent;
                i += nSent;

                if (nSent < batchSize) {
                    return totalSent;
                }
            }

            return totalSent;
        }

        static void setAndVerifySocketBuffers(NiceAgent *agent, guint streamId, int componentId, const int bufSize) {
            GSocket *gsock = nice_agent_get_selected_socket(agent, streamId, componentId);
            if (gsock) {
                GError *error = nullptr;

                if (!g_socket_set_option(gsock, SOL_SOCKET, SO_SNDBUF, bufSize, &error)) {
                    spdlog::warn("Socket {} Failed to set SO_SNDBUF: {}", componentId, error->message);
                    g_clear_error(&error);
                }

                if (!g_socket_set_option(gsock, SOL_SOCKET, SO_RCVBUF, bufSize, &error)) {
                    spdlog::warn("Socket {} Failed to set SO_RCVBUF: {}", componentId, error->message);
                    g_clear_error(&error);
                }

                int actualSendBuf = 0;
                int actualRecvBuf = 0;

                if (g_socket_get_option(gsock, SOL_SOCKET, SO_SNDBUF, &actualSendBuf, &error)) {
                    spdlog::info("Socket {} Actual OS Send Buffer: {}",
                                 componentId, common::Utils::sizeToReadableFormat(actualSendBuf));
                } else {
                    spdlog::warn("Socket{} Failed to get SO_SNDBUF: {}", componentId, error->message);
                    g_clear_error(&error);
                }

                if (g_socket_get_option(gsock, SOL_SOCKET, SO_RCVBUF, &actualRecvBuf, &error)) {
                    spdlog::info("Socket{} Actual OS Receive Buffer: {}",
                                 componentId, common::Utils::sizeToReadableFormat(actualRecvBuf));
                } else {
                    spdlog::warn("Socket{} Failed to get SO_RCVBUF: {}", componentId, error->message);
                    g_clear_error(&error);
                }
            }
        }

        static void dispose() {
            if (engine_) {
                lsquic_engine_destroy(engine_);
                engine_ = nullptr;
            }


            for (const auto &context: connectionContexts_) {
                delete context;
            }


            if (sslCtx_) {
                SSL_CTX_free(sslCtx_);
                sslCtx_ = nullptr;
            }

            connectionContexts_.clear();

            lsquic_global_cleanup();
        }


        static void process() {
            lsquic_engine_process_conns(engine_);
            lsquic_engine_send_unsent_packets(engine_);
        }
    };
}
