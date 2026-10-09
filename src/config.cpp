#include "pma/core.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <regex>
#include <ctime>
#include <iomanip>
namespace pma {
std::string trim(std::string s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}
std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read file: " + path.string());
    std::ostringstream out; out << in.rdbuf(); return out.str();
}
std::string timestamp() {
    auto t = std::time(nullptr); std::tm tm{}; gmtime_r(&t, &tm);
    std::ostringstream out; out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ"); return out.str();
}
Config load_config(const fs::path& env_file) {
    std::map<std::string, std::string> values;
    if (fs::exists(env_file)) {
        std::istringstream lines(read_file(env_file)); std::string line;
        while (std::getline(lines, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) throw std::runtime_error("Invalid .env line; expected NAME=value");
            auto name = trim(line.substr(0, eq)); auto value = trim(line.substr(eq + 1));
            if (!std::regex_match(name, std::regex("[A-Za-z_][A-Za-z0-9_]*"))) throw std::runtime_error("Invalid .env variable name");
            if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) value = value.substr(1, value.size()-2);
            values[name] = value;
        }
    }
    auto get = [&](const std::string& name, const std::string& fallback = "") {
        const char* value = std::getenv(name.c_str());
        if (value) return std::string(value);
        return values.count(name) ? values.at(name) : fallback;
    };
    auto number = [&](const std::string& name, int fallback, int lo, int hi) {
        auto value = get(name, std::to_string(fallback)); size_t pos = 0; int result;
        try { result = std::stoi(value, &pos); } catch (...) { throw std::runtime_error("Invalid integer: " + name); }
        if (pos != value.size() || result < lo || result > hi) throw std::runtime_error("Out-of-range integer: " + name);
        return result;
    };
    Config cfg;
    auto local = get("LOCAL_REPO_PATH");
    if (local.empty()) throw std::runtime_error("Set LOCAL_REPO_PATH to the directory containing your three providers");
    cfg.local = fs::weakly_canonical(fs::absolute(local));
    if (!fs::is_directory(cfg.local)) throw std::runtime_error("LOCAL_REPO_PATH must be an existing directory");
    cfg.state = fs::weakly_canonical(fs::absolute(get("STATE_DIR", ".pma")));
    // State and Git caches must never write inside the monitored provider tree.
    auto relative = cfg.state.lexically_relative(cfg.local);
    if (relative.empty() || (relative.begin() != relative.end() && *relative.begin() != "..")) throw std::runtime_error("STATE_DIR must be outside LOCAL_REPO_PATH");
    cfg.baselines = fs::absolute(get("BASELINES_FILE", (cfg.state / "baselines.json").string()));
    cfg.interval = number("POLL_INTERVAL_SECONDS", 300, 1, 86400);
    cfg.port = number("UI_PORT", 8787, 1, 65535);
    cfg.max_commits = number("MAX_COMMITS_PER_SCAN", 50, 1, 500);
    cfg.git_timeout = number("GIT_TIMEOUT_SECONDS", 180, 1, 3600);
    cfg.llm_timeout = number("LLM_TIMEOUT_SECONDS", 60, 1, 600);
    cfg.llm_url = get("LLM_BASE_URL");
    while (!cfg.llm_url.empty() && cfg.llm_url.back() == '/') cfg.llm_url.pop_back();
    cfg.llm_model = get("LLM_MODEL"); cfg.llm_key = get("LLM_API_KEY");
    cfg.usage_context = get("LOCAL_USAGE_CONTEXT");
    if (cfg.llm_url.empty() != cfg.llm_model.empty()) throw std::runtime_error("Set both LLM_BASE_URL and LLM_MODEL, or leave both empty");
    if (!cfg.llm_url.empty()) {
        if (!std::regex_match(cfg.llm_url, std::regex(R"(https://[^\s?#@]+|http://(localhost|127\.0\.0\.1|\[::1\])(:[0-9]+)?(/[^\s?#@]*)?)")))
            throw std::runtime_error("LLM_BASE_URL must use HTTPS, or HTTP on loopback; credentials belong in LLM_API_KEY");
    }
    cfg.providers = {{"llama.cpp", get("LLAMA_REMOTE", "https://github.com/ggml-org/llama.cpp.git"), get("LLAMA_BRANCH")},
                     {"openvino-genai", get("OPENVINO_REMOTE", "https://github.com/openvinotoolkit/openvino.genai.git"), get("OPENVINO_BRANCH")},
                     {"onnxruntime-genai", get("ONNX_REMOTE", "https://github.com/microsoft/onnxruntime-genai.git"), get("ONNX_BRANCH")}};
    for (auto& provider : cfg.providers) {
        if (provider.remote.starts_with("https://")) {
            if (provider.remote.find('@') != std::string::npos) throw std::runtime_error("Do not embed credentials in provider URLs");
        } else if (!fs::is_directory(provider.remote) || !fs::path(provider.remote).is_absolute()) {
            throw std::runtime_error("Provider remote must be an HTTPS URL or an absolute local Git directory");
        }
        if (!provider.branch.empty() && git(fs::current_path(), {"check-ref-format", "refs/heads/" + provider.branch}).code != 0)
            throw std::runtime_error("Invalid provider branch");
    }
    fs::create_directories(cfg.state);
    fs::permissions(cfg.state, fs::perms::owner_all, fs::perm_options::replace);
    return cfg;
}
}
