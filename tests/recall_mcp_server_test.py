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
sys.path.insert(0, str(ROOT / 'scripts'))
import replay_mcp as adapter  # noqa: E402


class Server:
    def __init__(self, archive):
        self.proc = subprocess.Popen(
            [sys.executable, '-B', str(SERVER)],
            env=dict(ENV, OMARCHY_REPLAY_ARCHIVE=archive),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def send(self, text):
        self.proc.stdin.write(text + '\n')
        self.proc.stdin.flush()

    def read(self):
        line = self.proc.stdout.readline()
        assert line, f'server closed: {self.proc.stderr.read()}'
        return json.loads(line)

    def request(self, payload):
        self.send(json.dumps(payload))
        return self.read()

    def value(self, payload):
        reply = self.request(payload)
        assert reply['result']['isError'] is False, reply
        return json.loads(reply['result']['content'][0]['text'])

    def stop(self):
        self.proc.stdin.close()
        assert self.proc.wait(timeout=30) == 0, self.proc.stderr.read()


def call(name, arguments, request_id=1):
    return {'jsonrpc': '2.0', 'id': request_id, 'method': 'tools/call',
            'params': {'name': name, 'arguments': arguments}}


def fake_cli_page(results=20, meetings=20, offset=10, meeting_total=100):
    """One recall page in the shape the CLI emits after the rebase."""
    return {
        'schema_version': 1, 'archive': '/arc', 'query': 'x', 'source': 'all',
        'order': 'chronological', 'limit': 20, 'offset': offset,
        'total_matches': 1000, 'results': [
            {'id': offset + i, 'timestamp': '2026-09-27T09:00:00.000Z', 'timestamp_ms': 1000 + i,
             'last_timestamp_ms': 1000 + i, 'observations': 1, 'text': f'unique screen {i} ' + 's' * 150,
             'ocr_state': 'ready', 'ocr_error': '', 'codec': 'webp', 'available': True,
             'archive_available': True} for i in range(results)],
        'has_more': True, 'next_offset': offset + results,
        'meetings_limit': 20, 'meetings_total_matches': meeting_total, 'meetings': [
            {'id': 1 + i, 'title': f'Meeting {i}', 'time_known': True, 'duration_seconds': 1800,
             'matching_passages': 1, 'passages': ['p' * 240], 'started_at_ms': 1_700_000_000_000 + i,
             'started_at': '2026-09-27T08:00:00.000Z'} for i in range(meetings)],
        'meetings_has_more': True, 'meetings_next_offset': offset + meetings,
        'coverage': {'pending': 0, 'ready': 10, 'failed': 0, 'disabled': 0, 'total': 10,
                     'gaps': [], 'gap_count': 0, 'gaps_truncated': False},
    }


def unit_pages():
    # A page that fits keeps the CLI's paging fields untouched (the review's
    # `limit 2` regression: has_more true with next_offset null).
    untouched = adapter.search_page(fake_cli_page())
    assert untouched['has_more'] is True and untouched['next_offset'] == 30, untouched
    assert untouched['meetings_has_more'] is True and untouched['meetings_next_offset'] == 30, untouched
    assert untouched['text_chars'] == adapter.DEFAULT_TEXT_CHARS, untouched
    assert len(json.dumps(untouched)) <= adapter.RESPONSE_BUDGET

    # Under pressure, previews shrink first and paging still describes the
    # emitted page: offsets advance by exactly what was kept, never past it.
    raw = fake_cli_page(results=60, meetings=40, offset=20)
    tight = adapter.search_page(raw, budget=8000)
    assert len(json.dumps(tight)) <= 8000, len(json.dumps(tight))
    assert tight['results'] and tight['meetings'], tight
    assert tight['has_more'] is True and tight['next_offset'] == \
        20 + sum(item['count'] for item in tight['results']), tight
    assert tight['meetings_has_more'] is True and tight['meetings_next_offset'] == \
        20 + len(tight['meetings']), tight
    raw_meetings = [meeting['id'] for meeting in raw['meetings']]
    assert [meeting['id'] for meeting in tight['meetings']] == \
        raw_meetings[:len(tight['meetings'])], tight['meetings']
    assert tight['results'][0]['first_id'] == raw['results'][0]['id'], tight['results'][0]
    assert len(tight['meetings']) + (100 - tight['meetings_next_offset']) == 100 - 20, tight
    assert all(len(item['text']) <= tight['text_chars'] for item in tight['results']), tight['text_chars']

    frames = [{'id': i, 'timestamp': 'T', 'timestamp_ms': i, 'ocr_state': 'ready',
               'text': f'unique {i} ' + 'r' * 200} for i in range(300)]
    listing = adapter.list_page(frames, '/arc', 300, 40, budget=9000)
    assert len(json.dumps(listing)) <= 9000, len(json.dumps(listing))
    assert listing['frames'] and listing['has_more'] is True, listing
    assert listing['next_offset'] == 40 + sum(frame['count'] for frame in listing['frames']), listing
    assert 'schema_version' not in listing, listing

    moment = {'schema_version': 1, 'archive': '/arc', 'moment': {
        'id': 1, 'timestamp': 'T', 'timestamp_ms': 1, 'text': 'z' * 120000,
        'image_path': 'frames/1.webp', 'lines': [{'text': 'x', 'x': 1, 'y': 2}]},
        'neighbors': [{'id': i, 'timestamp': 'T', 'timestamp_ms': i, 'text': 'n' * 100}
                      for i in range(64)]}
    cut = adapter.moment_page(moment)
    assert len(json.dumps(cut)) <= adapter.RESPONSE_BUDGET, len(json.dumps(cut))
    assert cut['truncated'] is True and 'lines' not in cut['moment'], cut
    assert len(cut['moment']['text']) < 120000 and cut['neighbors'], cut

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
    ])
    assert [(item['first_id'], item['last_id'], item['count']) for item in runs] == \
        [(1, 3, 3), (4, 4, 1), (5, 5, 1)], runs
    assert runs[0]['text'] == 'deploy 08:01:02 finished', runs[0]
    long_text = {'id': 9, 'timestamp': 'T', 'timestamp_ms': 9, 'ocr_state': 'ready', 'text': 'x' * 1000}
    rendered = adapter.render(adapter.collapse_frames([long_text])[0], adapter.DEFAULT_TEXT_CHARS)
    assert len(rendered['text']) == adapter.DEFAULT_TEXT_CHARS and rendered['text_truncated'] is True


