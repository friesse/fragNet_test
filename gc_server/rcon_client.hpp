#pragma once
#include <string>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET socket_t;
#else
#include <sys/socket.h>
typedef int socket_t;
#endif

enum class RCONPacketType : int32_t {
    AUTH = 3,
    EXECCOMMAND = 2,
    AUTH_RESPONSE = 2,
    RESPONSE_VALUE = 0
};

struct RCONPacket {
    int32_t size;
    int32_t id;
    int32_t type;
    std::string body;
    
    std::vector<uint8_t> Serialize() const;
    bool Deserialize(const std::vector<uint8_t>& data);
};

class RCONClient {
public:
    RCONClient(const std::string& host, uint16_t port, const std::string& password);
    ~RCONClient();
    
    bool Connect();
    void Disconnect();
    bool IsConnected() const { return m_connected; }
    
    // Execute RCON command and return response
    std::string ExecuteCommand(const std::string& command);
    
    // Async command execution (fire and forget)
    bool ExecuteCommandAsync(const std::string& command);
    
private:
    std::string m_host;
    uint16_t m_port;
    std::string m_password;
    socket_t m_socket;
    bool m_connected;
    bool m_authenticated;
    int32_t m_next_id;
    
    bool Authenticate();
    bool SendPacket(const RCONPacket& packet);
    RCONPacket ReceivePacket();
    int32_t GenerateId() { return ++m_next_id; }
};
