#include "production_matchmaking.hpp"
#include "logger.hpp"
#include "networking_matchmaking.hpp"
#include <algorithm>
#include <random>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

ProductionMatchmaker* g_productionMatchmaker = nullptr;

ProductionMatchmaker& ProductionMatchmaker::Instance() {
    static ProductionMatchmaker instance;
    return instance;
}

void ProductionMatchmaker::Initialize() {
    logger::info("=== Initializing Production Matchmaking System ===");
    
    // Initialize your 6 game servers
    // 4 local servers
    m_gameServers.push_back(std::make_unique<GameServer>("127.0.0.1", 27015, true, "rcon_password_1"));
    m_gameServers.push_back(std::make_unique<GameServer>("127.0.0.1", 27016, true, "rcon_password_2"));
    m_gameServers.push_back(std::make_unique<GameServer>("127.0.0.1", 27017, true, "rcon_password_3"));
    m_gameServers.push_back(std::make_unique<GameServer>("127.0.0.1", 27018, true, "rcon_password_4"));
    
    // 2 remote servers (replace with your actual remote server IP)
    m_gameServers.push_back(std::make_unique<GameServer>("YOUR_REMOTE_IP", 27015, false, "remote_rcon_1"));
    m_gameServers.push_back(std::make_unique<GameServer>("YOUR_REMOTE_IP", 27016, false, "remote_rcon_2"));
    
    logger::info("Configured %zu game servers:", m_gameServers.size());
    for (const auto& server : m_gameServers) {
        logger::info("  - %s:%d (%s)", server->ip.c_str(), server->port, 
                    server->is_local ? "local" : "remote");
    }
    
    // Start worker thread
    m_running = true;
    m_workerThread = std::thread([this]() {
        logger::info("Matchmaking worker thread started");
        while (m_running) {
            ProcessMatchmakingTick();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));  // 2 Hz tick rate
        }
        logger::info("Matchmaking worker thread stopped");
    });
    
    g_productionMatchmaker = this;
    logger::info("Production Matchmaking System initialized successfully!");
}

void ProductionMatchmaker::Shutdown() {
    logger::info("Shutting down Production Matchmaking System...");
    m_running = false;
    
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    
    g_productionMatchmaker = nullptr;
    logger::info("Matchmaking system shutdown complete");
}

void ProductionMatchmaker::EnqueuePlayer(uint64_t steamid, SNetSocket_t socket, int32_t mmr, const std::vector<int>& maps) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    // Check if player is already in queue
    auto existing = std::find_if(m_playerQueue.begin(), m_playerQueue.end(),
        [steamid](const QueuedPlayer& p) { return p.steamid == steamid; });
    
    if (existing != m_playerQueue.end()) {
        logger::warning("Player %llu already in matchmaking queue", steamid);
        return;
    }
    
    // Add player to queue
    QueuedPlayer player(steamid, socket, mmr);
    player.preferred_maps = maps;
    m_playerQueue.push_back(player);
    
    logger::info("Player %llu added to matchmaking queue (MMR: %d, Queue size: %zu)", 
                steamid, mmr, m_playerQueue.size());
}

void ProductionMatchmaker::DequeuePlayer(uint64_t steamid) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    auto it = std::remove_if(m_playerQueue.begin(), m_playerQueue.end(),
        [steamid](const QueuedPlayer& p) { return p.steamid == steamid; });
    
    if (it != m_playerQueue.end()) {
        m_playerQueue.erase(it, m_playerQueue.end());
        logger::info("Player %llu removed from matchmaking queue", steamid);
    }
}

void ProductionMatchmaker::ProcessMatchmakingTick() {
    // Try to create matches
    bool created_match = TryCreateMatch();
    
    // Clean up expired matches
    CleanupExpiredMatches();
    
    // Log queue status every 30 seconds
    static auto last_status_log = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_status_log).count() >= 30) {
        logger::info("Matchmaking Status - Queue: %d players, Pending matches: %d, Available servers: %d",
                    GetQueueSize(), GetActiveMatches(), GetAvailableServers());
        last_status_log = now;
    }
}

