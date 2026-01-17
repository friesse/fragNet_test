#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

class TunablesManager {
public:
  static TunablesManager &GetInstance();

  // Initialize by reading from file
  void Init(const std::string &filename = "tunables.txt");

  // Reloads config from file
  void Reload();

  // Getters for specific features
  bool IsOperationActive() const;
  bool IsTournamentDraftEnabled() const;
  bool IsXPSpoofActive() const;
  std::string GetWebAPIUrl() const;

  // Generic getters
  bool GetBool(const std::string &key, bool defaultValue = false) const;
  int GetInt(const std::string &key, int defaultValue = 0) const;
  std::string GetString(const std::string &key,
                        const std::string &defaultValue = "") const;

private:
  TunablesManager() = default;
  ~TunablesManager() = default;

  // Disable copy/move
  TunablesManager(const TunablesManager &) = delete;
  TunablesManager &operator=(const TunablesManager &) = delete;

  void LoadFromFile();

  std::string m_filename;
  mutable std::mutex m_mutex;
  std::unordered_map<std::string, std::string> m_config;
};
