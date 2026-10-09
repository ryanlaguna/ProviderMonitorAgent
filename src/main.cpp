#include "pma/core.hpp"
#include <httplib.h>
#include <curl/curl.h>
#include <csignal>
#include <iostream>
#include <thread>
namespace {
volatile std::sig_atomic_t interrupted = 0;
void stop(int) { interrupted = 1; }
void json_response(httplib::Response& res, const pma::Json& value) { res.set_content(value.dump(), "application/json"); }
}
int main(int argc, char** argv) {
    try {
        std::filesystem::path env = ".env", web = PMA_WEB_DIR;
        bool once = false, reanalyze = false;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--env" && i + 1 < argc) env = argv[++i];
            else if (arg == "--web-dir" && i + 1 < argc) web = argv[++i];
            else if (arg == "--once") once = true;
            else if (arg == "--reanalyze") reanalyze = true;
            else if (arg == "--help") {
                std::cout << "provider-monitor [--env FILE] [--once] [--reanalyze] [--web-dir DIR]\n"
                             "--once emits a JSON report and exits; it fails for incomplete/failed checks.\n"
                             "The dashboard binds to 127.0.0.1; no provider files are modified.\n"; return 0;
            } else throw std::runtime_error("Unknown or incomplete CLI option");
        }
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("Cannot initialize HTTP library");
        auto cfg = pma::load_config(env);
        // These objects are destroyed before global curl cleanup.
        int result = [&]() {
            pma::OrchestratorAgent manager(cfg);
            if (once) {
                auto report = manager.check_once(reanalyze); std::cout << report.dump(2) << '\n';
                if (!report.at("error").get<std::string>().empty()) return 2;
                for (const auto& provider : report.at("providers")) if (provider.at("status") != "ok") return 2;
                return 0;
            }
            httplib::Server server;
            server.set_payload_max_length(4096);
            server.set_read_timeout(5, 0); server.set_write_timeout(5, 0);
            server.set_default_headers({{"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"},
                {"Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'"}});
            auto index = pma::read_file(web / "index.html");
            const std::string loopback = "127.0.0.1:" + std::to_string(cfg.port);
            const std::string localhost = "localhost:" + std::to_string(cfg.port);
            server.set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
                auto host = req.get_header_value("Host");
                if (host != loopback && host != localhost) { res.status = 403; json_response(res, {{"error", "Invalid host"}}); return httplib::Server::HandlerResponse::Handled; }
                if (req.method == "POST") {
                    auto origin = req.get_header_value("Origin");
                    if (req.get_header_value("X-PMA-Action") != "1" || (!origin.empty() && origin != "http://" + loopback && origin != "http://" + localhost)) {
                        res.status = 403; json_response(res, {{"error", "Same-origin action header required"}}); return httplib::Server::HandlerResponse::Handled;
                    }
                }
                return httplib::Server::HandlerResponse::Unhandled;
            });
            server.set_exception_handler([](const auto&, auto& res, std::exception_ptr) {
                res.status = 500; json_response(res, {{"error", "Request failed; inspect configuration or current review window"}});
            });
            server.Get("/", [&](const auto&, auto& res) { res.set_content(index, "text/html"); });
            server.Get("/api/status", [&](const auto&, auto& res) { json_response(res, manager.snapshot()); });
            server.Get("/api/history/([a-zA-Z0-9_.-]+)", [&](const auto& req, auto& res) { json_response(res, manager.history(req.matches[1])); });
            server.Get("/api/diff/([a-zA-Z0-9_.-]+)/([0-9a-f]+)", [&](const auto& req, auto& res) {
                try { json_response(res, manager.diff(req.matches[1], req.matches[2])); }
                catch (...) { res.status = 404; json_response(res, {{"error", "Commit is unavailable or outside the current review window"}}); }
            });
            server.Get("/api/export", [&](const auto&, auto& res) {
                auto state = manager.snapshot(); pma::Json plan{{"generated_at", pma::timestamp()}, {"automatic_application", false},
                    {"notice", "Candidate recommendations only. Dependencies and integration compatibility require human review. No changes have been applied."}, {"providers", pma::Json::array()}};
                for (const auto& provider : state.at("providers")) {
                    pma::Json candidates = pma::Json::array();
                    for (const auto& commit : provider.value("commits", pma::Json::array())) if (commit["analysis"]["category"] == "recommended") candidates.push_back(commit);
                    plan["providers"].push_back({{"id", provider["id"]}, {"status", provider["status"]}, {"checked_at", provider.value("checked_at", "")},
                        {"baseline", provider.value("local", pma::Json::object()).value("baseline", "")}, {"upstream_head", provider.value("head", "")},
                        {"coverage_complete", provider.value("coverage_complete", false)}, {"warnings", provider.value("warnings", pma::Json::array())}, {"candidates", candidates}});
                }
                res.set_header("Content-Disposition", "attachment; filename=provider-import-plan.json"); json_response(res, plan);
            });
            server.Post("/api/check", [&](const auto&, auto& res) { manager.request(false); res.status = 202; json_response(res, {{"queued", true}}); });
            server.Post("/api/reanalyze", [&](const auto&, auto& res) { manager.request(true); res.status = 202; json_response(res, {{"queued", true}}); });
            if (!server.bind_to_port("127.0.0.1", cfg.port)) throw std::runtime_error("Cannot bind UI port; choose a free UI_PORT");
            std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
            if (reanalyze) manager.request(true);
            manager.start();
            std::thread http([&] { server.listen_after_bind(); });
            std::cout << "Dashboard listening on 127.0.0.1:" << cfg.port << "; " << (cfg.ai() ? "AI reviews enabled" : "preliminary triage only") << std::endl;
            for (int tries = 0; tries < 100 && !server.is_running(); ++tries) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            while (!interrupted && server.is_running()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            server.stop(); http.join(); return 0;
        }();
        curl_global_cleanup(); return result;
    } catch (const std::exception& error) { std::cerr << "Error: " << error.what() << '\n'; return 1; }
}
