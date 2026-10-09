# ProviderMonitorAgent

A C++20 application that monitors upstream commits for **llama.cpp**, **OpenVINO GenAI**, and **ONNX Runtime GenAI**, compares them with local provider versions, and presents per-commit recommendations in a browser dashboard. Intermediate commits can be recommended independently of the latest commit.

Five agent roles coordinate the workflow:

- **Orchestrator/Manager:** schedules concurrent provider jobs, collects local evidence, aggregates recommended commits, and persists results.
- **Local Code Checker:** identifies each provider's upstream baseline, local HEAD, fork commits, and uncommitted modifications.
- **Three Provider Monitor Agents:** fetch upstream history and review changes for their respective provider using an optional OpenAI-compatible model.

Git inspection and orchestration are deterministic. Without model configuration, commit titles receive conservative preliminary triage; the app never labels heuristic results as verified AI recommendations. The manager aggregates provider opinions rather than making an additional model call.

## Requirements and build

Linux or macOS, C++20 compiler, CMake 3.20+, Git 2.31+, pkg-config, libcurl, SQLite3, nlohmann/json 3.11+, and cpp-httplib 0.11+. Python 3 is required for integration tests. This first release uses POSIX subprocess APIs; native Windows support is not implemented. Windows users can run it in WSL.

On Debian 13 / Ubuntu with compatible package versions:

```sh
sudo apt-get update
sudo apt-get install -y g++ cmake ninja-build git pkg-config \
  libcurl4-openssl-dev libsqlite3-dev nlohmann-json3-dev libcpp-httplib-dev python3
cmake -S . -B build -G Ninja
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

In the prepared cloud workspace, run `./scripts/build.sh` to use the installed workspace-local dependency prefix and execute the same build/tests. On other machines, this helper uses system dependencies.

cpp-httplib is linked through its packaged library; its pkg-config feature flags must match the installed header/library. The build also supports dependencies extracted into an external prefix through `CMAKE_PREFIX_PATH` and `PKG_CONFIG_PATH`.

## Configure and run

```sh
cp .env.example .env
# Edit .env, especially LOCAL_REPO_PATH.
./build/provider-monitor
```

The UI listens on **127.0.0.1:8787**. In a local browser, open `http://127.0.0.1:8787`. For a remote machine, use an SSH tunnel or your platform's authenticated port forwarding. There is no public binding or built-in authentication in this release.

`LOCAL_REPO_PATH` points directly to this directory:

```text
/path/to/your/project/providers/
├── llama.cpp/
├── onnxruntime-genai/
└── openvino-genai/
```

The app runs an initial check and then repeats at `POLL_INTERVAL_SECONDS`. Each provider job operates concurrently; commits within a provider are reviewed sequentially. Failures increase the polling delay exponentially, capped at one hour. **Check now** bypasses the polling wait; **Reanalyze** also bypasses cached model results. A request during an active check queues another cycle.

Commands:

```sh
./build/provider-monitor --env /path/to/.env
./build/provider-monitor --env /path/to/.env --once
./build/provider-monitor --env /path/to/.env --once --reanalyze
./build/provider-monitor --help
```

`--once` prints JSON and exits: **0** means all providers completed their checks, **2** means at least one failed/incomplete provider check (including missing baseline or invalid AI analysis), and **1** means configuration/startup failed. A successful check without a configured model establishes monitoring and version comparison, not AI relevance analysis. Review coverage can still be capped; inspect `coverage_complete`.

## Local baseline detection

For a Git checkout or submodule rooted at the provider directory, the checker fetches its local HEAD into the app's separate cache and computes the common ancestor with the monitored upstream HEAD. A fork's local HEAD is shown separately from that baseline. Local-only commits and dirty files generate warnings. Complete related history is needed; use a full checkout or an explicit baseline when shallow histories prevent comparison.

For copied/vendored code, the containing application's Git HEAD is **not** used as the provider version. Supply a file of known upstream commits:

```json
{
  "llama.cpp": "FULL_UPSTREAM_COMMIT_SHA",
  "openvino-genai": "FULL_UPSTREAM_COMMIT_SHA",
  "onnxruntime-genai": "FULL_UPSTREAM_COMMIT_SHA"
}
```

Set `BASELINES_FILE=/absolute/path/to/baselines.json`, or use the default `.pma/baselines.json`. Values must be full commit SHAs available in fetched history and ancestors of the monitored branch. An explicit entry also overrides automatic Git baseline detection. Vendored versions are user-declared; the app cannot prove copied file contents match that baseline. It does not create or change the baseline file for you.

