cmake_minimum_required(VERSION 4.1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(Thruflux_C__)

find_package(CLI11 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-uwebsockets CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ixwebsocket CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS url)
find_package(PkgConfig REQUIRED)
find_package(lsquic CONFIG REQUIRED)
find_package(indicators CONFIG REQUIRED)
find_package(blake3 CONFIG REQUIRED)
pkg_check_modules(NICE REQUIRED IMPORTED_TARGET nice)

set(COMMON_SOURCES
        common/Utils.hpp
        common/Payloads.hpp
        common/Types.hpp
        common/TTLCache.hpp
        common/IceHandler.hpp
        common/ThreadManager.hpp
        common/Contexts.hpp
        common/Stream.hpp
        app/Main.cpp
)

add_executable(thru
        app/Main.cpp

        server/ServerConfig.hpp
        server/ServerSocketHandler.hpp
        server/TransferSession.hpp
        server/TransferSessionStore.hpp
        server/SessionTracker.hpp
        server/ServerEntryPoint.hpp

        sender/SenderConfig.hpp
        sender/SenderSocketHandler.hpp
        sender/SenderStream.hpp
        sender/SenderEntryPoint.hpp

        receiver/ReceiverConfig.hpp
        receiver/ReceiverSocketHandler.hpp
        receiver/ReceiverStream.hpp
        receiver/ReceiverContexts.hpp
        receiver/ReceiverEntryPoint.hpp

        ${COMMON_SOURCES}
)

target_link_libraries(thru PRIVATE
        CLI11::CLI11
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        Boost::url
        Boost::boost
        OpenSSL::Crypto
        PkgConfig::NICE
        indicators::indicators
        ixwebsocket::ixwebsocket
        lsquic::lsquic
        BLAKE3::blake3
        unofficial::uwebsockets::uwebsockets
)