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

    static constexpr uint64_t CONTROL_STREAM_ID = 2;

    struct FileHandleCache {
        struct Entry {
            int fd;
            uint64_t lastUsed;
        };

        std::unordered_map<uint32_t, Entry> openFiles;
        std::unordered_map<uint32_t, std::string> paths;
        const size_t MAX_FDS = 64;
        uint64_t accessCounter = 0;

        void registerPath(uint32_t id, const std::string &p) {
            paths[id] = p;
        }

        int get(uint32_t id, int flags, mode_t mode = 0) {
            accessCounter++;
            if (openFiles.contains(id)) {
                openFiles[id].lastUsed = accessCounter;
                return openFiles[id].fd;
            }

            if (openFiles.size() >= MAX_FDS) {
                uint32_t evictId = 0;
                uint64_t minT = UINT64_MAX;
                for (const auto &[fd, lastUsed]: openFiles | std::views::values) {
                    if (lastUsed < minT) {
                        minT = lastUsed;
                        evictId = fd;
                    }
                }
                close(openFiles[evictId].fd);
                openFiles.erase(evictId);
            }

            const int fd = open(paths[id].c_str(), flags, mode);
            if (fd == -1) {
                spdlog::error("Failed to open file: {}, {}", id, errno);
                return -1;
            }

            openFiles[id] = {fd, accessCounter};

            return fd;
        }

        ~FileHandleCache() {
            for (auto &[fd, lastUsed]: openFiles | std::views::values) {
                close(fd);
            }
        }
    };


    struct QuicConnectionContext {
        NiceAgent *agent;
        guint streamId;
        int componentId;
        lsquic_conn_t *connection;
        sockaddr_storage localAddr;
        sockaddr_storage remoteAddr;
        bool tickScheduled = false;
    };

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
        inline static std::vector<QuicConnectionContext *> connectionContexts_;
        inline static SSL_CTX *sslCtx_ = nullptr;


        static gboolean engineTick(gpointer data) {
            if (!engine_) {
                return G_SOURCE_REMOVE;
            }

            process();

            int diff;
            if (lsquic_engine_earliest_adv_tick(engine_, &diff)) {
                if (diff < 0) diff = 0;

                g_timeout_add(diff / 1000, engineTick, nullptr);
            } else {
                g_timeout_add(100, engineTick, nullptr);
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
                const auto *ctx = static_cast<QuicConnectionContext *>(
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

                int nSent = nice_agent_send_messages_nonblocking(
                    ctx->agent,
                    ctx->streamId,
                    ctx->componentId,
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
            senderMetrics.clear();

            lsquic_global_cleanup();
        }


        static void process() {
            if (engine_) lsquic_engine_process_conns(engine_);
        }
    };


    struct QuicProcessSource {
        GSource source;
        QuicConnectionContext *ctx;
    };


    static gboolean quic_prepare(GSource *source, gint *timeout) {
        auto *qs = (QuicProcessSource *) source;
        *timeout = -1;
        return qs->ctx->tickScheduled;
    }

    static gboolean quic_check(GSource *source) {
        auto *qs = (QuicProcessSource *) source;
        return qs->ctx->tickScheduled;
    }

    static gboolean quic_dispatch(GSource *source, GSourceFunc callback, gpointer user_data) {
        auto *qs = (QuicProcessSource *) source;
        Stream::process();
        qs->ctx->tickScheduled = false;
        return G_SOURCE_CONTINUE;
    }

    static GSourceFuncs quic_funcs = {
        quic_prepare,
        quic_check,
        quic_dispatch,
        nullptr
    };
}
