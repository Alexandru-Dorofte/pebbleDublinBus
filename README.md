# pebbleDublinBus

Next buses at your Dublin Bus stop, on a Pebble Time 2 (emery) or Pebble Round 2 (gabbro).

Live times come from the free NTA GTFS-Realtime API. That feed only sends delays, so each stop's
timetable is baked into the app from TFI's free static GTFS, and the phone adds the live delays on top.
Rows marked live are realtime predictions. Unmarked rows are timetable only.

## Setup

1. Get a free API key at <https://developer.nationaltransport.ie>.
2. `cp .env.example .env` and put the key in `NTA_API_KEY=`. The build bakes it into the app, so
   **don't share the built `.pbw`**. You can also type a key in the app's settings on your phone, which
   overrides the one from `.env`.
3. Pick your stops by the number on the pole (default is 4766, Deerhaven):

   ```sh
   ./stops.py 4766 1358    # downloads ~110 MB of GTFS, writes src/pkjs/stops.json
   ```

   Re-run it when timetables change. The script prints the date the timetable is valid until.

## Building & running

```sh
node test.js                       # self-check for the departure logic
pebble build
pebble install --emulator emery    # or gabbro
pebble install --phone <ip>        # your watch, via the phone on the same Wi-Fi
```

Select refreshes. The app polls at most once every 61 s, per the
[NTA usage policy](https://developer.nationaltransport.ie/usagepolicy) (one call per 60 s per key).

Data: National Transport Authority, licensed CC BY 4.0, provided as is.
