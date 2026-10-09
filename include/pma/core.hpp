#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
struct sqlite3;
namespace pma {
using Json = nlohmann::json;
namespace fs = std::filesystem;
struct Provider { std::string id, remote, branch; };
struct Config {
    fs::path local, state, baselines;
    std::vector<Provider> providers;
    int interval = 300, port = 8787, max_commits = 50, git_timeout = 180, llm_timeout = 60;
    std::string llm_url, llm_model, llm_key, usage_context;
    bool ai() const { return !llm_url.empty() && !llm_model.empty(); }
};
Config load_config(const fs::path& env_file);
std::string read_file(const fs::path& path);
std::string timestamp();
std::string trim(std::string value);
struct ProcessResult { int code; std::string output; bool truncated; };
ProcessResult run(const std::vector<std::string>& args, int timeout = 180, size_t limit = 2 * 1024 * 1024);
ProcessResult git(const fs::path& directory, const std::vector<std::string>& args, int timeout = 180);
std::string git_ok(const fs::path& directory, const std::vector<std::string>& args, int timeout = 180);
class Store {
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
public:
    explicit Store(const fs::path& path);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Json latest();
    Json history(const std::string& provider);
    void save(const Json& report);
};
class LocalCodeCheckerAgent {
public:
    Json inspect(const Config& cfg, const Provider& provider, const fs::path& cache, const std::string& upstream_head) const;
};
class ProviderMonitorAgent {
    Config cfg_;
    Provider provider_;
public:
    ProviderMonitorAgent(const Config& cfg, const Provider& provider) : cfg_(cfg), provider_(provider) {}
    Json observe() const;
    Json review(const Json& observation, const Json& local, const Json& previous, bool reanalyze) const;
    Json diff(const std::string& sha) const;
};
class OrchestratorAgent {
    Config cfg_;
    Store store_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Json reports_ = Json::object();
    bool pending_ = true, force_ = false, stopping_ = false, busy_ = false;
    std::string last_completed_, error_;
    int failures_ = 0;
    std::thread worker_;
    void loop();
public:
    explicit OrchestratorAgent(Config cfg);
    ~OrchestratorAgent();
    void start();
    Json check_once(bool reanalyze = false);
    Json snapshot() const;
    Json history(const std::string& provider) { return store_.history(provider); }
    void request(bool reanalyze);
    Json diff(const std::string& provider, const std::string& sha) const;
};
}
