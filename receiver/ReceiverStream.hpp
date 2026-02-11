#pragma once
#include <nice.h>
#include <lsquic.h>
#include <openssl/base.h>
#include <openssl/ssl.h>
#include <spdlog/spdlog.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <sys/mman.h>

#include "ReceiverConfig.hpp"
#include "../common/Metrics.hpp"
#include "../common/Stream.hpp"


namespace receiver {
    struct ReceiverState {
        common::FileHandleCache cache;
        std::vector<uint8_t> manifestBuf;
        bool manifestParsed = false;
        uint64_t totalExpectedBytes = 0;
        int totalExpectedFilesCount = 0;
        std::vector<uint64_t> fileSizes;
        std::vector<uint64_t> bytesWritten;

        void parseManifest() {
            uint8_t *p = manifestBuf.data();
            uint32_t count;
            memcpy(&count, p, 4);
            p += 4;
            fileSizes.resize(count);
            bytesWritten.resize(count, 0);

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
            manifestParsed = true;
            spdlog::info("Manifest parsed. {} files.", count);
        }
    };

    inline std::shared_ptr<ReceiverState> receiverState;

    struct ReceiverContext {
        enum StreamType { UNKNOWN, CONTROL, DATA } type = UNKNOWN;

        bool readingHeader = true;

        uint8_t headerBuf[16];
        uint8_t headerBytesRead = 0;

        uint64_t chunkOffset = 0;
        uint32_t chunkLength = 0;
        uint32_t bodyBytesRead = 0;
        uint32_t fileId = 0;
        alignas(4096) uint8_t writeBuffer[256 * 1024];
    };

    class ReceiverStream : public common::Stream {
        static void watchProgress() {
            common::receiverMetrics.startedTime = std::chrono::high_resolution_clock::now();

            g_timeout_add_full(G_PRIORITY_HIGH, 1000, [](gpointer data)-> gboolean {
                spdlog::info("watching..");
                const auto now = std::chrono::high_resolution_clock::now();
                if (common::receiverMetrics.lastTime.time_since_epoch().count() == 0) {
                    common::receiverMetrics.lastTime = now;
                    return G_SOURCE_CONTINUE;
                }

                std::chrono::duration<double> elapsed = now - common::receiverMetrics.startedTime;
                std::chrono::duration<double> delta = now - common::receiverMetrics.lastTime;

                const double elapsedSeconds = elapsed.count();
                const double deltaSeconds = delta.count();

                const double instantThroughput =
                        (common::receiverMetrics.bytesReceived - common::receiverMetrics.lastSnapshot) / deltaSeconds;
                const double averageThroughput = common::receiverMetrics.bytesReceived / elapsedSeconds;
                const double ewmaThroughput = common::receiverMetrics.ewmaThroughput == 0
                                                  ? instantThroughput
                                                  : 0.2 * instantThroughput + 0.8 * common::receiverMetrics.
                                                    ewmaThroughput;

                const double percent = (static_cast<double>(common::receiverMetrics.bytesReceived) / receiverState->
                                        totalExpectedBytes) * 100.0;

                common::receiverMetrics.lastTime = now;
                common::receiverMetrics.lastSnapshot = common::receiverMetrics.bytesReceived;

                spdlog::info(
                    "Instant Throughput: {}/s | EWMA Throughput: {}/s |  Elapsed: {}s | Average Throughput: {}/s | Total Received: {} | Progress: {}% | Files {}/{} ",
                    common::Utils::sizeToReadableFormat(instantThroughput),
                    common::Utils::sizeToReadableFormat(ewmaThroughput), elapsedSeconds,
                    common::Utils::sizeToReadableFormat(averageThroughput),
                    common::Utils::sizeToReadableFormat(common::receiverMetrics.bytesReceived), percent,
                    common::receiverMetrics.filesReceived,
                    receiverState->totalExpectedFilesCount
                );

                if (common::receiverMetrics.bytesReceived >= receiverState->totalExpectedBytes) {
                    spdlog::info("Transfer complete.");
                    common::ThreadManager::terminate();
                    return G_SOURCE_REMOVE;
                }
                return G_SOURCE_CONTINUE;
            }, nullptr, nullptr);
        }

        static int alpnSelectCallback(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                                      const unsigned char *in, unsigned int inlen, void *arg) {
            *out = reinterpret_cast<const unsigned char *>("thruflux");
            *outlen = 8;

            return SSL_TLSEXT_ERR_OK;
        }


