#pragma once
#include <IXWebSocket.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <spdlog/spdlog.h>
#include <latch>
#include "../common/Payloads.hpp"
#include "../common/Utils.hpp"
#include <boost/asio/post.hpp>
#include "../common/Worker.hpp"
#include "ReceiverConfig.hpp"
#include "ReceiverStream.hpp"

namespace receiver {
    class ReceiverSocketHandler {
    public:
        static void onConnect(ix::WebSocket &socket) {
            spdlog::info("Successfully connected to signaling server : {}", ReceiverConfig::serverUrl);
        }

        static void onClose(ix::WebSocket &socket, std::latch &clientDone, std::string_view reason) {
            spdlog::info("Disconnected from signaling server : {} Reason: {}", ReceiverConfig::serverUrl, reason);
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

                    spdlog::info("Gathering local ice candidates...");

                    boost::asio::post(common::Worker::backgroundWorker(), [&socket]() {
                                          auto result = common::IceHandler::gatherLocalCandidates(
                                              false, "", ReceiverConfig::totalConnections);
                                          // for (auto &candidate: result.serializedCandidates) {
                                          //     spdlog::info("Ice candidate gathered: {}", candidate.dump());
                                          // }
                                          spdlog::info(
                                              "Successfully gathered local ice candidates. {} candidates found.",
                                              result.serializedCandidates.size());
                                          spdlog::info("Now sending local ice candidates to the sender...");

                                          socket.send(nlohmann::json(common::JoinTransferSessionPayload{
                                              .candidatesResult = std::move(result),
                                              .joinCode = ReceiverConfig::joinCode,
                                          }).dump());
                                      }
                    );

                } else if (type == "error_payload") {
                    const auto errorPayload = j.get<common::ErrorPayload>();
                    spdlog::error("Error: {}", errorPayload.message);
                } else if (type == "accept_transfer_session_payload") {
                    const auto acceptedTransferSessionPayload = j.get<common::AcceptTransferSessionPayload>();
                    spdlog::info("Join request accepted from sender. Establishing ICE connection...");
                    boost::asio::post(common::Worker::backgroundWorker(),
                                      [payload = std::move(acceptedTransferSessionPayload) ]() {
                                          common::IceHandler::establishConnection(
                                              false, "",
                                              payload.candidatesResult,
                                              [](NiceAgent* agent, const bool success, const guint streamId, const int n) {
                                                  if (success) {
                                                      spdlog::info(
                                                          "ICE connection has been established!");
                                                      ReceiverStream::receiveTransfer(agent, streamId, common::IceHandler::getContext(),n);
                                                  }
                                                  else {
                                                      spdlog::error("Failed to establish ICE connection.");
                                                  }
                                              });
                                      });
                }


            } catch (const std::exception &e) {
                spdlog::error("Error occurred while handling socket message: {}", e.what());
                socket.close();
            }
        }
    };
}
