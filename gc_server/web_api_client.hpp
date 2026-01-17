#pragma once

#include "steam/isteamhttp.h"
#include "steam/steam_gameserver.h"
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>


// Callback type: success, response body
typedef std::function<void(bool, const std::string &)> WebRequestCallback;

struct AlertInfo {
  uint64_t steamId;
  std::string type; // "cooldown" or "alert"
  std::string message;
  int duration; // seconds (for cooldown)
  int reason;   // for cooldown
};

struct TournamentState {
  bool active;
  uint32_t phase;          // 0=None, 1=Veto, 2=Pick
  int teamA;               // Terrorist
  int teamB;               // CT
  std::vector<int> drafts; // Map IDs
};

class WebAPIClient {
public:
  static WebAPIClient &GetInstance();

  void Init();
  void Update(); // Call frequently to poll

  // Config getters
  std::vector<AlertInfo> GetAlertsForUser(uint64_t steamId);
  TournamentState GetTournamentState();

private:
  WebAPIClient();
  ~WebAPIClient() = default;

  void PollAlerts();
  void PollTournament();

  // Generic HTTP get
  void FetchJSON(const std::string &url, WebRequestCallback callback);

  // Steam HTTP Callbacks
  void OnHTTPRequestCompleted(HTTPRequestCompleted_t *pResult, bool bIOFailure);
  CCallResult<WebAPIClient, HTTPRequestCompleted_t>
      m_SteamCallResultHTTPRequestCompleted;

  // State
  std::map<HTTPRequestHandle, WebRequestCallback> m_pendingRequests;
  std::mutex m_requestMutex;

  // Polling timers
  uint32_t m_lastAlertPoll;
  uint32_t m_lastTournamentPoll;

  // Cache
  std::map<uint64_t, std::vector<AlertInfo>> m_cachedAlerts;
  TournamentState m_cachedTournamentState;
  std::mutex m_dataMutex;
};
