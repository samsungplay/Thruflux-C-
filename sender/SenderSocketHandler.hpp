#pragma once
#include <IXWebSocket.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <spdlog/spdlog.h>
#include "SenderConfig.hpp"
#include <latch>
#include "../common/Payloads.hpp"
#include "../common/Utils.hpp"
#include <boost/asio/post.hpp>
#include "SenderFileHandler.hpp"
#include "SenderStateHolder.hpp"
#include "SenderStream.hpp"
#include "../common/Worker.hpp"

namespace sender {
    struct DelayedTransferTask {
        NiceAgent *agent;
        guint streamId;
        int n;
        std::string receiverId;
    };

    static gboolean execute_delayed_transfer(gpointer data) {
        auto *task = static_cast<DelayedTransferTask *>(data);

        spdlog::info("3s delay finished. Starting Transfer for {}", task->receiverId);

        // Call your actual logic
        SenderStream::startTransfer(task->agent, task->streamId, common::IceHandler::getContext(), task->n,
                                    task->receiverId);

        // Clean up the memory we allocated for the task
        delete task;

        return FALSE; // Return FALSE so the timer only runs ONCE
    }

    class SenderSocketHandler {
    public:
        static void onConnect(ix::WebSocket &socket) {
            spdlog::info("Successfully connected to signaling server : {}", SenderConfig::serverUrl);
        }

        static void onClose(ix::WebSocket &socket, std::latch &clientDone, std::string_view reason) {
            spdlog::info("Disconnected from signaling server : {} Reason: {}", SenderConfig::serverUrl, reason);
            clientDone.count_down();
        }

        static void onMessage(ix::WebSocket &socket, std::string_view message) {
            try {
                nlohmann::json j = nlohmann::json::parse(message);
                const std::string type = j.value("type", "");
                if (type == "turn_credentials_payload") {
                    const auto turnCredentialsPayload = j.get<common::TurnCredentialsPayload>();
                    if (turnCredentialsPayload.username != "none" || turnCredentialsPayload.password != "none") {
                        if (auto turnServer = common::Utils::toTurnServer(
                            turnCredentialsPayload.turnUrl, turnCredentialsPayload.username,
                            turnCredentialsPayload.password); turnServer.has_value()) {
                            common::IceHandler::addTurnServer(turnServer.value());
                        }
                    }
                    boost::asio::post(common::Worker::backgroundWorker(), [&socket]() {
                        try {
                            const auto createTransferSessionPayload =
                                    SenderFileHandler::generateCreateTransferSessionPayload(
                                        SenderConfig::paths,
                                        SenderConfig::maxReceivers
                                    );
                            SenderStateHolder::setManifest(createTransferSessionPayload);
                            socket.send(nlohmann::json(createTransferSessionPayload).dump());
                        } catch (const std::exception &e) {
                            spdlog::error("Failed to prepare transfer session: {}", e.what());
                            socket.close();
                        }
                    });
                } else if (type == "error_payload") {
                    const auto errorPayload = j.get<common::ErrorPayload>();
                    spdlog::error("Error: {}", errorPayload.message);
                } else if (type == "created_transfer_session_payload") {
                    const auto createdTransferPayload = j.get<common::CreatedTransferSessionPayload>();
                    SenderStateHolder::setJoinCode(createdTransferPayload.joinCode);
                    spdlog::info("Session created with join code: {}", createdTransferPayload.joinCode);
                } else if (type == "join_transfer_session_payload") {
                    const auto joinTransferSessionPayload = j.get<common::JoinTransferSessionPayload>();
                    SenderStateHolder::addReceiver(joinTransferSessionPayload.receiverId);
                    auto receiverInfo = SenderStateHolder::getReceiverInfo(joinTransferSessionPayload.receiverId);
                    spdlog::info("Incoming join request from {}, gathering local candidates...",
                                 joinTransferSessionPayload.receiverId);
                    boost::asio::post(common::Worker::backgroundWorker(),
                                      [&socket, &receiverInfo,payload = std::move(joinTransferSessionPayload)]() {
                                          auto &receiverId = payload.receiverId;
                                          const auto result = common::IceHandler::gatherLocalCandidates(
                                              true, receiverId,
                                              SenderConfig::totalConnections);
                                          spdlog::info(
                                              "Successfully gathered local ice candidates. {} candidates found.",
                                              result.serializedCandidates.size());

                                          common::IceHandler::establishConnection(
                                              true, receiverId,
                                              payload.candidatesResult,
                                              [receiverId](NiceAgent *agent, const bool success, guint streamId,
                                                           const int n) {
                                                  if (success) {
                                                      spdlog::info(
                                                          "ICE connection has been established! waiting 3s...");

                                                      auto *task = new DelayedTransferTask{
                                                          agent,
                                                          streamId,
                                                          n,
                                                          receiverId
                                                      };

                                                      GMainContext *my_context = common::IceHandler::getContext();

                                                      // Create the source for the timer
                                                      GSource *source = g_timeout_source_new_seconds(3);

                                                      // Set the callback (same function as before)
                                                      g_source_set_callback(
                                                          source, (GSourceFunc) execute_delayed_transfer, task, NULL);

                                                      // ATTACH it specifically to your IceHandler's context
                                                      g_source_attach(source, my_context);

                                                      // Clean up the source reference
                                                      g_source_unref(source);

                                                  } else {
                                                      spdlog::error("Failed to establish ICE connection.");
                                                  }
                                              });


                                          spdlog::info("Accepted join request from {}.",
                                                       receiverId);

                                          socket.send(nlohmann::json(common::AcceptTransferSessionPayload{
                                              .candidatesResult = std::move(result),
                                              .receiverId = std::move(receiverId),
                                          }).dump());
                                      });
                } else if (type == "quit_transfer_session_payload") {
                    const auto quitTransferSessionPayload = j.get<common::QuitTransferSessionPayload>();
                    auto &receiverId = quitTransferSessionPayload.receiverId;
                    spdlog::info("A receiver with id {} has left.", receiverId);
                    SenderStateHolder::removeReceiver(receiverId);

                    auto disposeTask = [receiverId = std::move(receiverId)]() {
                        SenderStream::disposeReceiverConnection(receiverId);
                        common::IceHandler::dispose(receiverId);
                    };

                    auto *disposeTaskPtr = new decltype(disposeTask)(std::move(disposeTask));

                    g_main_context_invoke_full(common::IceHandler::getContext(), G_PRIORITY_DEFAULT,
                                               [](gpointer data) -> gboolean {
                                                   auto *t = static_cast<decltype(disposeTask) *>(data);
                                                   (*t)();
                                                   return G_SOURCE_REMOVE;
                                               }, disposeTaskPtr, [](const gpointer data) {
                                                   delete static_cast<decltype(disposeTask) *>(data);
                                               });
                }
            } catch (const std::exception &e) {
                spdlog::error("Error occurred while handling socket message: {}", e.what());
                socket.close();
            }
        }
    };
}
