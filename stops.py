#!/usr/bin/env python3
"""Bake the timetable for your stops into the app.

    ./stops.py 1358 7602        # stop numbers as shown on the pole

Downloads TFI's free static GTFS (the one that matches the GTFS-R feed),
keeps only the trips calling at your stops and writes src/pkjs/stops.json.
Re-run when timetables change, then `pebble build` and reinstall.
"""
import csv, io, json, sys, urllib.request, zipfile, os, datetime

GTFS_URL = 'https://www.transportforireland.ie/transitData/Data/GTFS_Realtime.zip'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'src', 'pkjs', 'stops.json')


def secs(t):
    h, m, s = t.split(':')
    return int(h) * 3600 + int(m) * 60 + int(s)


def rows(z, name):
    with z.open(name) as f:
        yield from csv.DictReader(io.TextIOWrapper(f, 'utf-8-sig'))


def bake(z, codes):
    stops = {}
    for r in rows(z, 'stops.txt'):
        if r['stop_code'] in codes:
            stops[r['stop_id']] = {'code': r['stop_code'], 'name': r['stop_name'], 'trips': []}
    missing = set(codes) - {s['code'] for s in stops.values()}
    if missing:
        sys.exit('Unknown stop number(s): ' + ', '.join(sorted(missing)))

    # One pass over stop_times: calls at our stops + each trip's first departure
    # (start time lets us match GTFS-R trips even if trip_ids get renumbered).
    calls, start = [], {}
    for r in rows(z, 'stop_times.txt'):
        t, seq = r['trip_id'], int(r['stop_sequence'])
        if t not in start or seq < start[t][0]:
            start[t] = (seq, r['departure_time'])
        if r['stop_id'] in stops:
            calls.append((r['stop_id'], t, secs(r['departure_time']), seq))
    want = {c[1] for c in calls}
    start = {t: v[1] for t, v in start.items() if t in want}

    trips = {r['trip_id']: r for r in rows(z, 'trips.txt') if r['trip_id'] in want}
    short = {r['route_id']: r['route_short_name'] for r in rows(z, 'routes.txt')}
    sids = {trips[t]['service_id'] for t in want}
    services = {}
    for r in rows(z, 'calendar.txt'):
        if r['service_id'] in sids:
            services[r['service_id']] = {
                'days': [int(r[d]) for d in ('monday', 'tuesday', 'wednesday', 'thursday', 'friday', 'saturday', 'sunday')],
                'start': r['start_date'], 'end': r['end_date'], 'add': [], 'rm': []}
    for r in rows(z, 'calendar_dates.txt'):
        if r['service_id'] in sids:
            s = services.setdefault(r['service_id'], {'days': [0] * 7, 'start': r['date'], 'end': r['date'], 'add': [], 'rm': []})
            s['add' if r['exception_type'] == '1' else 'rm'].append(r['date'])

    routes, heads = [], []
    def idx(lst, v):
        if v not in lst:
            lst.append(v)
        return lst.index(v)

    for stop_id, t, dep, seq in calls:
        tr = trips[t]
        stops[stop_id]['trips'].append([dep, idx(routes, tr['route_id']), idx(heads, tr['trip_headsign']),
                                        tr['service_id'], t, seq, start[t], int(tr['direction_id'] or 0)])
    order = {c: i for i, c in enumerate(codes)}
    out = sorted(({'id': k, **v} for k, v in stops.items()), key=lambda s: order[s['code']])
    for s in out:
        s['trips'].sort()
    return {
        'built': datetime.date.today().strftime('%Y%m%d'),
        'end': max(s['end'] for s in services.values()),
        # route_id -> short name for all routes: GTFS-R ADDED trips only carry route_id
        'routes': [[r, short.get(r, '?')] for r in routes],
        'allRoutes': {r: n for r, n in short.items()},
        'heads': heads, 'services': services, 'stops': out,
    }


if __name__ == '__main__':
    codes = sys.argv[1:]
    if not codes:
        sys.exit(__doc__)
    print('Downloading', GTFS_URL, '(~110 MB)...')
    data = urllib.request.urlopen(GTFS_URL).read()
    print('Reading timetable (takes a minute)...')
    out = bake(zipfile.ZipFile(io.BytesIO(data)), codes)
    with open(OUT, 'w') as f:
        json.dump(out, f, separators=(',', ':'), ensure_ascii=False)
    for s in out['stops']:
        print(f"  {s['code']} {s['name']}: {len(s['trips'])} timetabled calls")
    print(f'Wrote {OUT} ({os.path.getsize(OUT) // 1024} KB), valid until {out["end"]}')
