// node test.js — self-check for the departure merge logic.
var assert = require('assert');
var dep = require('./src/pkjs/departures');

var now = new Date(2026, 9, 15, 8, 0, 0).getTime(); // Thu 15 Oct 2026 08:00
var T = function (hhmm) { return dep.hms(hhmm + ':00'); };
var every = { days: [1, 1, 1, 1, 1, 1, 1], start: '20261001', end: '20261231', add: [], rm: [] };
var data = {
  routes: [['1 39A', '39A'], ['1 C1', 'C1']], heads: ['Ongar', 'Adamstown'],
  allRoutes: { '1 N4': 'N4' },
  services: { a: every, wkend: { days: [0, 0, 0, 0, 0, 1, 1], start: '20261001', end: '20261231', add: [], rm: [] },
              off: { days: [1, 1, 1, 1, 1, 1, 1], start: '20261001', end: '20261231', add: [], rm: ['20261015'] } },
  stops: [],
};
// [dep, route, head, service, trip_id, seq, trip start, dir]
var stop = { id: 'S1', trips: [
  [T('08:05'), 0, 0, 'a', 'sched', 10, '07:30:00', 0],     // no live data
  [T('08:10'), 0, 0, 'a', 'atstop', 10, '07:35:00', 0],    // delay at our stop
  [T('08:12'), 1, 1, 'a', 'prop', 20, '07:40:00', 1],      // delay propagated from earlier stop
  [T('08:15'), 0, 0, 'a', 'passed', 5, '07:45:00', 0],     // updates start after our stop
  [T('08:20'), 0, 0, 'a', 'cancel', 10, '08:00:00', 0],
  [T('08:25'), 0, 0, 'a', 'skip', 10, '08:05:00', 0],
  [T('08:30'), 0, 0, 'wkend', 'weekend', 10, '08:10:00', 0], // not running Thursday
  [T('08:35'), 0, 0, 'off', 'removed', 10, '08:15:00', 0],   // calendar_dates removal
  [T('08:40'), 0, 0, 'a', 'renumbered', 10, '08:20:00', 0],  // matched by route/dir/start
  [T('07:00'), 0, 0, 'a', 'gone', 10, '06:30:00', 0],        // in the past
  [T('11:00'), 0, 0, 'a', 'far', 10, '10:30:00', 0],         // beyond 90 min
]};
var tu = function (id, rel, stus, extra) {
  var trip = { trip_id: id, schedule_relationship: rel || 'SCHEDULED', start_date: '20261015' };
  for (var k in extra) trip[k] = extra[k];
  return { trip_update: { trip: trip, stop_time_update: stus || [] } };
};
var feed = { entity: [
  tu('atstop', 0, [{ stop_sequence: 10, departure: { delay: 120 } }]),
  tu('prop', 0, [{ stop_sequence: 12, arrival: { delay: -60 } }, { stop_sequence: 30, arrival: { delay: 600 } }]),
  tu('passed', 0, [{ stop_sequence: 7, arrival: { delay: 0 } }]),
  tu('cancel', 'CANCELED'),
  tu('skip', 0, [{ stop_sequence: 10, schedule_relationship: 'SKIPPED' }]),
  tu('NEW_ID', 0, [{ stop_sequence: 3, arrival: { delay: 180 } }], { route_id: '1 39A', direction_id: 0, start_time: '08:20:00' }),
  tu(undefined, 'ADDED', [{ stop_sequence: 1, stop_id: 'S1', arrival: { time: String((now + 45 * 60000) / 1000) } }], { route_id: '1 N4' }),
]};

var rows = dep.forStop(data, stop, feed, now).map(function (b) {
  return [b.route, b.head, dep.due(b.ms, now), b.live];
});
assert.deepStrictEqual(rows, [
  ['39A', 'Ongar', '5 min', false],
  ['C1', 'Adamstown', '11 min', true],
  ['39A', 'Ongar', '12 min', true],
  ['39A', 'Ongar', '43 min', true],
  ['N4', '', '45 min', true],
]);