bool ProductionMatchmaker::TryCreateMatch() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    if (m_playerQueue.size() < 10) {
        return false;  // Need 10 players for 5v5
    }
    
    // For your scale (50 players max), simple FIFO matching works great
    // Take first 10 players in queue
    std::vector<QueuedPlayer> match_players;
    for (int i = 0; i < 10 && i < static_cast<int>(m_playerQueue.size()); i++) {
        match_players.push_back(m_playerQueue[i]);
    }
    
    // Remove these players from queue
    m_playerQueue.erase(m_playerQueue.begin(), m_playerQueue.begin() + 10);
    
    // Create match
    std::string match_id = GenerateMatchId();
    std::vector<uint64_t> player_steamids;
    for (const auto& player : match_players) {
        player_steamids.push_back(player.steamid);
    }
    
    auto pending_match = std::make_unique<PendingMatch>(match_id, player_steamids);
    
    // Try to allocate a server
    if (!AllocateServer(pending_match.get())) {
        logger::error("No servers available for match %s! Putting players back in queue...", match_id.c_str());
        // Put players back in queue
        m_playerQueue.insert(m_playerQueue.begin(), match_players.begin(), match_players.end());
        return false;
    }
    
    logger::info("Created match %s with 10 players on server %s:%d", 
                match_id.c_str(), 
                pending_match->allocated_server->ip.c_str(),
                pending_match->allocated_server->port);
    
    // Send match found messages to all players
    SendMatchFoundToPlayers(pending_match.get());
    
    // Store pending match
    {
        std::lock_guard<std::mutex> match_lock(m_matchesMutex);
        m_pendingMatches[match_id] = std::move(pending_match);
    }
    
    return true;
}

bool ProductionMatchmaker::AllocateServer(PendingMatch* match) {
    // Prefer local servers first (better performance)
    for (auto& server : m_gameServers) {
        if (server->available.load() && server->is_local) {
            server->available = false;
            server->current_players = 10;
            match->allocated_server = server.get();
            
            // Configure server for this match
            ConfigureServerForMatch(server.get(), match->match_id);
            
            logger::info("Allocated local server %s:%d for match %s", 
                        server->ip.c_str(), server->port, match->match_id.c_str());
            return true;
        }
    }
    
    // Fall back to remote servers
    for (auto& server : m_gameServers) {
        if (server->available.load() && !server->is_local) {
            server->available = false;
            server->current_players = 10;
            match->allocated_server = server.get();
            
            ConfigureServerForMatch(server.get(), match->match_id);
            
            logger::info("Allocated remote server %s:%d for match %s", 
                        server->ip.c_str(), server->port, match->match_id.c_str());
            return true;
        }
    }
    
    return false;  // No servers available
}

void ProductionMatchmaker::ReleaseServer(GameServer* server) {
    if (!server) return;
    
    server->available = true;
    server->current_players = 0;
    
    // Reset server to default state
    SendRCONCommand(server, "mp_warmup_end");
    SendRCONCommand(server, "mp_restartgame 1");
    
    logger::info("Released server %s:%d", server->ip.c_str(), server->port);
}

void ProductionMatchmaker::SendMatchFoundToPlayers(PendingMatch* match) {
    logger::info("Sending match found messages to %zu players for match %s", 
                match->players.size(), match->match_id.c_str());
    
    for (uint64_t steamid : match->players) {
        // Send match found message via networking system
        GCNetwork_Matchmaking::SendMatchFound(steamid, match->allocated_server->ip, 
                                            match->allocated_server->port, match->match_id);
    }
}

void ProductionMatchmaker::HandleMatchResponse(uint64_t steamid, bool accepted) {
    std::lock_guard<std::mutex> lock(m_matchesMutex);
    
    // Find which match this player belongs to
    PendingMatch* player_match = nullptr;
    for (auto& [match_id, match] : m_pendingMatches) {
        if (std::find(match->players.begin(), match->players.end(), steamid) != match->players.end()) {
            player_match = match.get();
            break;
        }
    }
    
    if (!player_match) {
        logger::warning("Received match response from player %llu not in any pending match", steamid);
        return;
    }
    
    logger::info("Player %llu %s match %s", steamid, accepted ? "ACCEPTED" : "DECLINED", 
                player_match->match_id.c_str());
    
    player_match->player_responses[steamid] = accepted;
    player_match->responses_received++;
    
    // If anyone declined, cancel the match
    if (!accepted) {
        logger::info("Match %s cancelled - player declined", player_match->match_id.c_str());
        CancelMatch(player_match);
        return;
    }
    
    // Check if all players have accepted
    if (player_match->responses_received == static_cast<int>(player_match->players.size())) {
        bool all_accepted = true;
        for (const auto& [player_id, response] : player_match->player_responses) {
            if (!response) {
                all_accepted = false;
                break;
            }
        }
        
        if (all_accepted) {
            logger::info("All players accepted match %s! Starting match...", player_match->match_id.c_str());
            StartMatch(player_match);
        }
    }
}

