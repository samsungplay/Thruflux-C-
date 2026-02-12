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
)

add_executable(ThrufluxServer
        server/ServerEntryPoint.cpp
        server/ServerConfig.hpp
        server/ServerSocketHandler.hpp
        server/TransferSession.hpp
        server/TransferSessionStore.hpp
        server/SessionTracker.hpp
        ${COMMON_SOURCES}
)

add_executable(ThrufluxSender
        sender/SenderEntryPoint.cpp
        sender/SenderConfig.hpp
        sender/SenderSocketHandler.hpp
        ${COMMON_SOURCES}
        sender/SenderStream.hpp
)

add_executable(ThrufluxReceiver
        receiver/ReceiverConfig.hpp
        ${COMMON_SOURCES}
        receiver/ReceiverEntryPoint.cpp
        receiver/ReceiverSocketHandler.hpp
        receiver/ReceiverStream.hpp
        receiver/ReceiverContexts.hpp
)

set(SHARED_LIBS
        CLI11::CLI11
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        Boost::url
        Boost::boost
        OpenSSL::Crypto
        PkgConfig::NICE
)

set(SENDER_RECEIVER_SHARED_LIBS
        ixwebsocket::ixwebsocket
        lsquic::lsquic
)

target_link_libraries(ThrufluxServer PRIVATE ${SHARED_LIBS} unofficial::uwebsockets::uwebsockets)
target_link_libraries(ThrufluxSender PRIVATE ${SHARED_LIBS} ${SENDER_RECEIVER_SHARED_LIBS})
target_link_libraries(ThrufluxReceiver PRIVATE ${SHARED_LIBS} ${SENDER_RECEIVER_SHARED_LIBS})