        static void loadInMemoryCertificate(SSL_CTX *ctx) {
            EVP_PKEY_CTX *pkt = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
            if (!pkt) throw std::runtime_error("Failed to create keygen ctx");

            if (EVP_PKEY_keygen_init(pkt) <= 0) throw std::runtime_error("Keygen init failed");

            if (EVP_PKEY_CTX_set_rsa_keygen_bits(pkt, 2048) <= 0) throw std::runtime_error("RSA bit set failed");

            EVP_PKEY *pkey = nullptr;
            if (EVP_PKEY_keygen(pkt, &pkey) <= 0) throw std::runtime_error("Key generation failed");

            X509 *x509 = X509_new();
            X509_set_version(x509, 2);
            ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
            X509_gmtime_adj(X509_get_notBefore(x509), 0);
            X509_gmtime_adj(X509_get_notAfter(x509), 31536000L); // 1 year

            X509_set_pubkey(x509, pkey);
            X509_NAME *name = X509_get_subject_name(x509);
            X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char *) "thruflux.local", -1, -1, 0);
            X509_set_issuer_name(x509, name);

            if (!X509_sign(x509, pkey, EVP_sha256())) throw std::runtime_error("Signing failed");

            if (SSL_CTX_use_certificate(ctx, x509) <= 0) throw std::runtime_error("Cert use failed");
            if (SSL_CTX_use_PrivateKey(ctx, pkey) <= 0) throw std::runtime_error("Key use failed");

