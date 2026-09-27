#!/usr/bin/env python3
"""MCP adapter regressions over stdio against a synthetic demo archive."""
import json
import os
from datetime import datetime, timedelta, timezone
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / 'scripts/replay_mcp.py'
BIN = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / 'build/replay')
ENV = dict(os.environ, OMP_THREAD_LIMIT='1', QT_QPA_PLATFORM='offscreen',
           OMARCHY_REPLAY_BIN=BIN, PYTHONDONTWRITEBYTECODE='1')
ENV.pop('OMARCHY_REPLAY_ARCHIVE', None)


class Server:
    def __init__(self, archive):
        self.proc = subprocess.Popen(
            [sys.executable, '-B', str(SERVER)], env=dict(ENV, OMARCHY_REPLAY_ARCHIVE=archive),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def call(self, payload, timeout=120):
        self.proc.stdin.write(json.dumps(payload) + '\n')
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        assert line, f'server closed: {self.proc.stderr.read()}'
        return json.loads(line)

    def notify(self, payload):
        self.proc.stdin.write(json.dumps(payload) + '\n')
        self.proc.stdin.flush()


def main():
    with tempfile.TemporaryDirectory(prefix='replay-mcp-') as temporary:
        os.chdir(temporary)
        started = datetime.now(timezone.utc)
        dataset = Path(temporary) / 'archive'
        # Fictional synthetic workload only; no real screen content is involved.
        subprocess.run([BIN, 'demo', '--dir', str(dataset), '--frames', '200', '--interval', '.25',
                        '--width', '960', '--height', '540', '--max-mib', '64'],
                       env=ENV, capture_output=True, text=True, timeout=600)
        assert (dataset / 'ground-truth.json').exists(), 'demo archive was not created'

        server = Server(dataset)
        init = server.call({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
            'protocolVersion': '2024-11-05', 'capabilities': {}, 'clientInfo': {'name': 'test'}}})
        assert init['result']['serverInfo']['name'] == 'omarchy-replay-mcp', init
        server.notify({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
        # A notification must not produce a response line; the next reply is tools/list.
        listing = server.call({'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'})
        names = {tool['name'] for tool in listing['result']['tools']}
        assert names == {'search', 'list_frames', 'get_moment', 'status'}, names
        assert all('untrusted' in tool['description'].lower() for tool in listing['result']['tools'])

        search = server.call({'jsonrpc': '2.0', 'id': 3, 'method': 'tools/call', 'params': {
            'name': 'search', 'arguments': {'words': 'Patrick', 'limit': 2}}})
        assert search['result']['isError'] is False, search
        payload = json.loads(search['result']['content'][0]['text'])
        assert payload['total_matches'] >= 1 and payload['results'], payload
        first_id = payload['results'][0]['first_id']
        assert first_id == payload['results'][0]['last_id'], payload

        moment = server.call({'jsonrpc': '2.0', 'id': 4, 'method': 'tools/call', 'params': {
            'name': 'get_moment', 'arguments': {'id': first_id, 'context_seconds': 5}}})
        moment_payload = json.loads(moment['result']['content'][0]['text'])
        assert moment_payload['moment']['id'] == first_id, moment_payload
        assert moment_payload['moment']['image_path'], moment_payload
        assert not Path('extracted.png').exists(), 'moment fetch must not extract images'

        status = server.call({'jsonrpc': '2.0', 'id': 5, 'method': 'tools/call', 'params': {
            'name': 'status', 'arguments': {}}})
        status_payload = json.loads(status['result']['content'][0]['text'])
        assert isinstance(status_payload, dict), status_payload

        browsed = server.call({'jsonrpc': '2.0', 'id': 6, 'method': 'tools/call', 'params': {
            'name': 'list_frames', 'arguments': {
                'since': (started - timedelta(hours=1)).strftime('%Y-%m-%dT%H:%M:%SZ'), 'limit': 3}}})
        browse_payload = json.loads(browsed['result']['content'][0]['text'])
        assert isinstance(browse_payload, dict) and browse_payload['frames'], browse_payload
        assert browse_payload['truncated'] in (True, False), browse_payload

        # A bad argument is an MCP tool error, not a crash; the server stays alive.
        bad = server.call({'jsonrpc': '2.0', 'id': 7, 'method': 'tools/call', 'params': {
            'name': 'search', 'arguments': {'words': 'Patrick', 'limit': 0}}})
        assert bad['result']['isError'] is True, bad
        assert 'limit' in bad['result']['content'][0]['text'], bad
        after = server.call({'jsonrpc': '2.0', 'id': 8, 'method': 'tools/call', 'params': {
            'name': 'search', 'arguments': {'words': 'Patrick', 'limit': 1}}})
        assert after['result']['isError'] is False, after

        # An option-looking search word must stay a literal positional query,
        # never a CLI flag (`--` end-of-options marker in the adapter).
        option_word = server.call({'jsonrpc': '2.0', 'id': 11, 'method': 'tools/call', 'params': {
            'name': 'search', 'arguments': {'words': '--out'}}})
        assert option_word['result']['isError'] is False, option_word
        assert 'total_matches' in json.loads(option_word['result']['content'][0]['text']), option_word

        # U1: compact, collapsed, budgeted responses (LS-3291).
        sys.path.insert(0, str(ROOT / 'scripts'))
        import replay_mcp as adapter

        # Clock-masked collapse: frames differing only in clock digits or
        # whitespace merge into one run; body-text differences stay separate.
        runs = adapter.collapse_frames([
            {'id': 1, 'timestamp_ms': 1000, 'timestamp': '2026-09-27T09:21:00.000Z', 'ocr_state': 'ready',
             'text': 'deploy 08:01:02 finished'},
            {'id': 2, 'timestamp_ms': 6000, 'timestamp': '2026-09-27T09:21:05.000Z', 'ocr_state': 'ready',
             'text': 'deploy 08:01:07 finished'},
            {'id': 3, 'timestamp_ms': 11000, 'timestamp': '2026-09-27T09:21:10.000Z', 'ocr_state': 'ready',
             'text': 'deploy  08:02:00  finished'},
            {'id': 4, 'timestamp_ms': 16000, 'timestamp': '2026-09-27T09:21:15.000Z', 'ocr_state': 'ready',
             'text': 'deploy finished'},
            {'id': 5, 'timestamp_ms': 21000, 'timestamp': '2026-09-27T09:21:20.000Z', 'ocr_state': 'ready',
             'text': 'invoice 12:05 sent'},
        ], adapter.DEFAULT_TEXT_CHARS)
        assert [(item['first_id'], item['last_id'], item['count']) for item in runs] == \
            [(1, 3, 3), (4, 4, 1), (5, 5, 1)], runs
        long_frame = {'id': 9, 'timestamp_ms': 9000, 'timestamp': '2026-09-27T09:22:00.000Z',
                      'ocr_state': 'ready', 'text': 'x' * 1000}
        assert len(adapter.collapse_frames([long_frame], adapter.DEFAULT_TEXT_CHARS)[0]['text']) \
            == adapter.DEFAULT_TEXT_CHARS

        # A 200-frame demo page must be compact, collapsed and within budget.
        big = server.call({'jsonrpc': '2.0', 'id': 20, 'method': 'tools/call', 'params': {
            'name': 'list_frames', 'arguments': {'limit': 200}}})
        big_payload = json.loads(big['result']['content'][0]['text'])
        assert isinstance(big_payload, dict) and big_payload['frames'], big_payload
        assert len(json.dumps(big_payload)) <= adapter.RESPONSE_BUDGET, \
            f'page exceeds budget: {len(json.dumps(big_payload))}'
        assert all(item['count'] >= 1 and item['first_id'] <= item['last_id']
                   for item in big_payload['frames']), big_payload['frames'][:3]
        # Frames whose body text differs stay separate items (the recorder already
        # coalesces exact duplicates, so clock-only runs are covered by the
        # collapse_frames unit test above).
        assert len({item['text'] for item in big_payload['frames']}) >= 2, big_payload['frames'][:3]
        assert all('text' not in item or len(item['text']) <= adapter.DEFAULT_TEXT_CHARS
                   for item in big_payload['frames'])
        if big_payload['truncated']:
            assert isinstance(big_payload['next_offset'], int) and big_payload['next_offset'] > 0
        # A resumed page must advance past the kept frames.
        if big_payload['truncated']:
            more = server.call({'jsonrpc': '2.0', 'id': 21, 'method': 'tools/call', 'params': {
                'name': 'list_frames', 'arguments': {'limit': 200, 'offset': big_payload['next_offset']}}})
            more_payload = json.loads(more['result']['content'][0]['text'])
            kept_ids = {item['first_id'] for item in big_payload['frames']}
            assert more_payload['frames'] and \
                all(item['first_id'] not in kept_ids for item in more_payload['frames']), more_payload

        # Compact get_moment: no geometry, neighbours as ids and times only;
        # detail=full keeps the stored lines.
        compact_moment = server.call({'jsonrpc': '2.0', 'id': 22, 'method': 'tools/call', 'params': {
            'name': 'get_moment', 'arguments': {'id': first_id, 'context_seconds': 5}}})
        compact_moment_payload = json.loads(compact_moment['result']['content'][0]['text'])
        assert compact_moment_payload['moment']['id'] == first_id, compact_moment_payload
        assert compact_moment_payload['moment']['text'], compact_moment_payload
        assert compact_moment_payload['moment']['image_path'], compact_moment_payload
        assert 'lines' not in compact_moment_payload['moment'], compact_moment_payload
        assert compact_moment_payload['neighbors'] and \
            all(set(n) <= {'id', 'timestamp', 'timestamp_ms'} for n in compact_moment_payload['neighbors']), \
            compact_moment_payload['neighbors'][:2]
        full_moment = server.call({'jsonrpc': '2.0', 'id': 23, 'method': 'tools/call', 'params': {
            'name': 'get_moment', 'arguments': {'id': first_id, 'context_seconds': 5, 'detail': 'full'}}})
        full_moment_payload = json.loads(full_moment['result']['content'][0]['text'])
        assert full_moment_payload['moment'].get('lines'), full_moment_payload

        # Compact search keeps the page metadata and bounds item text.
        compact_search = server.call({'jsonrpc': '2.0', 'id': 24, 'method': 'tools/call', 'params': {
            'name': 'search', 'arguments': {'words': 'Patrick', 'limit': 50}}})
        compact_search_payload = json.loads(compact_search['result']['content'][0]['text'])
        assert compact_search_payload['total_matches'] >= 1, compact_search_payload
        assert all(len(item['text']) <= adapter.DEFAULT_TEXT_CHARS
                   for item in compact_search_payload['results']), compact_search_payload

        # P2/P3 follow-ups (Jude run-p2fix-r1): discriminating boundary shapes,
        # meetings starvation, non-advancing coordinates, and the real tail.
        # search items of 154 text chars measured 60,179 on the pre-fix adapter
        # (over budget) and measure in on the fixed one; list items of 125
        # measured 60,172 on the pre-fix adapter.
        boundary_search = {'total_matches': 300, 'offset': 0, 'meetings': [], 'results': [
            {'id': i, 'timestamp': 'T', 'timestamp_ms': i, 'ocr_state': 'ready',
             'text': f'uniq {i} ' + 'q' * 144}
            for i in range(300)]}
        cut = adapter.compact_search(boundary_search, 'compact')
        assert len(json.dumps(cut)) <= adapter.RESPONSE_BUDGET, len(json.dumps(cut))
        boundary_list = adapter.compact_list(
            [{'id': i, 'timestamp': 'T', 'timestamp_ms': i, 'ocr_state': 'ready',
              'text': f'uniq {i} ' + 'r' * 115} for i in range(300)], '/arc', 'compact', 0)
        assert len(json.dumps(boundary_list)) <= adapter.RESPONSE_BUDGET
        # Meetings must never starve screen results: half the budget is reserved.
        mixed = {'total_matches': 300, 'offset': 0, 'meetings': [
            {'id': i, 'title': f'm{i}', 'matching_passages': ['p' * 120],
             'transcript': 't' * adapter.DEFAULT_TEXT_CHARS}
            for i in range(200)], 'results': [
            {'id': 10_000 + i, 'timestamp': 'T', 'timestamp_ms': i, 'ocr_state': 'ready',
             'text': f'uniq screen {i} ' + 's' * 150}
            for i in range(300)]}
        cutm = adapter.compact_search(mixed, 'compact')
        assert len(json.dumps(cutm)) <= adapter.RESPONSE_BUDGET, len(json.dumps(cutm))
        assert cutm['results'] and cutm['truncated'] is True, cutm
        # A single unpageable FULL-detail item must still advance the resume
        # coordinate (compact items are text-capped at 400 and always fit).
        unpageable = {'total_matches': 1, 'offset': 40, 'meetings': [], 'results': [
            {'id': 7, 'timestamp': 'T', 'timestamp_ms': 7, 'ocr_state': 'ready',
             'text': 'z' * (adapter.RESPONSE_BUDGET * 2)}]}
        cutu = adapter.compact_search(unpageable, 'full')
        assert cutu['results'] == [] and cutu['next_offset'] == 41 and \
            cutu['truncated'] is True, cutu
        # A deep offset makes the tail digits wide; the emitted page still fits.
        deep = adapter.compact_list(
            [{'id': i, 'timestamp': 'T', 'timestamp_ms': i, 'ocr_state': 'ready',
              'text': f'uniq {i} ' + 'w' * 178} for i in range(300)], '/arc', 'compact', 123456789)
        assert len(json.dumps(deep)) <= adapter.RESPONSE_BUDGET, len(json.dumps(deep))
        meetings_heavy = {'total_matches': 1, 'offset': 0, 'results': [], 'meetings': [
            {'id': i, 'title': f'm{i}', 'matching_passages': ['p' * 120],
             'transcript': 't' * adapter.DEFAULT_TEXT_CHARS}
            for i in range(300)]}
        cutm2 = adapter.compact_search(meetings_heavy, 'compact')
        assert len(json.dumps(cutm2)) <= adapter.RESPONSE_BUDGET, len(json.dumps(cutm2))
        assert cutm2['truncated'] is True, cutm2
        # Meetings-only page at the CLI-faithful entry shape, at the measured
        # boundary (108 entries emitted 60,041 bytes on the pre-tail-measure
        # adapter): the emitted page must fit including its tail keys.
        meetings_only = {'total_matches': 0, 'offset': 0, 'results': [], 'meetings': [
            {'id': i, 'title': f'Meeting {i}', 'started_at_ms': 1_700_000_000_000 + i,
             'time_known': True, 'duration_seconds': 1800 + i,
             'matching_passages': ['passage ' * 12],
             'transcript': 'transcript ' * 40}
            for i in range(108)]}
        cutm3 = adapter.compact_search(meetings_only, 'compact')
        assert len(json.dumps(cutm3)) <= adapter.RESPONSE_BUDGET, len(json.dumps(cutm3))

        unknown_tool = server.call({'jsonrpc': '2.0', 'id': 9, 'method': 'tools/call', 'params': {
            'name': 'nope', 'arguments': {}}})
        assert unknown_tool['result']['isError'] is True, unknown_tool

        missing = server.call({'jsonrpc': '2.0', 'id': 10, 'method': 'no/such/method'})
        assert missing['error']['code'] == -32601, missing

        server.proc.stdin.close()
        assert server.proc.wait(timeout=30) == 0, server.proc.stderr.read()


if __name__ == '__main__':
    main()
