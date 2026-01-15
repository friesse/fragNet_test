#pragma once
/**
 * safe_parse.hpp - Safe string-to-number conversion utilities
 *
 * Replaces unsafe atoi() calls with proper error handling.
 * Returns std::optional which is empty on parse failure.
 */

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <optional>
#include <string>


namespace SafeParse {

/**
 * Safely parse a string to int.
 * Returns std::nullopt if str is null, empty, or not a valid integer.
 */
inline std::optional<int> toInt(const char *str) {
  if (!str || str[0] == '\0') {
    return std::nullopt;
  }

  char *endptr = nullptr;
  errno = 0;
  long result = std::strtol(str, &endptr, 10);

  // Check for errors
  if (errno == ERANGE || result > INT_MAX || result < INT_MIN) {
    return std::nullopt; // Overflow
  }
  if (endptr == str || *endptr != '\0') {
    return std::nullopt; // No valid conversion or trailing garbage
  }

  return static_cast<int>(result);
}

/**
 * Safely parse a string to uint32_t.
 * Returns std::nullopt if str is null, empty, negative, or not valid.
 */
inline std::optional<uint32_t> toUint32(const char *str) {
  if (!str || str[0] == '\0') {
    return std::nullopt;
  }

  // Check for negative sign
  if (str[0] == '-') {
    return std::nullopt;
  }

  char *endptr = nullptr;
  errno = 0;
  unsigned long result = std::strtoul(str, &endptr, 10);

  // Check for errors
  if (errno == ERANGE || result > UINT32_MAX) {
    return std::nullopt; // Overflow
  }
  if (endptr == str || *endptr != '\0') {
    return std::nullopt; // No valid conversion or trailing garbage
  }

  return static_cast<uint32_t>(result);
}

/**
 * Safely parse a string to uint64_t.
 * Returns std::nullopt if str is null, empty, negative, or not valid.
 */
inline std::optional<uint64_t> toUint64(const char *str) {
  if (!str || str[0] == '\0') {
    return std::nullopt;
  }

  // Check for negative sign
  if (str[0] == '-') {
    return std::nullopt;
  }

  char *endptr = nullptr;
  errno = 0;
  unsigned long long result = std::strtoull(str, &endptr, 10);

  // Check for errors
  if (errno == ERANGE) {
    return std::nullopt; // Overflow
  }
  if (endptr == str || *endptr != '\0') {
    return std::nullopt; // No valid conversion or trailing garbage
  }

  return static_cast<uint64_t>(result);
}

/**
 * Safely parse a string to float.
 * Returns std::nullopt if str is null, empty, or not a valid float.
 */
inline std::optional<float> toFloat(const char *str) {
  if (!str || str[0] == '\0') {
    return std::nullopt;
  }

  char *endptr = nullptr;
  errno = 0;
  float result = std::strtof(str, &endptr);

  // Check for errors
  if (errno == ERANGE) {
    return std::nullopt; // Overflow or underflow
  }
  if (endptr == str || *endptr != '\0') {
    return std::nullopt; // No valid conversion or trailing garbage
  }

  return result;
}

/**
 * Safely parse a string to double.
 * Returns std::nullopt if str is null, empty, or not a valid double.
 */
inline std::optional<double> toDouble(const char *str) {
  if (!str || str[0] == '\0') {
    return std::nullopt;
  }

  char *endptr = nullptr;
  errno = 0;
  double result = std::strtod(str, &endptr);

  // Check for errors
  if (errno == ERANGE) {
    return std::nullopt; // Overflow or underflow
  }
  if (endptr == str || *endptr != '\0') {
    return std::nullopt; // No valid conversion or trailing garbage
  }

  return result;
}

} // namespace SafeParse
