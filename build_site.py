#!/usr/bin/env python3
"""Build the timetable site the app reads from.

    ./build_site.py [GTFS_Realtime.zip] [out_dir]

Splits TFI's free static GTFS (the one matching the GTFS-R feed) into
  stops.json      every Dublin-area bus stop: [code, name, lat, lon, "70 · 270"]
  s/<code>.json   that stop's timetable for the next DAYS days
Run daily by .github/workflows/site.yml and published to GitHub Pages.
"""
import csv, datetime, io, json, os, sys, urllib.request, zipfile

GTFS_URL = 'https://www.transportforireland.ie/transitData/Data/GTFS_Realtime.zip'
DAYS = 8
BBOX = (53.15, 53.65, -6.65, -5.95)  # Dublin region: lat min/max, lon min/max


def rows(z, name):
    with z.open(name) as f:
        yield from csv.DictReader(io.TextIOWrapper(f, 'utf-8-sig'))


def secs(t):
    h, m, s = t.split(':')
    return int(h) * 3600 + int(m) * 60 + int(s)


def active_services(z, today):
    """service_id -> calendar entry, for services running on any of the next DAYS days."""
    days = [today + datetime.timedelta(d) for d in range(-1, DAYS)]
    keys = [d.strftime('%Y%m%d') for d in days]
    cal = {}
    for r in rows(z, 'calendar.txt'):
        cal[r['service_id']] = {
            'days': [int(r[d]) for d in ('monday', 'tuesday', 'wednesday', 'thursday', 'friday', 'saturday', 'sunday')],
            'start': r['start_date'], 'end': r['end_date'], 'add': [], 'rm': []}
    for r in rows(z, 'calendar_dates.txt'):
        s = cal.setdefault(r['service_id'], {'days': [0] * 7, 'start': r['date'], 'end': r['date'], 'add': [], 'rm': []})
        s['add' if r['exception_type'] == '1' else 'rm'].append(r['date'])

    def runs(s, d, k):
        if k in s['rm']:
            return False
        return k in s['add'] or (s['start'] <= k <= s['end'] and s['days'][d.weekday()] == 1)

    out = {}
    for sid, s in cal.items():
        if any(runs(s, d, k) for d, k in zip(days, keys)):
            # Only keep exception dates inside the window; the app never looks further.
            s['add'] = [k for k in s['add'] if k in keys]
            s['rm'] = [k for k in s['rm'] if k in keys]
            out[sid] = s
    return out


def build(z, today):
    services = active_services(z, today)
    short = {r['route_id']: r['route_short_name'] for r in rows(z, 'routes.txt') if r['route_type'] == '3'}
    trips = {r['trip_id']: (r['route_id'], r['service_id'], r['trip_headsign'], int(r['direction_id'] or 0))
             for r in rows(z, 'trips.txt') if r['service_id'] in services and r['route_id'] in short}
    stops = {}
    for r in rows(z, 'stops.txt'):
        lat, lon = float(r['stop_lat'] or 0), float(r['stop_lon'] or 0)
        if r['stop_code'] and BBOX[0] <= lat <= BBOX[1] and BBOX[2] <= lon <= BBOX[3]:
            stops[r['stop_id']] = {'code': r['stop_code'], 'name': r['stop_name'], 'lat': round(lat, 5),
                                   'lon': round(lon, 5), 'calls': []}

    start = {}
    for r in rows(z, 'stop_times.txt'):
        t = r['trip_id']
        if t not in trips:
            continue
        seq = int(r['stop_sequence'])
        if t not in start or seq < start[t][0]:
            start[t] = (seq, r['departure_time'])
        st = stops.get(r['stop_id'])
        if st is not None:
            st['calls'].append((secs(r['departure_time']), t, seq))

    files, index = {}, []
    # A few stop codes belong to several stop_ids; keep the busiest one.
    best = {}
    for stop_id, st in stops.items():
        kept = best.get(st['code'])
        if st['calls'] and (kept is None or len(st['calls']) > len(stops[kept]['calls'])):
            best[st['code']] = stop_id
    for stop_id in best.values():
        st = stops[stop_id]
        routes, heads, sids, out = [], [], set(), []
        for dep, t, seq in sorted(st['calls']):
            route_id, sid, head, d = trips[t]
            if route_id not in routes:
                routes.append(route_id)
            if head not in heads:
                heads.append(head)
            sids.add(sid)
            out.append([dep, routes.index(route_id), heads.index(head), sid, t, seq, start[t][1], d])
        names = sorted({short[r] for r in routes}, key=lambda n: (len(n), n))
        index.append([st['code'], st['name'], st['lat'], st['lon'], ' · '.join(names)])
        files[st['code']] = {
            'built': today.strftime('%Y%m%d'),
            'routes': [[r, short[r]] for r in routes], 'heads': heads,
            'allRoutes': {r: short[r] for r in routes},
            'services': {s: services[s] for s in sorted(sids)},
            'stops': [{'id': stop_id, 'code': st['code'], 'name': st['name'], 'trips': out}],
        }
    return index, files


def write(index, files, out):
    os.makedirs(os.path.join(out, 's'), exist_ok=True)
    def dump(obj, path):
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(obj, f, separators=(',', ':'), ensure_ascii=False)
    dump(index, os.path.join(out, 'stops.json'))
    for code, data in files.items():
        dump(data, os.path.join(out, 's', code + '.json'))


if __name__ == '__main__':
    src = sys.argv[1] if len(sys.argv) > 1 else None
    out = sys.argv[2] if len(sys.argv) > 2 else 'site'
    z = zipfile.ZipFile(src or io.BytesIO(urllib.request.urlopen(GTFS_URL).read()))
    index, files = build(z, datetime.date.today())
    write(index, files, out)
    print(f'{len(index)} stops written to {out}/')
