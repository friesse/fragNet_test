#pragma once
/**
 * db_pool.hpp - Thread-safe MySQL connection pool
 *
 * Provides RAII connection management with:
 * - Thread-safe connection checkout/return
 * - Automatic reconnection on failure
 * - Connection health checks
 */

#include "logger.hpp"
#include <chrono>
#include <condition_variable>
#include <mariadb/mysql.h>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>

class DBConnectionPool {
public:
  /**
   * RAII wrapper for pool connections.
   * Automatically returns connection to pool on destruction.
   */
  class Connection {
  public:
    Connection(DBConnectionPool *pool, MYSQL *conn)
        : m_pool(pool), m_conn(conn) {}

    ~Connection() {
      if (m_pool && m_conn) {
        m_pool->returnConnection(m_conn);
      }
    }

    // Move-only semantics
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    Connection(Connection &&other) noexcept
        : m_pool(other.m_pool), m_conn(other.m_conn) {
      other.m_pool = nullptr;
      other.m_conn = nullptr;
    }

    Connection &operator=(Connection &&other) noexcept {
      if (this != &other) {
        if (m_pool && m_conn) {
          m_pool->returnConnection(m_conn);
        }
        m_pool = other.m_pool;
        m_conn = other.m_conn;
        other.m_pool = nullptr;
        other.m_conn = nullptr;
      }
      return *this;
    }

    // Get raw connection pointer
    MYSQL *get() const { return m_conn; }
    MYSQL *operator->() const { return m_conn; }
    operator MYSQL *() const { return m_conn; }
    explicit operator bool() const { return m_conn != nullptr; }

  private:
    DBConnectionPool *m_pool;
    MYSQL *m_conn;
  };

  /**
   * Create a connection pool.
   * @param host Database host
   * @param user Database user
   * @param password Database password
   * @param database Database name
   * @param port Database port (default 3306)
   * @param poolSize Number of connections to maintain
   */
  DBConnectionPool(const std::string &host, const std::string &user,
                   const std::string &password, const std::string &database,
                   unsigned int port = 3306, size_t poolSize = 5)
      : m_host(host), m_user(user), m_password(password), m_database(database),
        m_port(port), m_poolSize(poolSize), m_shutdown(false) {
    // Pre-create connections
    for (size_t i = 0; i < poolSize; ++i) {
      MYSQL *conn = createConnection();
      if (conn) {
        m_available.push(conn);
      }
    }

    if (m_available.empty()) {
      throw std::runtime_error("Failed to create any database connections");
    }

    logger::info("DBConnectionPool: Created pool with %zu connections to %s",
                 m_available.size(), database.c_str());
  }

  ~DBConnectionPool() { shutdown(); }

  // Non-copyable
  DBConnectionPool(const DBConnectionPool &) = delete;
  DBConnectionPool &operator=(const DBConnectionPool &) = delete;

  /**
   * Get a connection from the pool.
   * Blocks if no connections available.
   * @param timeoutMs Maximum time to wait (0 = infinite)
   * @return RAII connection wrapper
   */
  Connection getConnection(uint32_t timeoutMs = 5000) {
    std::unique_lock<std::mutex> lock(m_poolMutex);

    // Wait for a connection to become available
    auto predicate = [this]() { return !m_available.empty() || m_shutdown; };

    if (timeoutMs > 0) {
      if (!m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                         predicate)) {
        logger::error("DBConnectionPool: Timeout waiting for connection");
        return Connection(nullptr, nullptr);
      }
    } else {
      m_cv.wait(lock, predicate);
    }

    if (m_shutdown || m_available.empty()) {
      return Connection(nullptr, nullptr);
    }

    MYSQL *conn = m_available.front();
    m_available.pop();

    // Check if connection is still alive
    if (mysql_ping(conn) != 0) {
      logger::warning("DBConnectionPool: Connection dead, reconnecting...");
      mysql_close(conn);
      conn = createConnection();
      if (!conn) {
        logger::error("DBConnectionPool: Failed to reconnect");
        return Connection(nullptr, nullptr);
      }
    }

    return Connection(this, conn);
  }

  /**
   * Get the number of available connections.
   */
  size_t availableCount() const {
    std::lock_guard<std::mutex> lock(m_poolMutex);
    return m_available.size();
  }

  /**
   * Shutdown the pool and close all connections.
   */
  void shutdown() {
    std::unique_lock<std::mutex> lock(m_poolMutex);
    m_shutdown = true;

    while (!m_available.empty()) {
      MYSQL *conn = m_available.front();
      m_available.pop();
      mysql_close(conn);
    }

    m_cv.notify_all();
  }

private:
  void returnConnection(MYSQL *conn) {
    std::lock_guard<std::mutex> lock(m_poolMutex);

    if (m_shutdown) {
      mysql_close(conn);
      return;
    }

    m_available.push(conn);
    m_cv.notify_one();
  }

  MYSQL *createConnection() {
    MYSQL *conn = mysql_init(nullptr);
    if (!conn) {
      logger::error("DBConnectionPool: mysql_init failed");
      return nullptr;
    }

    // Enable auto-reconnect
    my_bool reconnect = 1;
    mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);

    // Set connection timeout
    unsigned int timeout = 10;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    // Connect
    if (!mysql_real_connect(conn, m_host.c_str(), m_user.c_str(),
                            m_password.c_str(), m_database.c_str(), m_port,
                            nullptr, 0)) {
      logger::error("DBConnectionPool: Connection failed: %s",
                    mysql_error(conn));
      mysql_close(conn);
      return nullptr;
    }

    // Set UTF8
    mysql_set_character_set(conn, "utf8mb4");

    return conn;
  }

  std::string m_host;
  std::string m_user;
  std::string m_password;
  std::string m_database;
  unsigned int m_port;
  size_t m_poolSize;

  mutable std::mutex m_poolMutex;
  std::condition_variable m_cv;
  std::queue<MYSQL *> m_available;
  bool m_shutdown;
};
