#include <cstdio>
#include <string>

#include "url-utils.h"

namespace {

int failures = 0;

void expect(const char *name, bool condition)
{
    if (!condition) {
        std::printf("FAIL: %s\n", name);
        failures++;
    } else {
        std::printf("ok:   %s\n", name);
    }
}

void expect_url(const char *name, const std::string &input,
                const std::string &expected)
{
    std::string out;
    if (!normalize_widget_url(input, out)) {
        std::printf("FAIL: %s (expected normalization, got failure)\n", name);
        failures++;
        return;
    }
    if (out != expected) {
        std::printf("FAIL: %s (got \"%s\", expected \"%s\")\n", name,
                    out.c_str(), expected.c_str());
        failures++;
        return;
    }
    std::printf("ok:   %s -> %s\n", name, out.c_str());
}

void expect_invalid(const char *name, const std::string &input)
{
    std::string out;
    if (normalize_widget_url(input, out)) {
        std::printf("FAIL: %s (expected failure, got \"%s\")\n", name,
                    out.c_str());
        failures++;
        return;
    }
    std::printf("ok:   %s (rejected)\n", name);
}

} // namespace

int main()
{
    // Normalization: no scheme -> https.
    expect_url("plain host", "example.com/widget/1",
               "https://example.com/widget/1");
    expect_url("with subdomain and port", "alerts.example.com:443/x",
               "https://alerts.example.com:443/x");
    expect_url("https explicit", "https://example.com/a", "https://example.com/a");
    expect_url("http explicit", "http://example.com/a", "http://example.com/a");
    expect_url("uppercase scheme", "HTTPS://EXAMPLE.COM", "https://EXAMPLE.COM");
    expect_url("whitespace around", "  https://example.com  ",
               "https://example.com");
    expect_url("query string", "example.com/widget?a=1&b=2",
               "https://example.com/widget?a=1&b=2");
    expect_url("ipv4 literal", "127.0.0.1:8080/w", "https://127.0.0.1:8080/w");

    // Invalid inputs.
    expect_invalid("empty string", "");
    expect_invalid("only whitespace", "   \t ");
    expect_invalid("ftp scheme", "ftp://example.com/file");
    expect_invalid("javascript scheme", "javascript:alert(1)");
    expect_invalid("scheme without host", "https://");
    expect_invalid("broken scheme separator", "https:/example.com");
    expect_invalid("space inside host", "https://exa mple.com");

    // is_valid_widget_url agrees with normalize.
    expect("valid agrees", is_valid_widget_url("widget.example.com/x") &&
                               !is_valid_widget_url("") &&
                               !is_valid_widget_url("ftp://x.com"));

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", failures);
    return 1;
}