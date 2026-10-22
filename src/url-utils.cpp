#include "url-utils.h"

#include <algorithm>
#include <cctype>

namespace {

std::string trim(const std::string &s)
{
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    const auto first = std::find_if_not(s.begin(), s.end(), is_space);
    const auto last = std::find_if_not(s.rbegin(), s.rend(), is_space).base();
    if (first >= last)
        return std::string();
    return std::string(first, last);
}

bool ascii_iequal(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

bool is_scheme_char(char c)
{
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '+' || c == '-' || c == '.';
}

bool contains_raw_whitespace(const std::string &s)
{
    return s.find_first_of(" \t\r\n\v\f") != std::string::npos;
}

// Returns the scheme (lowercased) and the position just past "://" when the
// input carries a valid scheme separator; an empty scheme otherwise.
std::string extract_scheme(const std::string &s, size_t &rest_pos)
{
    const size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0)
        return std::string();

    // "host:port" (digits right after the colon) is not a scheme.
    if (colon + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[colon + 1])))
        return std::string();

    const std::string scheme = s.substr(0, colon);
    for (const char c : scheme)
        if (!is_scheme_char(c))
            return std::string();

    if (s.compare(colon, 3, "://") != 0)
        return std::string();

    rest_pos = colon + 3;
    std::string lower = scheme;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower;
}

} // namespace

bool normalize_widget_url(const std::string &input, std::string &out)
{
    std::string s = trim(input);
    if (s.empty())
        return false;
    if (contains_raw_whitespace(s))
        return false;

    size_t rest_pos = 0;
    const std::string scheme = extract_scheme(s, rest_pos);

    if (scheme.empty()) {
        // "javascript:..." style inputs carry a scheme without "://" and
        // without a port - reject them instead of guessing.
        const size_t colon = s.find(':');
        const size_t slash = s.find('/');
        if (colon != std::string::npos &&
            (slash == std::string::npos || colon < slash)) {
            const bool looks_like_port =
                colon + 1 < s.size() &&
                std::isdigit(static_cast<unsigned char>(s[colon + 1]));
            if (!looks_like_port)
                return false;
        }

        s = "https://" + s;
    } else {
        if (scheme != "http" && scheme != "https")
            return false;
        // Rebuild so the scheme is lowercased regardless of input casing.
        s = scheme + "://" + s.substr(rest_pos);
    }

    const size_t after_scheme = s.find("://") + 3;
    const std::string rest = s.substr(after_scheme);
    if (rest.empty())
        return false;

    out = s;
    return true;
}

bool is_valid_widget_url(const std::string &input)
{
    std::string out;
    return normalize_widget_url(input, out);
}