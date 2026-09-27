#!/usr/bin/env python3
"""Structured recall CLI regressions; synthetic/offscreen only."""
import json
import os
from datetime import datetime, timedelta, timezone
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BIN = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / 'build/replay')
ENV = dict(os.environ, OMP_THREAD_LIMIT='1', QT_QPA_PLATFORM='offscreen')


def run(*args, ok=True):
    proc = subprocess.run([BIN, *args], env=ENV, capture_output=True, text=True, timeout=120)
    if ok:
        assert proc.returncode == 0, proc.stderr
    return proc


def main():
    with tempfile.TemporaryDirectory(prefix='replay-recall-') as temporary:
        # Keep any stray relative output inside the test sandbox.
        os.chdir(temporary)
        started = datetime.now(timezone.utc)
        dataset = Path(temporary) / 'archive'
        # Fictional synthetic workload only; no real screen content is involved.
        run('demo', '--dir', str(dataset), '--frames', '8', '--interval', '.25',
            '--width', '960', '--height', '540', '--max-mib', '64')
        # Give the demo timestamps room to sit between the range bounds below.
        earlier = (started - timedelta(hours=1)).strftime('%Y-%m-%dT%H:%M:%SZ')
        later = (datetime.now(timezone.utc) + timedelta(hours=1)).strftime('%Y-%m-%dT%H:%M:%SZ')

        found = json.loads(run('recall', 'Patrick', '--dir', str(dataset)).stdout)
        assert found['schema_version'] == 1, found
        assert found['total_matches'] >= 1 and found['results'], found
        assert found['source'] == 'screen' and found['order'] == 'chronological', found
        first = found['results'][0]
        assert first['id'] > 0 and first['timestamp_ms'] > 0 and first['timestamp'].endswith('Z'), first
        assert first['ocr_state'] in ('ready', 'pending', 'failed', 'disabled'), first
        assert set(found['coverage']) >= {'pending', 'failed', 'ready', 'total', 'gaps'}, found
        assert found['coverage']['total'] >= 1 and found['coverage']['gaps'] == [], found['coverage']

        page_one = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--limit', '1').stdout)
        page_two = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--limit', '1', '--offset', '1').stdout)
        assert len(page_one['results']) == 1 and len(page_two['results']) == 1, (page_one, page_two)
        assert page_one['results'][0]['id'] != page_two['results'][0]['id'], 'paging did not advance'
        assert page_two['offset'] == 1, page_two

        rank = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--order', 'rank').stdout)
        assert rank['total_matches'] == found['total_matches'], (rank, found)
        assert {r['id'] for r in rank['results']} == {r['id'] for r in found['results']}, 'rank changed the result set'

        window = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--since', earlier, '--until', later).stdout)
        assert window['total_matches'] == found['total_matches'], (window, found)
        future = json.loads(run('recall', 'Patrick', '--dir', str(dataset),
                                '--since', str(int(datetime.now(timezone.utc).timestamp() * 1000) + 3_600_000)).stdout)
        assert future['total_matches'] == 0 and future['results'] == [], future

        meetings_only = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--source', 'meetings').stdout)
        assert meetings_only['total_matches'] == 0 and meetings_only['meetings'] == [], meetings_only
        everything = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--source', 'all').stdout)
        assert everything['total_matches'] == found['total_matches'], everything
        assert everything['meetings_total_matches'] == 0, everything

        moment = json.loads(run('recall', '--dir', str(dataset), '--id', str(first['id']),
                                '--context-seconds', '5').stdout)
        assert moment['schema_version'] == 1 and moment['moment']['id'] == first['id'], moment
        assert moment['moment']['text'] == first['text'], moment
        assert moment['moment']['lines'] and 'x' in moment['moment']['lines'][0], moment['moment']['lines'][:1]
        neighbor_ids = {n['id'] for n in moment['neighbors']}
        assert first['id'] not in neighbor_ids, moment['neighbors']
        assert len(moment['neighbors']) >= 1, moment['neighbors']
        # --out is opt-in; the parser default must not trigger extraction.
        assert not Path('runs/extracted.png').exists(), 'moment fetch wrote the default extraction file'

        target = Path(temporary) / 'evidence.png'
        run('recall', '--dir', str(dataset), '--id', str(first['id']), '--out', str(target))
        assert target.stat().st_size > 0, 'recall did not extract the moment image'

        listed = json.loads(run('list', '--dir', str(dataset), '--since', earlier, '--until', later).stdout)
        assert listed, 'time-range list returned nothing'
        empty = json.loads(run('list', '--dir', str(dataset),
                               '--since', str(int(datetime.now(timezone.utc).timestamp() * 1000) + 3_600_000)).stdout)
        assert empty == [], empty

        for bad in (['recall', 'Patrick', '--dir', str(dataset), '--order', 'relevance'],
                    ['recall', 'Patrick', '--dir', str(dataset), '--source', 'audio'],
                    ['recall', 'Patrick', '--dir', str(dataset), '--since', 'not-a-time'],
                    ['recall', 'Patrick', '--dir', str(dataset), '--since', later, '--until', earlier],
                    ['recall', '--dir', str(dataset)],
                    ['recall', '--dir', str(dataset), '--id', '99999']):
            proc = run(*bad, ok=False)
            assert proc.returncode != 0, bad
    print('PASS recall CLI: search paging/order/ranges, sources, moment fetch, extraction, list ranges, validation')


if __name__ == '__main__':
    main()
