// Exercise the actual HLE request constructors, without sockets or game files.
#undef NDEBUG
#include <cassert>
#include "../src/hle/http.cpp"
void register_hle_fn(const char*, void*) {}
static HostConfig test_config;
const HostConfig& config() { return test_config; }
static void active(bool enabled) {
#if defined(_WIN32)
    SetEnvironmentVariableA("BBHOST_JP_EU_ACTIVE", enabled ? "1" : nullptr);
#else
    if (enabled) setenv("BBHOST_JP_EU_ACTIVE", "1", 1); else unsetenv("BBHOST_JP_EU_ACTIVE");
#endif
}
static int connection(const char* ua, const char* url = "") {
    const int tmpl = hle_http_create_template(hle_http_init(0, 0, 0), ua, 1, 0);
    return hle_http_connect_url(tmpl, url, 0);
}
int main() {
    constexpr auto jp = "J-JP9000-CUSA03014_00-BLOODBORNE0000JP";
    constexpr auto eu = "E-EP9000-CUSA03173_00-BLOODBORNE0000EU";
    active(true);
    int conn = connection(jp);
    assert(g_http[g_http[conn].parent].ua == eu);
    auto request = [&](int method = 0, const char* url = "") {
        return g_http[hle_http_request_url(conn, method, url, 0)].url;
    };
    const std::string info = "https://thehuntersdream.com:20443/bb-eu/ss.info";
    assert(request() == info);
    assert(request().empty());  // less than a second
    g_jp_eu_bootstrap_history.back() -= std::chrono::seconds(2);
    assert(request() == info);  // no successful login required for reconnect
    g_jp_eu_bootstrap_history.assign(12, std::chrono::steady_clock::now() - std::chrono::seconds(2));
    assert(request().empty());
    for (auto& time : g_jp_eu_bootstrap_history) time -= std::chrono::minutes(1);
    assert(request() == info);
    g_jp_eu_bootstrap_history.clear();
    assert(request(1).empty());
    assert(request(0, "https://example.invalid/status") == "https://example.invalid/status");
    assert(g_jp_eu_bootstrap_history.empty());
    conn = connection("unrelated");
    assert(request().empty());
    conn = connection(eu, "https://example.invalid/");
    assert(request().empty());
    const std::string broken = "https://.s3.amazonaws.com/20261026/play-log_01.bin";
    test_config.online_host = "example.invalid";
    test_config.online_scheme = "https";
    assert(rewrite_jp_eu_missing_s3_playlog(broken, 4) == "https://example.invalid:18671/20261026/play-log_01.bin");
    assert(rewrite_jp_eu_missing_s3_playlog(broken, 0) == broken);
    for (const char* key : {"", "20261026/", "20261026/../x", "20261026/x?q=1", "abcdefgh/log"}) {
        const std::string url = std::string("https://.s3.amazonaws.com/") + key;
        assert(rewrite_jp_eu_missing_s3_playlog(url, 4) == url);
    }
    active(false);
    assert(rewrite_jp_eu_missing_s3_playlog(broken, 4) == broken);
    conn = connection(jp);
    assert(g_http[g_http[conn].parent].ua == jp);
    assert(request().empty());
    active(true);
    g_jp_eu_bootstrap_history.push_back(std::chrono::steady_clock::now());
    hle_http_term();
    assert(g_jp_eu_bootstrap_history.empty());
    conn = connection(jp);
    assert(request() == info);
    active(false);
}
