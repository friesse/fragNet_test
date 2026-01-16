#ifndef NETWORKING_H
#define NETWORKING_H

#include "steam/steam_api.h"
#include <cstdint>
#include <mariadb/mysql.h>
#include <string>
#include <vector>

#include <ctime> // time_t
#include <memory>
#include <shared_mutex>
#include <unordered_map>

#include "db_pool.hpp"
#include "networking_users.hpp"

constexpr int NetMessageSendFlags = 8; // k_nSteamNetworkingSend_Reliable
constexpr int NetMessageChannel = 7;

class ClientSessions {
public:
  CSteamID steamID;
  SNetSocket_t socket;

  bool isAuthenticated;
  time_t lastActivity;
  uint64_t lastCheckedItemId;
  bool itemIdInitialized;

  ClientSessions(CSteamID id)
      : steamID(id), isAuthenticated(false), lastCheckedItemId(0),
        itemIdInitialized(false), socket(k_HSteamNetConnection_Invalid) {
    time(&lastActivity);
  }

  void updateActivity() { time(&lastActivity); }
};

class GCNetwork {
private:
  // STEAM_CALLBACK(GCNetwork, SocketStatusCallback, SocketStatusCallback_t,
  // m_SocketStatusCallback);
  CCallbackManual<GCNetwork, SocketStatusCallback_t> m_SocketStatusCallback;
  void SocketStatusCallback(SocketStatusCallback_t *pParam);

  // client sessions - protected by m_sessionsMutex
  mutable std::shared_mutex m_sessionsMutex;
  std::unordered_map<uint64_t, ClientSessions> m_activeSessions;
  std::unordered_map<SNetSocket_t, uint64_t>
      m_socketToSteamId; // O(1) reverse lookup
  uint64_t GetSessionSteamId(SNetSocket_t socket);

  // Database connection pools (#6 fix)
  std::shared_ptr<DBConnectionPool> m_classicPool;   // classiccounter
  std::shared_ptr<DBConnectionPool> m_inventoryPool; // ollum_inventory
  std::shared_ptr<DBConnectionPool> m_rankedPool;    // ollum_ranked

  // Legacy raw pointers for backward compatibility (will be phased out)
  MYSQL *m_mysql1; // classiccounter - legacy
  MYSQL *m_mysql2; // inventory - legacy
  MYSQL *m_mysql3; // ranked - legacy

  // matchmaking
  class MatchmakingManager *m_matchmakingManager;

  // whitelist - DISABLED (all Steam-authenticated users allowed)
  // bool m_maintenanceMode = false;
  // std::vector<uint64_t> m_maintenanceAllowlist = {};
  // bool IsUserWhitelisted(uint64_t steamID64, MYSQL* classiccounter_db);

public:
  GCNetwork();
  ~GCNetwork();
  void Init(const char *bind_ip = "0.0.0.0", uint16 port = 21818);
  void Update();

  void ReadAuthTicket(SNetSocket_t p2psocket, void *message, uint32 msgsize,
                      MYSQL *classiccounter_db, MYSQL *inventory_db,
                      MYSQL *ranked_db);

  // db methods
  bool InitDatabases();
  bool ExecuteQuery(MYSQL *connection, const char *query);
  void CloseDatabases();

  // Connection pool accessors (new API)
  DBConnectionPool::Connection GetClassicConnection() {
    return m_classicPool->getConnection();
  }
  DBConnectionPool::Connection GetInventoryConnection() {
    return m_inventoryPool->getConnection();
  }
  DBConnectionPool::Connection GetRankedConnection() {
    return m_rankedPool->getConnection();
  }

  // client sessions
  void CleanupSessions();
  void CheckNewItemsForActiveSessions();

  // Singleton access
  static GCNetwork *GetInstance();
  SNetSocket_t GetSocketForSteamId(uint64_t steamId);

private:
  static GCNetwork *s_pInstance;
};

#endif