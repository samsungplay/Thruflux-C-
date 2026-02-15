
#include "../server/ServerEntryPoint.hpp"
#include "../sender/SenderEntryPoint.hpp"
#include "../receiver/ReceiverEntryPoint.hpp"

int main(const int argc, char **argv) {
    CLI::App app{"Thruflux"};
    app.require_subcommand(1);
    CLI::App* host = app.add_subcommand("host", "Share files with other multiple receivers");
    CLI::App* join = app.add_subcommand("join", "Receive files from a host");
    CLI::App* server = app.add_subcommand("server", "Start a thruflux signaling server");
    server::ServerConfig::initialize(server);
    sender::SenderConfig::initialize(host);
    receiver::ReceiverConfig::initialize(join);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        return app.exit(e);
    }

    if (app.got_subcommand(host)) {
        sender::run(argc, argv);
    }
    else if (app.got_subcommand(join)) {
        receiver::run(argc, argv);
    }
    else if (app.got_subcommand(server)) {
        server::run(argc, argv);
    }

    return 0;
}
