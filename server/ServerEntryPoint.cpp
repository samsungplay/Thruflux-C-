#include <uwebsockets/App.h>

#include "ServerConfig.hpp"
#include "ServerSocketHandler.hpp"
#include "TransferSessionStore.hpp"
#include "../common/Types.hpp"


int main(const int argc, char **argv) {
    server::ServerConfig::initialize(argc, argv);


    server::TransferSessionStore::instance();

    auto *loop = reinterpret_cast<struct us_loop_t *>(uWS::Loop::get());

    us_timer_t *timer = us_create_timer(loop, 0, 0);
    us_timer_set(timer, [](struct us_timer_t *t) {
        server::TransferSessionStore::instance().cleanExpiredSessions();
    }, 5000, 5000);

    uWS::App().ws<common::SocketUserData>("/ws", {
                                              .idleTimeout = server::ServerConfig::wsIdleTimeout,
                                              .upgrade = [](auto *res, auto *req, auto *context) {
                                                  auto role = std::string(req->getHeader("x-role"));
                                                  auto id = std::string(req->getHeader("x-id"));

                                                  res->template upgrade<common::SocketUserData>(
                                                      common::SocketUserData{
                                                          .id = std::move(id),
                                                          .role = std::move(role)
                                                      },
                                                      req->getHeader("sec-websocket-key"),
                                                      req->getHeader("sec-websocket-protocol"),
                                                      req->getHeader("sec-websocket-extensions"),
                                                      context
                                                  );
                                              },
                                              .open = [](auto *ws) {
                                                  server::ServerSocketHandler::onConnect(ws);
                                              },
                                              .message = [](auto *ws, std::string_view message, uWS::OpCode op) {
                                                  if (op != uWS::OpCode::TEXT) return;
                                                  server::ServerSocketHandler::onMessage(ws, message);
                                              },
                                              .close = [](auto *ws, int code, std::string_view message) {
                                                  server::ServerSocketHandler::onClose(ws, message);
                                              }
                                          })
            .listen(server::ServerConfig::port, [](const us_listen_socket_t *socket) {
                if (socket) {
                    spdlog::info("Server successfully started on port {}", server::ServerConfig::port);
                } else {
                    spdlog::info("Server failed to start on port {}", server::ServerConfig::port);
                }
            })
            .run();
}
