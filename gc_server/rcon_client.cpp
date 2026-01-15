#include "rcon_client.hpp"
#include "logger.hpp"
#include <cstring>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <ws2tcpip.h>
#define CLOSE_SOCKET closesocket
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define CLOSE_SOCKET close
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#endif

std::vector<uint8_t> RCONPacket::Serialize() const {
    std::vector<uint8_t> data;
    
    // Calculate total size (excluding the size field itself)
    int32_t total_size = 4 + 4 + body.length() + 2;  // id + type + body + 2 null terminators
    
    // Size (little endian)
    data.push_back(total_size & 0xFF);
    data.push_back((total_size >> 8) & 0xFF);
    data.push_back((total_size >> 16) & 0xFF);
    data.push_back((total_size >> 24) & 0xFF);
    
    // ID (little endian)
    data.push_back(id & 0xFF);
    data.push_back((id >> 8) & 0xFF);
    data.push_back((id >> 16) & 0xFF);
    data.push_back((id >> 24) & 0xFF);
    
    // Type (little endian)
    data.push_back(type & 0xFF);
    data.push_back((type >> 8) & 0xFF);
    data.push_back((type >> 16) & 0xFF);
    data.push_back((type >> 24) & 0xFF);
    
    // Body + null terminator
    for (char c : body) {
        data.push_back(c);
    }
    data.push_back(0);
    
    // Additional null terminator
    data.push_back(0);
    
    return data;
}

RCONClient::RCONClient(const std::string& host, uint16_t port, const std::string& password)
    : m_host(host), m_port(port), m_password(password), m_socket(INVALID_SOCKET), 
      m_connected(false), m_authenticated(false), m_next_id(1000) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

RCONClient::~RCONClient() {
    Disconnect();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool RCONClient::Connect() {
    if (m_connected) {
        return true;
    }
    
    logger::info("Connecting to RCON server %s:%d", m_host.c_str(), m_port);
    
    // Create socket
    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket == INVALID_SOCKET) {
        logger::error("Failed to create RCON socket");
        return false;
    }
    
    // Set up server address
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(m_port);
    
#ifdef _WIN32
    if (inet_pton(AF_INET, m_host.c_str(), &server_addr.sin_addr) != 1) {
#else
    if (inet_aton(m_host.c_str(), &server_addr.sin_addr) == 0) {
#endif
        logger::error("Invalid RCON server IP address: %s", m_host.c_str());
        CLOSE_SOCKET(m_socket);
        m_socket = INVALID_SOCKET;
        return false;
    }
    
    // Connect to server
    if (connect(m_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        logger::error("Failed to connect to RCON server %s:%d", m_host.c_str(), m_port);
        CLOSE_SOCKET(m_socket);
        m_socket = INVALID_SOCKET;
        return false;
    }
    
    m_connected = true;
    logger::info("Connected to RCON server successfully");
    
    // Authenticate
    if (!Authenticate()) {
        logger::error("RCON authentication failed");
        Disconnect();
        return false;
    }
    
    logger::info("RCON authentication successful");
    return true;
}

void RCONClient::Disconnect() {
    if (m_socket != INVALID_SOCKET) {
        CLOSE_SOCKET(m_socket);
        m_socket = INVALID_SOCKET;
    }
    m_connected = false;
    m_authenticated = false;
}

bool RCONClient::Authenticate() {
    RCONPacket auth_packet;
    auth_packet.id = GenerateId();
    auth_packet.type = static_cast<int32_t>(RCONPacketType::AUTH);
    auth_packet.body = m_password;
    
    if (!SendPacket(auth_packet)) {
        logger::error("Failed to send RCON auth packet");
        return false;
    }
    
    // Receive auth response
    RCONPacket response = ReceivePacket();
    if (response.id == auth_packet.id && response.type == static_cast<int32_t>(RCONPacketType::AUTH_RESPONSE)) {
        m_authenticated = true;
        return true;
    }
    
    logger::error("RCON authentication failed - invalid response");
    return false;
}

std::string RCONClient::ExecuteCommand(const std::string& command) {
    if (!m_connected || !m_authenticated) {
        logger::error("RCON not connected or authenticated");
        return "";
    }
    
    RCONPacket cmd_packet;
    cmd_packet.id = GenerateId();
    cmd_packet.type = static_cast<int32_t>(RCONPacketType::EXECCOMMAND);
    cmd_packet.body = command;
    
    if (!SendPacket(cmd_packet)) {
        logger::error("Failed to send RCON command: %s", command.c_str());
        return "";
    }
    
    // Receive response
    RCONPacket response = ReceivePacket();
    if (response.id == cmd_packet.id) {
        return response.body;
    }
    
    logger::error("RCON command response ID mismatch");
    return "";
}

bool RCONClient::ExecuteCommandAsync(const std::string& command) {
    if (!m_connected || !m_authenticated) {
        if (!Connect()) {
            return false;
        }
    }
    
    RCONPacket cmd_packet;
    cmd_packet.id = GenerateId();
    cmd_packet.type = static_cast<int32_t>(RCONPacketType::EXECCOMMAND);
    cmd_packet.body = command;
    
    return SendPacket(cmd_packet);
}

bool RCONClient::SendPacket(const RCONPacket& packet) {
    if (m_socket == INVALID_SOCKET) {
        return false;
    }
    
    std::vector<uint8_t> data = packet.Serialize();
    size_t bytes_sent = 0;
    
    while (bytes_sent < data.size()) {
        int result = send(m_socket, reinterpret_cast<const char*>(data.data() + bytes_sent), 
                         static_cast<int>(data.size() - bytes_sent), 0);
        if (result == SOCKET_ERROR) {
            logger::error("RCON send failed");
            return false;
        }
        bytes_sent += result;
    }
    
    return true;
}

RCONPacket RCONClient::ReceivePacket() {
    RCONPacket packet{};
    
    if (m_socket == INVALID_SOCKET) {
        return packet;
    }
    
    // Read packet size first
    uint8_t size_bytes[4];
    if (recv(m_socket, reinterpret_cast<char*>(size_bytes), 4, 0) != 4) {
        logger::error("Failed to receive RCON packet size");
        return packet;
    }
    
    int32_t packet_size = size_bytes[0] | (size_bytes[1] << 8) | (size_bytes[2] << 16) | (size_bytes[3] << 24);
    
    // Read the rest of the packet
    std::vector<uint8_t> packet_data(packet_size);
    size_t bytes_received = 0;
    
    while (bytes_received < packet_size) {
        int result = recv(m_socket, reinterpret_cast<char*>(packet_data.data() + bytes_received), 
                         static_cast<int>(packet_size - bytes_received), 0);
        if (result <= 0) {
            logger::error("Failed to receive RCON packet data");
            return packet;
        }
        bytes_received += result;
    }
    
    // Parse packet data
    if (packet_size >= 8) {
        packet.id = packet_data[0] | (packet_data[1] << 8) | (packet_data[2] << 16) | (packet_data[3] << 24);
        packet.type = packet_data[4] | (packet_data[5] << 8) | (packet_data[6] << 16) | (packet_data[7] << 24);
        
        if (packet_size > 10) {  // Has body
            packet.body = std::string(reinterpret_cast<char*>(packet_data.data() + 8), packet_size - 10);
        }
    }
    
    return packet;
}
