#include "ReceiverConfig.hpp"
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>
#include <spdlog/spdlog.h>
#include "../common/Utils.hpp"
#include <latch>
#include <boost/algorithm/string.hpp>

#include "ReceiverSocketHandler.hpp"

int main(const int argc, char **argv) {
    receiver::ReceiverConfig::initialize(argc, argv);

    //Start UI Loop
    boost::asio::io_context uiIo;
    auto worker = boost::asio::make_work_guard(uiIo);
    auto benchmarker = std::make_shared<common::Benchmarker>(uiIo, false);
    benchmarker->initialize();
    boost::asio::post(common::Worker::uiWorker(), [&uiIo] {
        uiIo.run();
    });

    common::IceHandler::initialize();

    std::vector<std::string> rawStunUrls;
    boost::split(rawStunUrls, receiver::ReceiverConfig::stunServers, boost::is_any_of(","), boost::token_compress_on);

    for (const auto &rawStunUrl: rawStunUrls) {
        if (auto stunServer = common::Utils::toStunServer(rawStunUrl); stunServer.has_value()) {
            common::IceHandler::addStunServer(stunServer.value());
        }
    }

    std::vector<std::string> rawTurnUrls;
    boost::split(rawTurnUrls, receiver::ReceiverConfig::turnServers, boost::is_any_of(","), boost::token_compress_on);

    for (const auto &rawTurnUrl: rawTurnUrls) {
        if (auto turnServer = common::Utils::toTurnServer(rawTurnUrl); turnServer.has_value()) {
            common::IceHandler::addTurnServer(turnServer.value());
        }
    }


    ix::initNetSystem();

    receiver::ReceiverStream::initialize();
    ix::WebSocket socketClient;
    socketClient.disableAutomaticReconnection();
    std::latch clientDone{1};


    socketClient.setUrl(common::Utils::toWebSocketURL(receiver::ReceiverConfig::serverUrl));
    ix::WebSocketHttpHeaders headers;
    headers["x-role"] = "receiver";
    headers["x-id"] = common::Utils::generateNanoId();
    socketClient.setExtraHeaders(headers);
    socketClient.setPingInterval(30);

    socketClient.setOnMessageCallback([&socketClient, &clientDone](const ix::WebSocketMessagePtr &msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            receiver::ReceiverSocketHandler::onConnect(socketClient);
        } else if (msg->type == ix::WebSocketMessageType::Message) {
            receiver::ReceiverSocketHandler::onMessage(socketClient, msg->str);
        } else if (msg->type == ix::WebSocketMessageType::Close) {
            receiver::ReceiverSocketHandler::onClose(socketClient, clientDone, msg->closeInfo.reason);
        }
    });

    spdlog::info("Connecting to signaling server at {}...", receiver::ReceiverConfig::serverUrl);

    socketClient.start();

    clientDone.wait();

    socketClient.stop();

    common::IceHandler::destroy();

    receiver::ReceiverStream::dispose();

    ix::uninitNetSystem();
}
