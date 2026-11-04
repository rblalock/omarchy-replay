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
        assert page_one['has_more'] and page_one['next_offset'] == 1, page_one

        # A naive pager following next_offset must terminate; past the end is an
        # empty page, never the viewer's clamped last page.
        seen, offset = [], 0
        for _ in range(found['total_matches'] + 2):
            page = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--limit', '1',
                                  '--offset', str(offset)).stdout)
            seen += [r['id'] for r in page['results']]
            if page['next_offset'] is None:
                assert not page['has_more'], page
                break
            offset = page['next_offset']
        else:
            raise AssertionError('recall paging did not terminate')
        assert len(seen) == len(set(seen)) == found['total_matches'], seen
        past = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--limit', '2',
                              '--offset', str(found['total_matches'] + 5)).stdout)
        assert past['results'] == [] and past['next_offset'] is None and not past['has_more'], past
        assert past['offset'] == found['total_matches'] + 5, past

        rank = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--order', 'rank').stdout)
        assert rank['total_matches'] == found['total_matches'], (rank, found)
        assert {r['id'] for r in rank['results']} == {r['id'] for r in found['results']}, 'rank changed the result set'

        window = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--since', earlier, '--until', later).stdout)
        assert window['total_matches'] == found['total_matches'], (window, found)
        future = json.loads(run('recall', 'Patrick', '--dir', str(dataset),
                                '--since', str(int(datetime.now(timezone.utc).timestamp() * 1000) + 3_600_000)).stdout)
        assert future['total_matches'] == 0 and future['results'] == [], future

        # Overlap semantics: a moment first seen before the window but observed
        # inside it belongs to the window, in search, list and coverage alike.
        frames = json.loads(run('list', '--dir', str(dataset)).stdout)
        lasting = next(f for f in frames if f['last_timestamp_ms'] - f['timestamp_ms'] >= 100 and 'Patrick' in f['text'])
        inside = [str(lasting['timestamp_ms'] + 50), str(lasting['last_timestamp_ms'] - 10)]
        overlap = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--since', inside[0], '--until', inside[1]).stdout)
        assert [r['id'] for r in overlap['results']] == [lasting['id']] and overlap['total_matches'] == 1, overlap
        assert overlap['coverage']['total'] == 1, overlap['coverage']
        listed_overlap = json.loads(run('list', '--dir', str(dataset), '--since', inside[0], '--until', inside[1]).stdout)
        assert [f['id'] for f in listed_overlap] == [lasting['id']], listed_overlap
        assert set(found['coverage']) >= {'gap_count', 'gaps_truncated'}, found['coverage']

        meetings_only = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--source', 'meetings').stdout)
        assert meetings_only['total_matches'] == 0 and meetings_only['meetings'] == [], meetings_only
        everything = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--source', 'all').stdout)
        assert everything['total_matches'] == found['total_matches'], everything
        assert everything['meetings_total_matches'] == 0, everything
        assert everything['meetings_limit'] == 20 and everything['meetings_next_offset'] is None, everything
        wide = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--source', 'all', '--limit', '500').stdout)
        assert wide['meetings_limit'] == 100, wide

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
                    ['recall', '--dir', str(dataset), '--id', '99999'],
                    # Times: negative, epoch seconds, a bare year, pre-1970.
                    ['recall', 'Patrick', '--dir', str(dataset), '--until', '-1'],
                    ['recall', 'Patrick', '--dir', str(dataset), '--since', str(int(started.timestamp()))],
                    ['recall', 'Patrick', '--dir', str(dataset), '--since', '2026'],
                    ['recall', 'Patrick', '--dir', str(dataset), '--until', '1969-06-01'],
                    ['list', '--dir', str(dataset), '--since', later, '--until', earlier],
                    # --id / --meeting-id never silently drop other options.
                    ['recall', 'Patrick', '--dir', str(dataset), '--id', str(first['id'])],
                    ['recall', '--dir', str(dataset), '--id', str(first['id']), '--source', 'all'],
                    ['recall', '--dir', str(dataset), '--id', str(first['id']), '--since', earlier],
                    ['recall', '--dir', str(dataset), '--id', str(first['id']), '--order', 'rank'],
                    ['recall', '--dir', str(dataset), '--id', str(first['id']), '--meeting-id', '1'],
                    ['recall', 'Patrick', '--dir', str(dataset), '--out', str(Path(temporary) / 'x.png')],
                    ['recall', '--dir', str(dataset), '--meeting-id', '1'],
                    ['recall', '--dir', str(dataset), '--meeting-id', '0']):
            proc = run(*bad, ok=False)
            assert proc.returncode != 0 and proc.stderr.strip(), bad

        # Zone-less dates and datetimes are local time; a bare --until date
        # covers the whole local day.
        today = datetime.now().astimezone().strftime('%Y-%m-%d')
        local_day = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--since', today, '--until', today).stdout)
        assert local_day['total_matches'] == found['total_matches'], local_day
        midnight = datetime.now().astimezone().replace(hour=0, minute=0, second=0, microsecond=0)
        assert local_day['since_ms'] == int(midnight.timestamp() * 1000), local_day
        assert local_day['until_ms'] == int((midnight + timedelta(days=1)).timestamp() * 1000) - 1, local_day
        zoneless = (datetime.now() - timedelta(hours=1)).strftime('%Y-%m-%dT%H:%M:%S')
        recent = json.loads(run('recall', 'Patrick', '--dir', str(dataset), '--since', zoneless).stdout)
        assert recent['total_matches'] == found['total_matches'], recent
    print('PASS recall CLI: search paging/termination/order, overlap ranges, sources, moment fetch, extraction, list ranges, time parsing, validation')


if __name__ == '__main__':
    main()