// No feed at all (fetch failed): timetable only, no crash.
assert.strictEqual(dep.forStop(data, stop, null, now).every(function (b) { return !b.live; }), true);

// ADDED entry that is really a timetabled trip: shown once, with its absolute time.
var dupFeed = { entity: [tu(undefined, 'ADDED', [{ stop_sequence: 10, stop_id: 'S1', departure: { time: String((now + 7 * 60000) / 1000) } }],
  { route_id: '1 39A', direction_id: 0, start_time: '07:30:00' })] };
var d = dep.forStop(data, { id: 'S1', trips: [stop.trips[0]] }, dupFeed, now);
assert.deepStrictEqual(d.map(function (b) { return [b.route, dep.due(b.ms, now), b.live]; }), [['39A', '7 min', true]]);

// Delay propagates past a SKIPPED earlier stop; another day's trip_id update is ignored.
var p2 = { entity: [tu('prop', 0, [{ stop_sequence: 12, arrival: { delay: 300 } }, { stop_sequence: 15, schedule_relationship: 'SKIPPED' }]),
  tu('sched', 'CANCELED', [], { start_date: '20261014' })] };
var r2 = dep.forStop(data, { id: 'S1', trips: [stop.trips[0], stop.trips[2]] }, p2, now);
assert.deepStrictEqual(r2.map(function (b) { return [b.route, dep.due(b.ms, now), b.live]; }), [['39A', '5 min', false], ['C1', '17 min', true]]);

// A bus running 70 min late still shows.
var late = { entity: [tu('gone', 0, [{ stop_sequence: 10, departure: { delay: 70 * 60 } }])] };
assert.strictEqual(dep.due(dep.forStop(data, { id: 'S1', trips: [stop.trips[9]] }, late, now)[0].ms, now), '10 min');

// Night bus timetabled as 24:30 on yesterday's service day.
var night = new Date(2026, 9, 15, 0, 20, 0).getTime();
var n = dep.forStop(data, { id: 'S1', trips: [[T('24:30'), 0, 0, 'a', 'n', 1, '24:00:00', 0]] }, { entity: [] }, night);
assert.strictEqual(dep.due(n[0].ms, night), '10 min');

assert.strictEqual(dep.due(now + 30000, now), 'Due');
assert.strictEqual(dep.due(now + 75 * 60000, now), '09:15');

// cached() from index.js, against fake storage and network: favourites (keepDays 6) open from an
// older copy at once; the stop list (keepDays 0) waits for today's copy.
(function () {
  var fn = require('fs').readFileSync(__dirname + '/src/pkjs/index.js', 'utf8').match(/function cached\([\s\S]*?\n}\n/)[0];
  var mem = {}, calls = 0, online = true, SITE = '';
  function store(k, v) { if (v === undefined) return mem[k] || null; mem[k] = v; }
  function today() { return '2026-10-15'; }
  function get(url, h, cb) { calls++; online ? cb(null, 'NEW') : cb('network'); }
  function saved(daysAgo) { mem['c:x'] = JSON.stringify({ day: 'old', at: Date.now() - daysAgo * 86400e3, data: 'OLD' }); }
  var cached = eval('(' + fn + ')');
  function run(keep) { var got = []; cached('x', keep, function (e, d) { got.push(e || d); }); return got; }
  assert.deepEqual(run(6), ['NEW']); assert.strictEqual(calls, 1);  // nothing saved: download
  assert.deepEqual(run(6), ['NEW']); assert.strictEqual(calls, 1);  // today's copy: no download
  saved(2); assert.deepEqual(run(6), ['OLD']);                       // older favourite: at once...
  assert.strictEqual(JSON.parse(mem['c:x']).data, 'NEW');            // ...and refreshed behind
  saved(2); assert.deepEqual(run(0), ['NEW']);                       // stop list waits for today's
  saved(9); online = false; assert.deepEqual(run(6), ['OLD']);       // too old but offline: fallback
  delete mem['c:x']; assert.deepEqual(run(6), ['network']);          // nothing at all: the error
})();
console.log('ok');