## AI opinions

Configure both:

```dotenv
LLM_BASE_URL=http://localhost:8080/v1
LLM_MODEL=your-model-name
LLM_API_KEY=
LOCAL_USAGE_CONTEXT="Linux CPU inference; specify APIs, build flags, constraints, and integration tests here"
```

Hosted endpoints must use HTTPS; plain HTTP is allowed only on loopback. The app calls `<LLM_BASE_URL>/chat/completions`. Authentication uses `LLM_API_KEY` when provided. Process environment variables override `.env` values. API keys are never returned by the dashboard or report export; keep `.env` outside Git.

**Model-enabled reviews send commit diffs, local path/version/status evidence, and `LOCAL_USAGE_CONTEXT` to the configured endpoint.** Choose an endpoint appropriate for your code. Tracked local patch contents and local source files are not sent. Local integration relevance relies on the supplied usage context; automatic consuming-project API/build analysis is not implemented.

Provider opinions include category (`recommended`, `optional`, `defer`, `needs_investigation`), rationale, confidence, risks, suggested prerequisite SHAs, and human validation steps. Diffs are capped at 24 KB per model review. Repository text is supplied as untrusted evidence. Responses are schema-checked, and suggested dependencies must resolve to ancestor commits; this still does **not** prove a complete or compatible cherry-pick set. Malformed responses, endpoint failures, or invented dependency SHAs produce `partial` results with a manual-review fallback and are retried in subsequent scans.

Results are reused across restarts when commit SHA, baseline, local HEAD, model, endpoint, and usage context match. Dirty providers are reanalyzed. Use **Reanalyze** after changing model server behavior, credentials, or declared vendored contents.

## Dashboard and persistence

The dashboard shows provider health, local/upstream versions, manager priorities, commit opinions, selectable diffs, coverage warnings, and recent checks. It refreshes every two seconds. The JSON import-plan export contains recommended candidates and their uncertainties; it is **not** a verified dependency closure. Importing code is a human action: the application never resets, checks out, cherry-picks, or writes local provider files.

`STATE_DIR` defaults to `.pma` in the working directory and must be outside `LOCAL_REPO_PATH`. It retains bare Git caches and SQLite history (100 reports per provider; UI/API history shows the latest 20). Processes restart separately from retained state. Stop with Ctrl+C; current bounded Git/model requests may finish before shutdown completes.

Default upstream repositories:

| Directory | Upstream | Overrides |
|---|---|---|
| `llama.cpp` | `ggml-org/llama.cpp` | `LLAMA_REMOTE`, `LLAMA_BRANCH` |
| `openvino-genai` | `openvinotoolkit/openvino.genai` | `OPENVINO_REMOTE`, `OPENVINO_BRANCH` |
| `onnxruntime-genai` | `microsoft/onnxruntime-genai` | `ONNX_REMOTE`, `ONNX_BRANCH` |

Default branches are discovered from each remote. Remote overrides accept HTTPS URLs without embedded credentials or absolute local Git paths (useful for offline testing). Git uses existing platform/proxy authentication; the application does not require a GitHub API token. Full monitored-branch history is fetched; initial fetches can be substantial. Adjust `GIT_TIMEOUT_SECONDS` accordingly.

`MAX_COMMITS_PER_SCAN` defaults to 50 (maximum 500). The **most recent** commits after the baseline are reviewed in topological order. Older intermediate commits outside that window are explicitly unreviewed. Increase the limit to inspect a broader range; large gaps may require a future paginated review implementation. No automated provider builds or release-note retrieval are performed.

API endpoints:

- `GET /api/status`: current provider reports and manager priorities.
- `GET /api/history/<provider>`: recent check summaries.
- `GET /api/diff/<provider>/<sha>`: first-parent patch for a commit in the current review window.
- `GET /api/export`: candidate import-plan download.
- `POST /api/check` and `POST /api/reanalyze`: queue actions; require `X-PMA-Action: 1` and same-origin requests.

## Validation

```sh
ctest --test-dir build --output-on-failure
```

The Python integration suite creates real local Git repositories and a mock OpenAI-compatible HTTP server. It verifies intermediate-commit selection, restart caching, forced reanalysis, review caps, malformed model responses, invalid dependencies, forks and dirty files, vendored baselines, divergent history, unavailable providers, read-only local behavior, configuration validation, and dashboard/API operations. It does not require external provider clones, hosted model access, or credentials. Mock-model tests verify the protocol and result handling, not the quality of a production model's opinions.
