// Merge the baked timetable (stops.json) with an NTA GTFS-R TripUpdates feed.
var WINDOW = 90 * 60 * 1000; // show buses due in the next 90 min
var MAX_ROWS = 10;

function ymd(d) {
  return '' + d.getFullYear() + ('0' + (d.getMonth() + 1)).slice(-2) + ('0' + d.getDate()).slice(-2);
}

function hms(s) {
  var p = s.split(':');
  return +p[0] * 3600 + +p[1] * 60 + +p[2];
}

function runs(svc, d) {
  var day = ymd(d);
  if (!svc || svc.rm.indexOf(day) >= 0) return false;
  if (svc.add.indexOf(day) >= 0) return true;
  return day >= svc.start && day <= svc.end && svc.days[(d.getDay() + 6) % 7] === 1;
}

// Index the feed by trip_id, and by route/direction/date/start time in case
// trip_ids were renumbered since stops.json was baked.
function indexFeed(feed) {
  var byId = {}, byKey = {}, added = [];
  ((feed && feed.entity) || []).forEach(function (e) {
    var tu = e.trip_update;
    if (!tu || !tu.trip) return;
    var t = tu.trip;
    if (t.schedule_relationship === 'ADDED') return added.push(tu);
    if (t.trip_id) byId[t.trip_id] = tu;
    byKey[[t.route_id, t.direction_id || 0, t.start_date, t.start_time].join('|')] = tu;
  });
  return { byId: byId, byKey: byKey, added: added };
}

// Returns predicted epoch ms, or null when the bus won't call here
// (trip cancelled, stop skipped, or the bus already passed the stop).
function predict(tu, seq, sched) {
  if (!tu) return { ms: sched, live: false };
  if (tu.trip.schedule_relationship === 'CANCELED') return null;
  var best = null, first = Infinity;
  (tu.stop_time_update || []).forEach(function (u) {
    first = Math.min(first, u.stop_sequence);
    if (u.stop_sequence <= seq && (!best || u.stop_sequence > best.stop_sequence)) best = u;
  });
  if (first > seq && first !== Infinity) return null; // updates start after our stop: bus has gone by
  if (best && best.stop_sequence === seq) {
    if (best.schedule_relationship === 'SKIPPED') return null;
    var ev = best.departure || best.arrival || {};
    if (ev.time) return { ms: +ev.time * 1000, live: true };
    if (ev.delay !== undefined) return { ms: sched + ev.delay * 1000, live: true };
  }
  // Earlier stop's delay carries forward (GTFS-R propagation rule).
  if (best) {
    var e = best.departure || best.arrival || {};
    if (e.delay !== undefined) return { ms: sched + e.delay * 1000, live: true };
  }
  return { ms: sched, live: false };
}

function forStop(data, stop, feed, now) {
  var idx = feed.byId ? feed : indexFeed(feed);
  var out = [];
  // Service days: yesterday (for 24:xx+ trips), today, tomorrow.
  [-1, 0, 1].forEach(function (off) {
    var noon = new Date(now);
    noon.setHours(12, 0, 0, 0);
    noon.setDate(noon.getDate() + off);
    var day = ymd(noon);
    var base = noon.getTime() - 12 * 3600 * 1000; // GTFS: times count from noon minus 12h
    stop.trips.forEach(function (t) {
      var sched = base + t[0] * 1000;
      if (sched < now - 30 * 60 * 1000 || sched > now + WINDOW) return;
      if (!runs(data.services[t[3]], noon)) return;
      var route = data.routes[t[1]];
      var tu = idx.byId[t[4]] || idx.byKey[[route[0], t[7], day, t[6]].join('|')];
      var p = predict(tu, t[5], sched);
      if (p) out.push({ route: route[1], head: data.heads[t[2]], ms: p.ms, live: p.live });
    });
  });
  idx.added.forEach(function (tu) {
    (tu.stop_time_update || []).forEach(function (u) {
      var ev = u.departure || u.arrival;
      if (u.stop_id !== stop.id || !ev || !ev.time || u.schedule_relationship === 'SKIPPED') return;
      out.push({ route: data.allRoutes[tu.trip.route_id] || '?', head: '', ms: +ev.time * 1000, live: true });
    });
  });
  return out
    .filter(function (b) { return b.ms >= now - 30 * 1000 && b.ms <= now + WINDOW; })
    .sort(function (a, b) { return a.ms - b.ms; })
    .slice(0, MAX_ROWS);
}

function due(ms, now) {
  var mins = Math.floor((ms - now) / 60000);
  if (mins < 1) return 'Due';
  if (mins < 60) return mins + ' min';
  var d = new Date(ms);
  return ('0' + d.getHours()).slice(-2) + ':' + ('0' + d.getMinutes()).slice(-2);
}

module.exports = { forStop: forStop, indexFeed: indexFeed, due: due, hms: hms };
