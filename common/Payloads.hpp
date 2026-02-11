#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include <vector>

#include "IceHandler.hpp"

namespace common {

    struct QuitTransferSessionPayload {
        std::string type = "quit_transfer_session_payload";
        std::string receiverId;
    };

    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(QuitTransferSessionPayload, type, receiverId);

    struct AcceptTransferSessionPayload {
        std::string type = "accept_transfer_session_payload";
        CandidatesResult candidatesResult;
        std::string receiverId;
    };

    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AcceptTransferSessionPayload, type, candidatesResult, receiverId);

    struct JoinTransferSessionPayload {
        std::string type = "join_transfer_session_payload";
        CandidatesResult candidatesResult;
        std::string joinCode;
        std::string receiverId;
    };

    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(JoinTransferSessionPayload, type, candidatesResult, joinCode, receiverId);

    struct ErrorPayload {
        std::string type = "error_payload";
        std::string message;
    };

    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ErrorPayload, type, message);

    struct TurnCredentialsPayload {
        std::string type = "turn_credentials_payload";
        std::string username;
        std::string password;
        std::string turnUrl;
    };

    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TurnCredentialsPayload, type, username, password, turnUrl);

    struct CreateTransferSessionPayload {
        std::string type = "create_transfer_session_payload";
        int maxReceivers = 0;
        std::uint64_t totalSize;
        int filesCount;
    };

    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CreateTransferSessionPayload, type, maxReceivers, totalSize, filesCount);

    struct CreatedTransferSessionPayload {
        std::string type = "created_transfer_session_payload";
        std::string joinCode;
    };

    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CreatedTransferSessionPayload, type, joinCode);
}
