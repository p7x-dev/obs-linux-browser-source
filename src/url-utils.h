#pragma once

#include <string>

// Pure URL normalization logic, no Qt dependency (unit-testable headless).

// Trims whitespace, prepends "https://" when no scheme is present,
// validates that the scheme is http/https.
// Returns false when the input cannot be turned into a usable widget URL.
bool normalize_widget_url(const std::string &input, std::string &out);

// True when the string looks like a usable widget URL.
bool is_valid_widget_url(const std::string &input);