void ProductionMatchmaker::StartMatch(PendingMatch* match) {
    logger::info("Starting match %s on server %s:%d", 
                match->match_id.c_str(),
                match->allocated_server->ip.c_str(), 
                match->allocated_server->port);
    
    // Configure server for competitive match
    GameServer* server = match->allocated_server;
    
    // Set up competitive config
    SendRCONCommand(server, "exec gamemode_competitive");
    SendRCONCommand(server, "mp_warmup_start");
    SendRCONCommand(server, "mp_warmuptime 60");
    
    // Set random map (for now, use dust2)
    SendRCONCommand(server, "changelevel de_dust2");
    
    // Send final connect info to all players
    for (uint64_t steamid : match->players) {
        GCNetwork_Matchmaking::SendMatchReady(steamid, server->ip, server->port, "");
    }
    
    // Remove from pending matches
    {
        std::lock_guard<std::mutex> lock(m_matchesMutex);
        m_pendingMatches.erase(match->match_id);
    }
    
    logger::info("Match %s started successfully!", match->match_id.c_str());
}

void ProductionMatchmaker::CancelMatch(PendingMatch* match) {
    logger::info("Cancelling match %s", match->match_id.c_str());
    
    // Release the server
    ReleaseServer(match->allocated_server);
    
    // Notify players that match was cancelled
    for (uint64_t steamid : match->players) {
        GCNetwork_Matchmaking::SendMatchCancelled(steamid);
        
        // Re-queue players who accepted
        if (match->player_responses[steamid]) {
            // TODO: Re-add to queue with priority
        }
    }
    
    // Remove from pending matches
    {
        std::lock_guard<std::mutex> lock(m_matchesMutex);
        m_pendingMatches.erase(match->match_id);
    }
}

void ProductionMatchmaker::CleanupExpiredMatches() {
    std::lock_guard<std::mutex> lock(m_matchesMutex);
    
    auto now = std::chrono::steady_clock::now();
    auto it = m_pendingMatches.begin();
    
    while (it != m_pendingMatches.end()) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second->created_at);
        
        // Expire matches after 30 seconds
        if (age.count() > 30) {
            logger::info("Match %s expired (age: %lld seconds)", it->second->match_id.c_str(), age.count());
            CancelMatch(it->second.get());
            it = m_pendingMatches.erase(it);
        } else {
            ++it;
        }
    }
}

std::string ProductionMatchmaker::GenerateMatchId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    
    std::stringstream ss;
    ss << "match_";
    for (int i = 0; i < 8; i++) {
        ss << std::hex << dis(gen);
    }
    return ss.str();
}

int ProductionMatchmaker::GetAvailableServers() const {
    int count = 0;
    for (const auto& server : m_gameServers) {
        if (server->available.load()) {
            count++;
        }
    }
    return count;
}

bool ProductionMatchmaker::ConfigureServerForMatch(GameServer* server, const std::string& match_id) {
    logger::info("Configuring server %s:%d for match %s", server->ip.c_str(), server->port, match_id.c_str());
    
    // Basic server setup for competitive match
    SendRCONCommand(server, "mp_restartgame 1");
    SendRCONCommand(server, "mp_warmup_start");
    SendRCONCommand(server, "sv_cheats 0");
    SendRCONCommand(server, "mp_limitteams 1");
    SendRCONCommand(server, "mp_autoteambalance 0");
    
    return true;
}

void ProductionMatchmaker::SendRCONCommand(GameServer* server, const std::string& command) {
    // TODO: Implement actual RCON communication
    // For now, just log what we would send
    logger::info("RCON [%s:%d]: %s", server->ip.c_str(), server->port, command.c_str());
    
    // This would typically use:
    // 1. Socket connection to server's RCON port
    // 2. RCON authentication with password
    // 3. Send command packet
    // 4. Receive response
}
