# pebbleDublinBus

Next buses at Dublin stops, on a Pebble Time 2 (emery). Pebble Round 2 (gabbro) builds too.

- **Favourites:** saved stops at the top. Long-press Select on a stop to save or remove it, or manage them in the phone settings.
- **Nearby:** the 10 closest stops within 1 km, from your phone's GPS.
- **Live:** NTA GTFS-Realtime delays on top of the timetable, refreshed every 61 s while a stop is open.
  Rows marked *Live* are realtime predictions. *Scheduled* rows are timetable only.

## How it works

The NTA realtime feed only sends delays, so the app also needs each stop's timetable.
`.github/workflows/site.yml` runs `build_site.py` every night. The script splits TFI's free static GTFS into
`stops.json` (every Dublin-area stop with its location) and `s/<stop>.json` (that stop's next 8 days of timetable).
GitHub Pages hosts them, and the phone downloads only the stop you open.

## Setup

1. Get a free API key at <https://developer.nationaltransport.ie>.
2. Install the app, open its settings in the Pebble phone app and paste the key. The key is stored only on your phone
   and is never in this repo or the app file.

## Building & running

```sh
node test.js                       # self-check for the departure logic
pebble build
pebble install --emulator emery
pebble install --cloudpebble       # your watch, via the phone's Dev Connect
```

The app calls the realtime API at most once every 61 s, per the
[NTA usage policy](https://developer.nationaltransport.ie/usagepolicy) (one call per 60 s per key).

Data: National Transport Authority, licensed CC BY 4.0, provided as is.
