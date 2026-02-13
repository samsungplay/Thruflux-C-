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
#include "ReceiverContexts.hpp"
#include "../common/Contexts.hpp"
#include "../common/Stream.hpp"


namespace receiver {
    class ReceiverStream : public common::Stream {
        static void watchProgress() {
            auto *ctx = static_cast<ReceiverConnectionContext *>(connectionContexts_[0]);
            ctx->startTime = std::chrono::high_resolution_clock::now();

            g_timeout_add_full(G_PRIORITY_HIGH, 1000, [](gpointer data)-> gboolean {
                auto *receiverConnectionContext = static_cast<ReceiverConnectionContext *>(
                    connectionContexts_[0]);
                if (!receiverConnectionContext) {
                    return G_SOURCE_CONTINUE;
                }
                if (receiverConnectionContext->complete) {
                    return G_SOURCE_REMOVE;
                }
                spdlog::info("watching.. {}", receiverConnectionContext->totalExpectedBytes);
                const auto now = std::chrono::high_resolution_clock::now();
                if (receiverConnectionContext->lastTime.time_since_epoch().count() == 0) {
                    receiverConnectionContext->lastTime = now;
                    return G_SOURCE_CONTINUE;
                }

                std::chrono::duration<double> elapsed = now - receiverConnectionContext->startTime;
                std::chrono::duration<double> delta = now - receiverConnectionContext->lastTime;

                const double elapsedSeconds = elapsed.count();
                const double deltaSeconds = delta.count();

                const double instantThroughput =
                        (receiverConnectionContext->bytesMoved - receiverConnectionContext->lastBytesMoved) /
                        deltaSeconds;
                const double averageThroughput = receiverConnectionContext->bytesMoved / elapsedSeconds;
                const double ewmaThroughput = receiverConnectionContext->ewmaThroughput == 0
                                                  ? instantThroughput
                                                  : 0.2 * instantThroughput + 0.8 * receiverConnectionContext->
                                                    ewmaThroughput;
                receiverConnectionContext->ewmaThroughput = ewmaThroughput;

                const double percent = (static_cast<double>(receiverConnectionContext->bytesMoved) /
                                        receiverConnectionContext->
                                        totalExpectedBytes) * 100.0;

                receiverConnectionContext->lastTime = now;
                receiverConnectionContext->lastBytesMoved = receiverConnectionContext->bytesMoved;

                spdlog::info(
                    "Instant Throughput: {}/s | EWMA Throughput: {}/s |  Elapsed: {}s | Average Throughput: {}/s | Total Received: {} | Progress: {}% | Files {}/{} ",
                    common::Utils::sizeToReadableFormat(instantThroughput),
                    common::Utils::sizeToReadableFormat(ewmaThroughput), elapsedSeconds,
                    common::Utils::sizeToReadableFormat(averageThroughput),
                    common::Utils::sizeToReadableFormat(receiverConnectionContext->bytesMoved), percent,
                    receiverConnectionContext->filesMoved,
                    receiverConnectionContext->totalExpectedFilesCount
                );

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
                auto *ctx = static_cast<ReceiverConnectionContext *>(lsquic_conn_get_peer_ctx(c, nullptr));
                ctx->connection = c;
                spdlog::info("New QUIC connection formed");
                return reinterpret_cast<lsquic_conn_ctx *>(ctx);
            },
            .on_conn_closed = [](lsquic_conn_t *c) {
                auto *ctx = reinterpret_cast<ReceiverConnectionContext *>(lsquic_conn_get_ctx(c));
                spdlog::warn("QUIC Connection closed");
                lsquic_conn_set_ctx(c, nullptr);
                if (ctx) {
                    if (ctx->complete) {
                        spdlog::info("Transfer completed.");
                        const std::chrono::duration<double> diff =  ctx->endTime - ctx->startTime;
                        spdlog::info("Time taken: {}s", diff.count());
                    }
                    else {
                        spdlog::error("Transfer failed.");
                    }
                    ctx->connection = nullptr;
                }
                //no need to delete connection context pointer for receiver; to be handled by dispose() function anyways
                common::ThreadManager::terminate();
            },
            .on_new_stream = [](void *stream_if_ctx, lsquic_stream_t *stream) -> lsquic_stream_ctx_t * {
                lsquic_stream_wantread(stream, 1);
                return reinterpret_cast<lsquic_stream_ctx_t *>(new ReceiverStreamContext());
            },
            .on_read = [](lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
                auto *connCtx = reinterpret_cast<ReceiverConnectionContext *>(lsquic_conn_get_ctx(
                    lsquic_stream_conn(stream)));
                auto *ctx = reinterpret_cast<ReceiverStreamContext *>(h);

                if (ctx->type == ReceiverStreamContext::UNKNOWN) {
                    uint8_t tag;
                    if (lsquic_stream_read(stream, &tag, 1) == 1) {
                        ctx->type = (tag == 0x00)
                                        ? ReceiverStreamContext::MANIFEST
                                        : ReceiverStreamContext::DATA;
                        if (ctx->type == ReceiverStreamContext::DATA && !connCtx->started) {
                            connCtx->started = true;
                            watchProgress();
                        }
                    } else {
                        return;
                    }
                }

                if (ctx->type == ReceiverStreamContext::MANIFEST) {
                    uint8_t tmp[4096];
                    while (!connCtx->manifestParsed) {
                        const auto nr = lsquic_stream_read(stream, tmp, sizeof(tmp));
                        if (nr > 0) connCtx->manifestBuf.insert(connCtx->manifestBuf.end(), tmp, tmp + nr);
                        if (nr == 0) {
                            connCtx->parseManifest();
                            connCtx->manifestParsed = true;
                            connCtx->pendingManifestAck = true;
                            //must write ACK
                            lsquic_stream_wantwrite(stream, 1);
                            //no reading
                            lsquic_stream_wantread(stream,0);
                            break;
                        }
                        if (nr < 0) {
                            spdlog::error("Error while reading manifest stream");
                        }
                    }
                    return;
                }

                while (true) {
                    if (ctx->readingHeader) {
                        size_t remaining = 16 - ctx->headerBytesRead;
                        ssize_t nr = lsquic_stream_read(stream, ctx->headerBuf + ctx->headerBytesRead, remaining);
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
                            break;
                        }

                        const int fd = connCtx->cache.get(ctx->fileId, O_WRONLY | O_CREAT, 0644);
                        if (fd == -1) {
                            spdlog::error("Unexpected error: Could not get fd");
                            lsquic_stream_close(stream);
                            return;
                        }

                        uint64_t writePos = ctx->chunkOffset + ctx->bodyBytesRead;
                        ssize_t nw = pwrite(fd, ctx->writeBuffer, nr, writePos);
                        connCtx->bytesMoved += nw;
                        connCtx->perFileBytesWritten[ctx->fileId] += nw;
                        if (connCtx->perFileBytesWritten[ctx->fileId] == connCtx->fileSizes[ctx->fileId]) {
                            connCtx->filesMoved++;
                        }

                        if (nw < 0) {
                            spdlog::error("Could not write to disk: {}", errno);
                            lsquic_stream_close(stream);
                            return;
                        }

                        ctx->bodyBytesRead += nw;

                        if (ctx->bodyBytesRead >= ctx->chunkLength) {
                            ctx->readingHeader = true;
                            ctx->headerBytesRead = 0;
                        }

                        if (connCtx->bytesMoved >= connCtx->totalExpectedBytes) {
                            //transfer complete. needs to send ACK to sender..
                            connCtx->complete = true;
                            connCtx->pendingCompleteAck = true;
                            connCtx->endTime = std::chrono::high_resolution_clock::now();
                            lsquic_stream_wantwrite(connCtx->manifestStream,1);
                            lsquic_stream_wantread(stream,0);
                            return;
                        }
                    }
                }
            },
            .on_write = [](lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
                auto *connCtx = reinterpret_cast<ReceiverConnectionContext *>(lsquic_conn_get_ctx(
                    lsquic_stream_conn(stream)));
                auto *ctx = reinterpret_cast<ReceiverStreamContext *>(h);

                if (ctx->type == ReceiverStreamContext::MANIFEST) {
                    if (connCtx->pendingManifestAck) {
                        uint8_t ack = common::RECEIVER_MANIFEST_RECEIVED_ACK;
                        const auto nw = lsquic_stream_write(stream, &ack, 1);
                        if (nw == 1) {
                            lsquic_stream_flush(stream);
                            connCtx->pendingManifestAck = false;
                            //save the manifest stream for writing future ACK
                            connCtx->manifestStream = stream;
                            lsquic_stream_wantwrite(stream,0);
                        }
                    }
                    if (connCtx->pendingCompleteAck) {
                        uint8_t ack = common::RECEIVER_TRANSFER_COMPLETE_ACK;
                        const auto nw = lsquic_stream_write(stream, &ack, 1);
                        if (nw == 1) {
                            lsquic_stream_flush(stream);
                            connCtx->pendingCompleteAck = false;
                            lsquic_stream_wantwrite(stream,0);
                        }
                    }
                }
            },
            .on_close = [](lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
                const auto *ctx = reinterpret_cast<ReceiverStreamContext *>(h);
                delete ctx;
            },
            .on_hsk_done = [](lsquic_conn_t *c, enum lsquic_hsk_status status) {
                if (status == LSQ_HSK_OK || status == LSQ_HSK_RESUMED_OK) {
                    spdlog::info("QUIC Handshake Successful");
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
            settings.es_pace_packets = 0;
            settings.es_delayed_acks = 0;
            settings.es_max_batch_size = 32;
            settings.es_rw_once = 1;
            settings.es_scid_len = 8;
            settings.es_max_cfcw = 1024 * 1024 * 1024;
            settings.es_max_sfcw = 256 * 1024 * 1024;

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


        static void receiveTransfer(NiceAgent *agent, const guint streamId) {
            setAndVerifySocketBuffers(agent, streamId, 1, ReceiverConfig::udpBufferBytes);
            NiceCandidate *local = nullptr, *remote = nullptr;
            if (!nice_agent_get_selected_pair(agent, streamId, 1, &local, &remote)) {
                spdlog::error("ICE not ready for QUIC connection");
                return;
            }

            auto *ctx = new ReceiverConnectionContext();
            ctx->agent = agent;
            ctx->streamId = streamId;
            nice_address_copy_to_sockaddr(&local->addr, reinterpret_cast<sockaddr *>(&ctx->localAddr));
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
                                   process();
                               },
                               ctx
        );

            g_timeout_add(0, engineTick, nullptr);
        }
    };
};
