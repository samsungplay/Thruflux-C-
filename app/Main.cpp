
#include "../server/ServerEntryPoint.hpp"
#include "../sender/SenderEntryPoint.hpp"
#include "../receiver/ReceiverEntryPoint.hpp"

static void forceUtf8Locale() {
    if (std::setlocale(LC_ALL, "") != nullptr) return;

    if (std::setlocale(LC_ALL, "C.UTF-8") != nullptr) {
        setenv("LANG", "C.UTF-8", 1);
        setenv("LC_ALL", "C.UTF-8", 1);
        return;
    }
    if (std::setlocale(LC_ALL, "en_US.UTF-8") != nullptr ||
        std::setlocale(LC_ALL, "en_US.utf8") != nullptr) {
        setenv("LANG", "en_US.UTF-8", 1);
        setenv("LC_ALL", "en_US.UTF-8", 1);
        return;
        }

    std::setlocale(LC_ALL, "C");
    setenv("LANG", "C", 1);
    setenv("LC_ALL", "C", 1);
}

int main(const int argc, char **argv) {
    forceUtf8Locale();
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
