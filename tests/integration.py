"""Real Git/database/HTTP integration tests; model calls use a local protocol fixture."""
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError
from urllib.request import Request, urlopen

BINARY = str(Path(sys.argv[1]).resolve())
del sys.argv[1]
IDS = ['llama.cpp', 'openvino-genai', 'onnxruntime-genai']


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], stderr=subprocess.PIPE, text=True).strip()


def commit(path, name, text):
    (path / name).write_text(text)
    git(path, 'add', name)
    git(path, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.test', 'commit', '-m', text)
    return git(path, 'rev-parse', 'HEAD')


class Model(BaseHTTPRequestHandler):
    def do_POST(self):
        payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        evidence = json.loads(payload['messages'][1]['content'])
        self.server.calls.append(evidence)
        answer = {
            'category': 'recommended' if 'fix critical crash' in evidence['commit']['subject'] else 'defer',
            'rationale': 'Fix affects the configured CPU integration.' if 'fix critical crash' in evidence['commit']['subject'] else 'Documentation is outside the requested integration.',
            'confidence': 'medium', 'risks': ['Run integration tests before importing.'],
            'dependencies': [], 'dependency_status': 'unknown', 'validation': ['Run CPU smoke tests.'],
        }
        if self.server.mode == 'invalid':
            answer['category'] = 'invented-category'
        elif self.server.mode == 'bad_dependency':
            answer['dependencies'] = ['f' * 40]
        body = json.dumps({'choices': [{'message': {'content': json.dumps(answer)}}]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


class MonitoringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.model = ThreadingHTTPServer(('127.0.0.1', 0), Model)
        cls.model.calls = []
        cls.model.mode = 'valid'
        cls.thread = threading.Thread(target=cls.model.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.model.shutdown()
        cls.model.server_close()
        cls.thread.join()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='pma fixture ; ')
        self.root = Path(self.tmp.name)
        self.upstream = self.root / 'upstream'
        self.upstream.mkdir()
        git(self.upstream, 'init', '-b', 'main')
        self.base = commit(self.upstream, 'cpu.cpp', 'initial version')
        self.providers = self.root / 'providers'
        self.providers.mkdir()
        for provider in IDS:
            git(self.root, 'clone', str(self.upstream), str(self.providers / provider))
        self.middle = commit(self.upstream, 'cpu.cpp', 'fix critical crash')
        self.head = commit(self.upstream, 'notes.md', 'update documentation')
        self.state = self.root / 'state'
        self.envfile = self.root / '.env'
        self.settings = {
            'LOCAL_REPO_PATH': str(self.providers), 'STATE_DIR': str(self.state),
            'LLAMA_REMOTE': str(self.upstream), 'OPENVINO_REMOTE': str(self.upstream), 'ONNX_REMOTE': str(self.upstream),
            'LLM_BASE_URL': f'http://127.0.0.1:{self.model.server_port}/v1', 'LLM_MODEL': 'fixture-model',
            'LLM_API_KEY': 'test-secret-must-not-appear', 'LOCAL_USAGE_CONTEXT': 'CPU integration',
            'POLL_INTERVAL_SECONDS': '1', 'GIT_TIMEOUT_SECONDS': '10', 'LLM_TIMEOUT_SECONDS': '5',
        }
        self.model.calls.clear()
        self.model.mode = 'valid'
        self.process = None

    def tearDown(self):
        if self.process and self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.communicate()
        self.tmp.cleanup()

    def environment(self):
        env = dict(os.environ)
        for key in self.settings:
            env.pop(key, None)
        return env

    def write_env(self):
        self.envfile.write_text(''.join(f'{key}="{value}"\n' for key, value in self.settings.items()))

    def scan(self, *args, expected=0):
        self.write_env()
        result = subprocess.run([BINARY, '--env', str(self.envfile), '--once', *args], env=self.environment(), text=True, capture_output=True, timeout=40)
        self.assertEqual(result.returncode, expected, result.stderr + result.stdout)
        self.assertNotIn('test-secret-must-not-appear', result.stdout + result.stderr)
        return json.loads(result.stdout)

    def test_intermediate_commit_recommendations_and_cache(self):
        indices = {p: (self.providers / p / '.git/index').read_bytes() for p in IDS}
        report = self.scan()
        for provider in IDS:
            self.assertEqual((self.providers / provider / '.git/index').read_bytes(), indices[provider])
        self.assertEqual(len(self.model.calls), 6)
        self.assertFalse(report['busy'])
        self.assertFalse(report['queued'])
        self.assertEqual(len(report['priorities']), 3)
        for provider in report['providers']:
            self.assertEqual(provider['local']['baseline'], self.base)
            self.assertEqual(provider['head'], self.head)
            self.assertEqual(provider['behind'], 2)
            self.assertTrue(provider['coverage_complete'])
            self.assertEqual([c['sha'] for c in provider['commits']], [self.middle, self.head])
            self.assertEqual(provider['commits'][0]['analysis']['category'], 'recommended')
            self.assertEqual(provider['commits'][1]['analysis']['category'], 'defer')
            self.assertEqual(provider['commits'][0]['analysis']['dependency_status'], 'unknown')
        cached = self.scan()
        self.assertEqual(len(self.model.calls), 6)
        self.assertTrue(all(c['cached'] for p in cached['providers'] for c in p['commits']))
        self.scan('--reanalyze')
        self.assertEqual(len(self.model.calls), 12)
        for provider in IDS:
            self.assertEqual(git(self.providers / provider, 'rev-parse', 'HEAD'), self.base)
            self.assertEqual(git(self.providers / provider, 'status', '--porcelain'), '')

    def test_preliminary_mode_and_capped_coverage(self):
        self.settings['LLM_BASE_URL'] = self.settings['LLM_MODEL'] = ''
        self.settings['MAX_COMMITS_PER_SCAN'] = '1'
        report = self.scan()
        self.assertFalse(report['ai_enabled'])
        self.assertEqual(report['priorities'], [])
        self.assertEqual(len(self.model.calls), 0)
        for p in report['providers']:
            self.assertFalse(p['coverage_complete'])
            self.assertEqual(p['reviewed'], 1)
            self.assertEqual(p['commits'][0]['sha'], self.head)
            self.assertEqual(p['commits'][0]['analysis']['category'], 'needs_investigation')

    def test_invalid_model_results_and_retry(self):
        self.model.mode = 'invalid'
        report = self.scan(expected=2)
        self.assertTrue(all(p['status'] == 'partial' and p['analysis_failures'] == 2 for p in report['providers']))
        self.assertEqual(report['priorities'], [])
        self.model.mode = 'valid'
        report = self.scan()
        self.assertEqual(len(self.model.calls), 12)
        self.assertTrue(all(p['analysis_failures'] == 0 for p in report['providers']))

    def test_invented_dependencies_are_rejected(self):
        self.model.mode = 'bad_dependency'
        report = self.scan(expected=2)
        self.assertEqual(report['priorities'], [])
        self.assertTrue(all(p['status'] == 'partial' for p in report['providers']))

    def test_local_fork_and_dirty_changes_preserved(self):
        local = self.providers / 'llama.cpp'
        fork = commit(local, 'integration.cpp', 'local integration')
        (local / 'cpu.cpp').write_text('uncommitted local fix')
        before = git(local, 'status', '--porcelain')
        report = self.scan()
        p = next(p for p in report['providers'] if p['id'] == 'llama.cpp')
        self.assertEqual(p['local']['baseline'], self.base)
        self.assertEqual(p['local']['head'], fork)
        self.assertEqual(p['local']['local_only_commits'], 1)
        self.assertTrue(p['local']['dirty'])
        self.assertEqual(git(local, 'status', '--porcelain'), before)
        self.assertEqual((local / 'cpu.cpp').read_text(), 'uncommitted local fix')
        self.scan()
        self.assertEqual(len(self.model.calls), 8)  # Dirty provider reanalyzed; clean providers reused.

    def test_vendored_sources_need_explicit_baseline(self):
        for provider in IDS:
            shutil.rmtree(self.providers / provider / '.git')
        git(self.providers, 'init', '-b', 'main')
        commit(self.providers, 'application.cpp', 'enclosing application version')
        report = self.scan(expected=2)
        self.assertTrue(all(p['status'] == 'needs_baseline' and p['local']['head'] == '' for p in report['providers']))
        baselines = self.root / 'baselines.json'
        baselines.write_text(json.dumps({p: self.base for p in IDS}))
        self.settings['BASELINES_FILE'] = str(baselines)
        report = self.scan()
        self.assertTrue(all(p['local']['baseline_source'] == 'explicit' for p in report['providers']))
        baselines.write_text(json.dumps({'llama.cpp': 'f' * 40}))
        report = self.scan(expected=2)
        self.assertEqual(report['providers'][0]['status'], 'error')

    def test_diverged_baseline_is_reported(self):
        fork = commit(self.providers / 'llama.cpp', 'fork.cpp', 'divergent local history')
        baselines = self.root / 'baselines.json'
        baselines.write_text(json.dumps({'llama.cpp': fork}))
        self.settings['BASELINES_FILE'] = str(baselines)
        report = self.scan(expected=2)
        self.assertEqual(report['providers'][0]['status'], 'diverged')
        self.assertEqual(report['providers'][0]['commits'], [])

    def test_missing_provider_and_unavailable_upstream(self):
        shutil.rmtree(self.providers / 'llama.cpp')
        report = self.scan(expected=2)
        self.assertEqual(report['providers'][0]['status'], 'missing')
        offline = self.root / 'not-a-git-repo'
        offline.mkdir()
        self.settings['OPENVINO_REMOTE'] = str(offline)
        report = self.scan(expected=2)
        self.assertEqual(report['providers'][1]['status'], 'error')
        self.assertEqual(report['providers'][2]['status'], 'ok')

    def test_http_dashboard_actions_history_diff_and_export(self):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        self.settings['UI_PORT'] = str(port)
        self.write_env()
        self.process = subprocess.Popen([BINARY, '--env', str(self.envfile)], env=self.environment(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        base_url = f'http://127.0.0.1:{port}'
        def fetch(path, data=None, headers=None):
            with urlopen(Request(base_url + path, data=data, headers=headers or {}), timeout=5) as res:
                return res.status, res.read()
        deadline = time.monotonic() + 15
        while True:
            try:
                _, raw = fetch('/api/status')
                state = json.loads(raw)
                if not state['busy'] and all(p['status'] == 'ok' for p in state['providers']):
                    break
            except OSError:
                pass
            if time.monotonic() > deadline:
                self.fail('Dashboard did not become ready')
            time.sleep(0.05)
        _, html = fetch('/')
        self.assertIn(b'Provider Monitor', html)
        _, diff = fetch('/api/diff/llama.cpp/' + self.middle)
        self.assertIn('fix critical crash', json.loads(diff)['text'])
        with self.assertRaises(HTTPError) as error:
            fetch('/api/diff/llama.cpp/' + 'f' * 40)
        self.assertEqual(error.exception.code, 404)
        _, history = fetch('/api/history/llama.cpp')
        self.assertGreaterEqual(len(json.loads(history)), 1)
        _, exported = fetch('/api/export')
        plan = json.loads(exported)
        self.assertFalse(plan['automatic_application'])
        self.assertEqual(plan['providers'][0]['candidates'][0]['sha'], self.middle)
        self.assertNotIn(b'test-secret-must-not-appear', exported)
        with self.assertRaises(HTTPError) as error:
            fetch('/api/check', data=b'')
        self.assertEqual(error.exception.code, 403)
        with self.assertRaises(HTTPError) as error:
            fetch('/api/check', data=b'', headers={'X-PMA-Action': '1', 'Origin': 'http://untrusted.example'})
        self.assertEqual(error.exception.code, 403)
        status, _ = fetch('/api/check', data=b'', headers={'X-PMA-Action': '1'})
        self.assertEqual(status, 202)
        status, _ = fetch('/api/reanalyze', data=b'', headers={'X-PMA-Action': '1'})
        self.assertEqual(status, 202)

    def test_merge_commits_include_first_parent_files(self):
        git(self.upstream, 'checkout', '-b', 'feature')
        feature = commit(self.upstream, 'feature.cpp', 'new provider feature')
        git(self.upstream, 'checkout', 'main')
        commit(self.upstream, 'main.cpp', 'parallel main update')
        git(self.upstream, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.test',
            'merge', '--no-ff', 'feature', '-m', 'merge feature')
        merge = git(self.upstream, 'rev-parse', 'HEAD')
        report = self.scan()
        for provider in report['providers']:
            c = next(c for c in provider['commits'] if c['sha'] == merge)
            self.assertIn('feature.cpp', c['files'])
            self.assertEqual(len(c['parents'].split()), 2)
            self.assertTrue(any(c['sha'] == feature for c in provider['commits']))

    def test_configuration_prevents_writes_inside_provider_tree(self):
        self.settings['STATE_DIR'] = str(self.providers / 'cache')
        self.write_env()
        result = subprocess.run([BINARY, '--env', str(self.envfile), '--once'], env=self.environment(), text=True, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn('STATE_DIR must be outside', result.stderr)
        self.assertFalse((self.providers / 'cache').exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
