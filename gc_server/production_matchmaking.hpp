#pragma once
#include <vector>
#include <mutex>
#include <chrono>
#include <optional>
#include <string>
#include <map>
#include <thread>
#include <atomic>
#include "networking.hpp"

struct GameServer {
    std::string ip;
    uint16_t port;
    bool is_local;
    std::atomic<int> current_players{0};
    int max_players = 10;  // 5v5
    std::atomic<bool> available{true};
    std::string rcon_password;
    
    GameServer(const std::string& _ip, uint16_t _port, bool _local, const std::string& _rcon_pass = "")
        : ip(_ip), port(_port), is_local(_local), rcon_password(_rcon_pass) {}
};

struct QueuedPlayer {
    uint64_t steamid;
    SNetSocket_t socket;
    int32_t mmr;
    std::vector<int> preferred_maps;
    std::chrono::steady_clock::time_point queue_start;
    int max_ping;
    
    QueuedPlayer(uint64_t id, SNetSocket_t sock, int32_t rating)
        : steamid(id), socket(sock), mmr(rating), queue_start(std::chrono::steady_clock::now()), max_ping(150) {}
};

struct PendingMatch {
    std::string match_id;
    std::vector<uint64_t> players;
    GameServer* allocated_server = nullptr;
    std::map<uint64_t, bool> player_responses;  // steamid -> accepted
    std::chrono::steady_clock::time_point created_at;
    int responses_received = 0;
    bool all_accepted = false;
    
    PendingMatch(const std::string& id, const std::vector<uint64_t>& player_list)
        : match_id(id), players(player_list), created_at(std::chrono::steady_clock::now()) {
        for (uint64_t steamid : players) {
            player_responses[steamid] = false;
        }
    }
};

class ProductionMatchmaker {
public:
    static ProductionMatchmaker& Instance();
    
    void Initialize();
    void Shutdown();
    
    // Player queue management
    void EnqueuePlayer(uint64_t steamid, SNetSocket_t socket, int32_t mmr, const std::vector<int>& maps);
    void DequeuePlayer(uint64_t steamid);
    
    // Match management
    void ProcessMatchmakingTick();
    void HandleMatchResponse(uint64_t steamid, bool accepted);
    
    // Server management
    bool AllocateServer(PendingMatch* match);
    void ReleaseServer(GameServer* server);
    
    // Statistics
    int GetQueueSize() const { return static_cast<int>(m_playerQueue.size()); }
    int GetAvailableServers() const;
    int GetActiveMatches() const { return static_cast<int>(m_pendingMatches.size()); }
    
private:
    ProductionMatchmaker() = default;
    
    // Game servers (your 6 servers)
    std::vector<std::unique_ptr<GameServer>> m_gameServers;
    
    // Player queue
    std::vector<QueuedPlayer> m_playerQueue;
    mutable std::mutex m_queueMutex;
    
    // Pending matches waiting for acceptance
    std::map<std::string, std::unique_ptr<PendingMatch>> m_pendingMatches;
    std::mutex m_matchesMutex;
    
    // Worker thread
    std::thread m_workerThread;
    std::atomic<bool> m_running{false};
    
    // Helper methods
    bool TryCreateMatch();
    std::string GenerateMatchId();
    void SendMatchFoundToPlayers(PendingMatch* match);
    void StartMatch(PendingMatch* match);
    void CancelMatch(PendingMatch* match);
    void CleanupExpiredMatches();
    
    // RCON integration
    bool ConfigureServerForMatch(GameServer* server, const std::string& match_id);
    void SendRCONCommand(GameServer* server, const std::string& command);
};

// Global instance accessor
extern ProductionMatchmaker* g_productionMatchmaker;