def main():
    unit_pages()
    with tempfile.TemporaryDirectory(prefix='replay-mcp-') as temporary:
        os.chdir(temporary)
        started = datetime.now(timezone.utc)
        dataset = Path(temporary) / 'archive'
        # Fictional synthetic workload only; no real screen content is involved.
        subprocess.run([BIN, 'demo', '--dir', str(dataset), '--frames', '60', '--interval', '.25',
                        '--width', '960', '--height', '540', '--max-mib', '64'],
                       env=ENV, capture_output=True, text=True, timeout=600)
        assert (dataset / 'ground-truth.json').exists(), 'demo archive was not created'

        server = Server(dataset)
        # An unsupported protocol version negotiates to a known one, never an echo.
        init = server.request({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
            'protocolVersion': '2099-01-01', 'capabilities': {}, 'clientInfo': {'name': 'test'}}})
        assert init['result']['serverInfo']['name'] == 'omarchy-replay-mcp', init
        assert init['result']['protocolVersion'] == adapter.PROTOCOL_VERSIONS[0], init
        echoed = server.request({'jsonrpc': '2.0', 'id': 99, 'method': 'initialize', 'params': {
            'protocolVersion': '2024-11-05'}})
        assert echoed['result']['protocolVersion'] == '2024-11-05', echoed

        # A notification is never answered: the next line is the ping reply, and
        # ping returns an empty result (not -32601).
        server.send(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}))
        ping = server.request({'jsonrpc': '2.0', 'id': 2, 'method': 'ping'})
        assert ping['id'] == 2 and ping['result'] == {}, ping

        listing = server.request({'jsonrpc': '2.0', 'id': 3, 'method': 'tools/list'})
        names = {tool['name'] for tool in listing['result']['tools']}
        assert names == {'search', 'list_frames', 'get_moment', 'status'}, names
        assert all('untrusted' in tool['description'].lower() for tool in listing['result']['tools'])

        page = server.value(call('search', {'words': 'Patrick', 'limit': 2}, 4))
        assert page['total_matches'] >= 2 and page['results'], page
        # Paging continuity: follow next_offset until it is null; the frames
        # consumed must equal total_matches, with no repeats and no stalls.
        seen, offset = [], 0
        for _ in range(page['total_matches'] + 2):
            current = server.value(call('search', {'words': 'Patrick', 'limit': 2, 'offset': offset}, 5))
            seen += [(item['first_id'], item['count']) for item in current['results']]
            if not current['has_more']:
                assert current['next_offset'] is None, current
                break
            assert current['next_offset'] > offset, current
            offset = current['next_offset']
        else:
            raise AssertionError('search paging did not terminate')
        assert sum(count for _, count in seen) == page['total_matches'], (seen, page['total_matches'])
        assert len({first for first, _ in seen}) == len(seen), seen

        past = server.value(call('search', {'words': 'Patrick', 'limit': 2,
                                            'offset': page['total_matches'] + 5}, 6))
        assert past['results'] == [] and past['next_offset'] is None and past['has_more'] is False, past

        earlier = (started - timedelta(hours=1)).strftime('%Y-%m-%dT%H:%M:%SZ')
        later = (datetime.now(timezone.utc) + timedelta(hours=1)).strftime('%Y-%m-%dT%H:%M:%SZ')
        window = server.value(call('search', {'words': 'Patrick', 'since': earlier, 'until': later}, 7))
        assert window['total_matches'] == page['total_matches'], window

        browsed = server.value(call('list_frames', {'since': earlier, 'limit': 3}, 8))
        assert set(browsed) == {'archive', 'frames', 'has_more', 'next_offset', 'text_chars'}, browsed
        assert browsed['frames'] and browsed['has_more'] is True and browsed['next_offset'] == 3, browsed
        resumed = server.value(call('list_frames', {'since': earlier, 'limit': 3, 'offset': 3}, 9))
        assert resumed['frames'] and resumed['frames'][0]['first_id'] > browsed['frames'][-1]['first_id'], resumed

        first_id = page['results'][0]['first_id']
        moment = server.value(call('get_moment', {'id': first_id, 'context_seconds': 5}, 10))
        assert moment['moment']['id'] == first_id, moment
        assert moment['moment']['image_path'], moment
        assert 'lines' not in moment['moment'], moment
        assert moment['neighbors'] and all(
            set(neighbour) == {'id', 'timestamp', 'timestamp_ms'} for neighbour in moment['neighbors'])
        assert moment['truncated'] is False, moment
        assert not Path('extracted.png').exists(), 'moment fetch must not extract images'

        status = server.value(call('status', {}, 11))
        assert isinstance(status, dict) and status, status

        # A bad argument is an MCP tool error, not a crash; the server stays alive.
        bad = server.request(call('search', {'words': 'Patrick', 'limit': 0}, 12))
        assert bad['result']['isError'] is True and 'limit' in bad['result']['content'][0]['text'], bad
        alive = server.value(call('search', {'words': 'Patrick', 'limit': 1}, 13))
        assert alive['results'], alive

        # Malformed requests answer with a protocol error and leave the loop running.
        # `params: null` is legal (no params) and must not crash the server.
        null_params = server.request({'jsonrpc': '2.0', 'id': 14, 'method': 'tools/list',
                                      'params': None})
        assert null_params['result']['tools'], null_params
        list_params = server.request({'jsonrpc': '2.0', 'id': 15, 'method': 'tools/list',
                                      'params': []})
        assert list_params['error']['code'] == -32602, list_params
        null_call = server.request({'jsonrpc': '2.0', 'id': 16, 'method': 'tools/call',
                                    'params': None})
        assert null_call['result']['isError'] is True, null_call
        malformed_args = server.request({'jsonrpc': '2.0', 'id': 17, 'method': 'tools/call',
                                        'params': {'name': 'search', 'arguments': []}})
        assert malformed_args['error']['code'] == -32602, malformed_args
        server.send('not json')
        assert server.read()['error']['code'] == -32700
        server.send('[1, 2]')
        assert server.read()['error']['code'] == -32600
        assert server.value(call('search', {'words': 'Patrick', 'limit': 1}, 18))['results']

        # An option-looking word stays a literal positional query, never a CLI flag.
        option_word = server.request(call('search', {'words': '--out'}, 19))
        assert option_word['result']['isError'] is False, option_word
        assert 'total_matches' in json.loads(option_word['result']['content'][0]['text']), option_word

        unknown_tool = server.request(call('nope', {}, 20))
        assert unknown_tool['result']['isError'] is True, unknown_tool
        missing = server.request({'jsonrpc': '2.0', 'id': 21, 'method': 'no/such/method'})
        assert missing['error']['code'] == -32601, missing

        server.stop()


if __name__ == '__main__':
    main()
