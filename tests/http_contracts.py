"""Real-socket transport contracts; synthetic backend controls scheduling deterministically."""
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import time
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 else 'build/turnserve-server'


class Service:
    def __init__(self, *options):
        self.errors = tempfile.TemporaryFile(mode="w+")
        self.process = subprocess.Popen([BINARY, '--port', '0', *options], stdout=subprocess.PIPE,
                                        stderr=self.errors, text=True)
        ready, _, _ = select.select([self.process.stdout], [], [], 30)
        if not ready:
            self.process.kill()
            self.process.wait()
            self.errors.close()
            raise RuntimeError('server startup timeout')
        line = self.process.stdout.readline()
        if not line:
            self.process.wait()
            self.errors.seek(0)
            error = self.errors.read()
            self.errors.close()
            raise RuntimeError(error)
        self.port = json.loads(line)['port']

    def request(self, method='GET', path='/metrics', body=None, headers=None):
        c = http.client.HTTPConnection('127.0.0.1', self.port, timeout=10)
        try:
            c.request(method, path, body, headers or {})
            r = c.getresponse()
            return r.status, r.read().decode()
        finally:
            c.close()

    def stream(self, tokens=1024):
        s = socket.create_connection(('127.0.0.1', self.port), timeout=5)
        s.sendall(f'POST /generate HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\nX-Max-Tokens: {tokens}\r\n\r\nhi'.encode())
        return s

    def metrics(self):
        return json.loads(self.request()[1])

    def wait(self, predicate):
        end = time.monotonic() + 5
        while time.monotonic() < end:
            m = self.metrics()
            if predicate(m):
                return m
            time.sleep(.01)
        raise AssertionError(m)

    def close(self):
        self.process.terminate()
        try:
            out, err = self.process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            out, err = self.process.communicate()
            self.errors.seek(0)
            error = self.errors.read()
            self.errors.close()
            raise AssertionError('server did not shut down: ' + error)
        self.errors.seek(0)
        error = self.errors.read()
        self.errors.close()
        if self.process.returncode != 0:
            raise AssertionError(error)
        return json.loads(out.strip().splitlines()[-1])


class Contracts(unittest.TestCase):
    def service(self, *args):
        service = Service(*args)
        self.addCleanup(lambda: service.close() if service.process.poll() is None else None)
        return service

    def test_concurrent_streams_and_session_reclamation(self):
        s = self.service('--max-inflight', '4')
        # More lifetime requests than the runtime's session cap must succeed.
        for _ in range(6):
            with concurrent.futures.ThreadPoolExecutor(4) as pool:
                results = list(pool.map(lambda _: s.request('POST', '/generate', 'hello',
                                                           {'X-Max-Tokens': '8'}), range(4)))
            for status, body in results:
                self.assertEqual(status, 200)
                events = [json.loads(line[6:]) for line in body.splitlines() if line.startswith('data: ')]
                self.assertEqual(sum(e['type'] == 'completed' for e in events), 1)
                self.assertEqual(sum(e['type'] == 'token' for e in events), 8)
                self.assertEqual(events[-1]['type'], 'completed')
        self.assertEqual(s.metrics()['completed'], 24)

    def test_disconnect_cancels_and_releases_capacity(self):
        s = self.service('--max-inflight', '1', '--synthetic-tick-delay-ms', '2')
        c = s.stream()
        self.assertIn(b'200 OK', c.recv(4096))
        c.close()
        m = s.wait(lambda m: m['cancelled'] == 1 and m['active'] == 0)
        self.assertEqual(m['completed'], 0)
        status, body = s.request('POST', '/generate', 'next', {'X-Max-Tokens': '2'})
        self.assertEqual(status, 200)
        self.assertIn('event: completed', body)

    def test_overload_rejected_before_stream_starts(self):
        s = self.service('--max-inflight', '1', '--synthetic-tick-delay-ms', '2')
        c = s.stream()
        self.addCleanup(c.close)
        c.recv(4096)
        self.assertEqual(s.request('POST', '/generate', 'next')[0], 503)
        self.assertEqual(s.metrics()['rejected'], 1)

    def test_slow_reader_has_bounded_buffer_and_releases_capacity(self):
        s = self.service('--buffer-bytes', '1024', '--timeout-ms', '500')
        c = s.stream()
        self.addCleanup(c.close)
        m = s.wait(lambda m: m['slow_clients'] >= 1 and m['active'] == 0)
        self.assertLessEqual(m['peak_buffer_bytes'], 1024)
        self.assertEqual(m['cancelled'], 1)
        self.assertEqual(s.request('POST', '/generate', 'next', {'X-Max-Tokens': '2'})[0], 200)

    def test_request_deadline_is_not_extended_by_trickle(self):
        s = self.service('--timeout-ms', '100')
        c = socket.create_connection(('127.0.0.1', s.port), timeout=3)
        self.addCleanup(c.close)
        c.sendall(b'POST /generate HTTP/1.1\r\n')
        time.sleep(.06)
        c.sendall(b'Host: localhost\r\n')
        self.assertIn(b'408', c.recv(4096))

    def test_bad_framing_and_bounds(self):
        s = self.service()
        for headers, body, expected in [
            ('Content-Length: 2\r\nContent-Length: 2', b'hi', 400),
            ('Content-Length: 2\r\nTransfer-Encoding: chunked', b'hi', 400),
            ('Content-Length: 999999999999999999999', b'', 400),
            ('Content-Length: 65537', b'', 413),
            ('Content-Length: 2\r\nX-Max-Tokens: 2048', b'hi', 400),
        ]:
            with socket.create_connection(('127.0.0.1', s.port), timeout=3) as c:
                c.sendall(b'POST /generate HTTP/1.1\r\n' + headers.encode() + b'\r\n\r\n' + body)
                self.assertIn(str(expected).encode(), c.recv(4096))
        self.assertEqual(s.metrics()['submitted'], 0)

    def test_shutdown_drains_or_cancels(self):
        s = self.service('--grace-ms', '50', '--synthetic-tick-delay-ms', '2')
        c = s.stream()
        self.addCleanup(c.close)
        c.recv(4096)
        m = s.close()
        self.assertEqual(m['active'], 0)
        self.assertEqual(m['cancelled'], 1)
        self.assertEqual(m['submitted'], m['completed'] + m['cancelled'] + m['failed'])

    def test_shutdown_allows_short_request_to_finish(self):
        s = self.service('--grace-ms', '1000', '--synthetic-tick-delay-ms', '2')
        c = s.stream(8)
        self.addCleanup(c.close)
        c.recv(4096)
        m = s.close()
        self.assertEqual(m['completed'], 1)
        self.assertEqual(m['cancelled'], 0)

    @unittest.skipUnless(os.environ.get('TURNSERVE_TEST_MODEL'), 'real model opt-in')
    def test_real_model_stream(self):
        s = self.service('--model', os.environ['TURNSERVE_TEST_MODEL'])
        status, body = s.request('POST', '/generate', 'Name a planet.', {'X-Max-Tokens': '8'})
        self.assertEqual(status, 200)
        self.assertIn('event: token', body)
        self.assertIn('event: completed', body)


if __name__ == '__main__':
    unittest.main()
