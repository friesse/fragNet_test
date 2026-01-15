#pragma once

#include "logger.hpp"
#include <mariadb/mysql.h>

/**
 * @brief RAII wrapper for MySQL transactions.
 *
 * Automatically executes "START TRANSACTION" on construction.
 * If Commit() is not called before destruction, it automatically executes
 * "ROLLBACK".
 */
class SQLTransaction {
public:
  /**
   * @brief Starts a new transaction on the given database connection.
   * @param db The MySQL connection handle.
   */
  explicit SQLTransaction(MYSQL *db)
      : m_db(db), m_committed(false), m_rolledBack(false) {
    if (mysql_query(m_db, "START TRANSACTION") != 0) {
      logger::error("SQLTransaction: Failed to start transaction: %s",
                    mysql_error(m_db));
      // In a real exception-safe environment we might throw here,
      // but for this codebase we'll log error.
      // Mark as rolled back so we don't try to rollback again on invalid state
      // if possible, though standard behavior is usually to just let dtor try
      // or check error state. For now, we assume simple usage.
    }
  }

  /**
   * @brief Destructor. Automatically rolls back if not committed.
   */
  ~SQLTransaction() {
    if (!m_committed && !m_rolledBack) {
      Rollback();
    }
  }

  // specific disable of copy/move to prevent double-rollback issues for now
  SQLTransaction(const SQLTransaction &) = delete;
  SQLTransaction &operator=(const SQLTransaction &) = delete;

  /**
   * @brief Commits the transaction.
   * @return true if commit was successful, false otherwise.
   */
  bool Commit() {
    if (m_committed || m_rolledBack) {
      return false;
    }

    if (mysql_query(m_db, "COMMIT") != 0) {
      logger::error("SQLTransaction: Failed to commit transaction: %s",
                    mysql_error(m_db));
      return false;
    }

    m_committed = true;
    return true;
  }

  /**
   * @brief Manually rolls back the transaction.
   * Usually not needed as destructor handles this, but useful for early exit.
   */
  void Rollback() {
    if (m_committed || m_rolledBack) {
      return;
    }

    if (mysql_query(m_db, "ROLLBACK") != 0) {
      logger::error("SQLTransaction: Failed to rollback transaction: %s",
                    mysql_error(m_db));
    }

    m_rolledBack = true;
  }

private:
  MYSQL *m_db;
  bool m_committed;
  bool m_rolledBack;
};
