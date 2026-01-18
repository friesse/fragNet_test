#include "tunables_manager.hpp"
#include "logger.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

TunablesManager &TunablesManager::GetInstance() {
  static TunablesManager instance;
  return instance;
}

void TunablesManager::Init(const std::string &filename) {
  m_filename = filename;
  LoadFromFile();
}

void TunablesManager::Reload() { LoadFromFile(); }

// Helpers to trim string
static std::string trim(const std::string &str) {
  size_t first = str.find_first_not_of(" \t\r\n");
  if (std::string::npos == first)
    return str;
  size_t last = str.find_last_not_of(" \t\r\n");
  return str.substr(first, (last - first + 1));
}

void TunablesManager::LoadFromFile() {
  std::ifstream file(m_filename);
  if (!file.is_open()) {
    logger::warning("TunablesManager: Could not open %s, using defaults.",
                    m_filename.c_str());
    return;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  m_config.clear();

  std::string line;
  while (std::getline(file, line)) {
    // Remove comments
    size_t commentPos = line.find('#');
    if (commentPos != std::string::npos) {
      line = line.substr(0, commentPos);
    }

    line = trim(line);
    if (line.empty())
      continue;

    size_t delimiterPos = line.find('=');
    if (delimiterPos != std::string::npos) {
      std::string key = trim(line.substr(0, delimiterPos));
      std::string value = trim(line.substr(delimiterPos + 1));
      m_config[key] = value;
      logger::info("TunablesManager: Loaded %s = %s", key.c_str(),
                   value.c_str());
    }
  }
}

// Feature specific getters
bool TunablesManager::IsOperationActive() const {
  return GetBool("operation_active", false); // Default false if not found
}

bool TunablesManager::IsTournamentDraftEnabled() const {
  return GetBool("tournament_draft", false);
}

bool TunablesManager::IsXPSpoofActive() const {
  return GetBool("xp_spoof", false);
}

std::string TunablesManager::GetWebAPIUrl() const {
  return GetString("web_api_url", "https://fragmount.net");
}

bool TunablesManager::IsOptimized() const { return GetBool("optimise", true); }

int TunablesManager::GetCacheSizeMB() const {
  int val = GetInt("cache_size_mb", 512);
  if (val < 1)
    return 1;
  if (val > 2048)
    return 2048; // Max 2GB
  return val;
}

// Generic getters
bool TunablesManager::GetBool(const std::string &key, bool defaultValue) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_config.find(key);
  if (it != m_config.end()) {
    std::string val = it->second;
    std::transform(val.begin(), val.end(), val.begin(), ::tolower);
    return val == "true" || val == "1" || val == "yes";
  }
  return defaultValue;
}

int TunablesManager::GetInt(const std::string &key, int defaultValue) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_config.find(key);
  if (it != m_config.end()) {
    try {
      return std::stoi(it->second);
    } catch (...) {
      return defaultValue;
    }
  }
  return defaultValue;
}

std::string TunablesManager::GetString(const std::string &key,
                                       const std::string &defaultValue) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_config.find(key);
  if (it != m_config.end()) {
    return it->second;
  }
  return defaultValue;
}