            EVP_PKEY_CTX_free(pkt);
            X509_free(x509);
            EVP_PKEY_free(pkey);
        }


        inline static lsquic_stream_if streamCallbacks = {
            .on_new_conn = [](void *streamIfCtx, lsquic_conn_t *c) -> lsquic_conn_ctx * {
                auto *ctx = static_cast<common::QuicConnectionContext *>(lsquic_conn_get_peer_ctx(c, nullptr));
                ctx->connection = c;
                spdlog::info("New QUIC connection on ICE Comp {}", ctx->componentId);


                return reinterpret_cast<lsquic_conn_ctx *>(ctx);
            },
            .on_conn_closed = [](lsquic_conn_t *c) {
                auto *ctx = reinterpret_cast<common::QuicConnectionContext *>(lsquic_conn_get_ctx(c));
                spdlog::warn("QUIC Connection closed on ICE Comp {}", ctx ? ctx->componentId : 0);
                lsquic_conn_set_ctx(c, nullptr);
                if (ctx) {
                    ctx->connection = nullptr;
                }
            },
            .on_new_stream = [](void *stream_if_ctx, lsquic_stream_t *stream) -> lsquic_stream_ctx_t * {
                lsquic_stream_wantread(stream, 1);
                return reinterpret_cast<lsquic_stream_ctx_t *>(new ReceiverContext());
            },
            .on_read = [](lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
                auto *ctx = reinterpret_cast<ReceiverContext *>(h);

                if (ctx->type == ReceiverContext::UNKNOWN) {
                    uint8_t tag;
                    if (lsquic_stream_read(stream, &tag, 1) == 1) {
                        ctx->type = (tag == 0x00) ? ReceiverContext::CONTROL : ReceiverContext::DATA;
                        if (ctx->type == ReceiverContext::DATA && !common::receiverMetrics.started) {
                            common::receiverMetrics.started = true;
                            watchProgress();
                        }
                    } else {
                        return;
                    }
                    return;
                }

                if (ctx->type == ReceiverContext::CONTROL) {
                    uint8_t tmp[4096];
                    ssize_t nr = lsquic_stream_read(stream, tmp, sizeof(tmp));
                    if (nr > 0) receiverState->manifestBuf.insert(receiverState->manifestBuf.end(), tmp, tmp + nr);
                    if (nr == 0 && !receiverState->manifestParsed) {
                        receiverState->parseManifest();
                        const uint8_t ack = 0x06;
                        lsquic_stream_write(stream, &ack, 1);
                        lsquic_stream_flush(stream);
                        lsquic_stream_shutdown(stream, 1);
                    }

                    return;
                }

                if (!receiverState->manifestParsed) {
                    //wait until manifest gets parsed
                    return;
                }

                while (true) {
                    if (ctx->readingHeader) {
                        size_t remaining = 16 - ctx->headerBytesRead;
                        ssize_t nr = lsquic_stream_read(stream, ctx->headerBuf, remaining);
                        if (nr <= 0) {
                            return;
                        }

                        ctx->headerBytesRead += nr;

                        if (ctx->headerBytesRead == 16) {
                            memcpy(&ctx->chunkOffset, ctx->headerBuf, 8);
                            memcpy(&ctx->chunkLength, ctx->headerBuf + 8, 4);
                            memcpy(&ctx->fileId, ctx->headerBuf + 12, 4);

                            ctx->readingHeader = false;
                            ctx->bodyBytesRead = 0;
                        } else {
                            return;
                        }
                    } else {
                        size_t remaining = ctx->chunkLength - ctx->bodyBytesRead;
                        size_t readSize = std::min(sizeof(ctx->writeBuffer), remaining);
                        ssize_t nr = lsquic_stream_read(stream, ctx->writeBuffer, readSize);
                        if (nr <= 0) {
                            return;
                        }

                        const int fd = receiverState->cache.get(ctx->fileId, O_WRONLY | O_CREAT, 0644);
                        if (fd == -1) {
                            spdlog::error("Unexpected error: Could not get fd");
                            lsquic_stream_close(stream);
                            return;
                        }


                        uint64_t writePos = ctx->chunkOffset + ctx->bodyBytesRead;
                        ssize_t nw = pwrite(fd, ctx->writeBuffer, nr, writePos);
                        common::receiverMetrics.bytesReceived += nw;
                        receiverState->bytesWritten[ctx->fileId] += nw;
                        if (receiverState->bytesWritten[ctx->fileId] == receiverState->fileSizes[ctx->fileId]) {
                            common::receiverMetrics.filesReceived++;
                        }

                        if (nw < 0) {
                            spdlog::error("Could not write to disk: {}", errno);
                            lsquic_stream_close(stream);
                            return;
                        }

                        ctx->bodyBytesRead += nr;

                        if (ctx->bodyBytesRead >= ctx->chunkLength) {
                            ctx->readingHeader = true;
                            ctx->headerBytesRead = 0;
                        }
                    }
                }
            },
            .on_write = [](lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
            },
            .on_close = [](lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
                const auto *ctx = reinterpret_cast<ReceiverContext *>(h);
                delete ctx;
            },
            .on_hsk_done = [](lsquic_conn_t *c, enum lsquic_hsk_status status) {
                auto *ctx = reinterpret_cast<common::QuicConnectionContext *>(lsquic_conn_get_ctx(c));
                if (status == LSQ_HSK_OK || status == LSQ_HSK_RESUMED_OK) {
                    spdlog::info("QUIC Handshake SUCCESS on ICE Comp {}", ctx->componentId);
                }
            }
        };

    public:
        static void initialize() {
            // common::init_lsquic_logging();
            sslCtx_ = createSslCtx();
            SSL_CTX_set_alpn_select_cb(sslCtx_, alpnSelectCallback, nullptr);
            loadInMemoryCertificate(sslCtx_);
            lsquic_global_init(LSQUIC_GLOBAL_SERVER);
            lsquic_engine_settings settings;
            lsquic_engine_init_settings(&settings, LSENG_SERVER);
            settings.es_versions = (1 << LSQVER_I001);
            settings.es_cc_algo = 2;
            settings.es_init_max_data = ReceiverConfig::quicConnWindowBytes;
            settings.es_init_max_streams_uni = ReceiverConfig::quicMaxIncomingStreams;
            settings.es_init_max_streams_bidi = ReceiverConfig::quicMaxIncomingStreams;
            settings.es_idle_conn_to = 30000000;
            settings.es_init_max_stream_data_uni = ReceiverConfig::quicStreamWindowBytes;
            settings.es_init_max_stream_data_bidi_local = ReceiverConfig::quicStreamWindowBytes;
            settings.es_init_max_stream_data_bidi_remote = ReceiverConfig::quicStreamWindowBytes;
            settings.es_handshake_to = 16777215;
            settings.es_allow_migration = 0;

            char err_buf[256];
            if (0 != lsquic_engine_check_settings(&settings, LSENG_SERVER, err_buf, sizeof(err_buf))) {
                spdlog::error("Invalid lsquic engine settings: {}", err_buf);
                return;
            }
            lsquic_engine_api api = {};
            api.ea_settings = &settings;
            api.ea_stream_if = &streamCallbacks;
            api.ea_packets_out = sendPackets;
            api.ea_get_ssl_ctx = getSslCtx;
            engine_ = lsquic_engine_new(LSENG_SERVER, &api);

            spdlog::info("LSQUIC Engine Successfully Initialized. {}", engine_ == nullptr);
        }


        static void receiveTransfer(NiceAgent *agent, const guint streamId, const int n) {
            if (!receiverState) {
                receiverState = std::make_shared<ReceiverState>();
            }
            for (int i = 1; i <= n; i++) {
                setAndVerifySocketBuffers(agent, streamId, i, ReceiverConfig::udpBufferBytes);
                NiceCandidate *local = nullptr, *remote = nullptr;
                if (!nice_agent_get_selected_pair(agent, streamId, i, &local, &remote)) {
                    spdlog::error("ICE component {} is not ready for QUIC connection", i);
                    continue;
                }

                auto *ctx = new common::QuicConnectionContext();
                ctx->agent = agent;
                ctx->streamId = streamId;
                ctx->componentId = i;
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

                                           // int diff = 0;
                                           // if (lsquic_engine_earliest_adv_tick(engine_, &diff)) {
                                           //     if (diff <= 0) {
                                           //         lsquic_engine_process_conns(engine_);
                                           //     }
                                           // }
                                       },
                                       ctx
                );
            }
            g_timeout_add(0, engineTick, nullptr);
        }
    };
};
