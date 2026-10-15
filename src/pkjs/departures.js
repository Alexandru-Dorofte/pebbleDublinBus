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
// trip_ids were renumbered or NTA sent a timetabled trip as ADDED.
function indexFeed(feed) {
  var byId = {}, byKey = {}, added = [];
  ((feed && feed.entity) || []).forEach(function (e) {
    var tu = e.trip_update;
    if (!tu || !tu.trip) return;
    var t = tu.trip;
    if (t.schedule_relationship === 'ADDED') added.push(tu);
    else if (t.trip_id) byId[t.trip_id] = tu;
    byKey[[t.route_id, t.direction_id || 0, t.start_date, t.start_time].join('|')] = tu;
  });
  return { byId: byId, byKey: byKey, added: added };
}

function ev(u) { return u.departure || u.arrival || {}; }

// Returns predicted epoch ms, or null when the bus won't call here
// (trip cancelled, stop skipped, or the bus already passed the stop).
function predict(tu, seq, sched) {
  if (!tu) return { ms: sched, live: false };
  if (tu.trip.schedule_relationship === 'CANCELED') return null;
  var ups = tu.stop_time_update || [], at = null, prev = null, first = Infinity;
  ups.forEach(function (u) {
    first = Math.min(first, u.stop_sequence);
    if (u.stop_sequence === seq) at = u;
    // Delay carries forward from the nearest earlier stop that has one (GTFS-R propagation).
    else if (u.stop_sequence < seq && ev(u).delay !== undefined && (!prev || u.stop_sequence > prev.stop_sequence)) prev = u;
  });
  if (ups.length && first > seq) return null; // updates start after our stop: bus has gone by
  if (at) {
    if (at.schedule_relationship === 'SKIPPED') return null;
    if (ev(at).time) return { ms: +ev(at).time * 1000, live: true };
    if (ev(at).delay !== undefined) return { ms: sched + ev(at).delay * 1000, live: true };
  }
  if (prev) return { ms: sched + ev(prev).delay * 1000, live: true };
  return { ms: sched, live: false };
}

function forStop(data, stop, feed, now) {
  var idx = feed && feed.byId ? feed : indexFeed(feed);
  var out = [], used = [];
  // Service days: yesterday (for 24:xx+ trips), today, tomorrow.
  [-1, 0, 1].forEach(function (off) {
    var noon = new Date(now);
    noon.setHours(12, 0, 0, 0);
    noon.setDate(noon.getDate() + off);
    var day = ymd(noon);
    var base = noon.getTime() - 12 * 3600 * 1000; // GTFS: times count from noon minus 12h
    stop.trips.forEach(function (t) {
      var sched = base + t[0] * 1000;
      if (sched < now - 2 * 3600 * 1000 || sched > now + WINDOW) return; // 2 h back: very late buses
      if (!runs(data.services[t[3]], noon)) return;
      var route = data.routes[t[1]];
      var tu = idx.byId[t[4]];
      if (tu && tu.trip.start_date && tu.trip.start_date !== day) tu = null; // another day's run
      tu = tu || idx.byKey[[route[0], t[7], day, t[6]].join('|')];
      if (tu) used.push(tu);
      var p = predict(tu, t[5], sched);
      if (p) out.push({ id: t[4] + '|' + day, route: route[1], head: data.heads[t[2]], ms: p.ms, live: p.live });
    });
  });
  idx.added.forEach(function (tu) {
    if (used.indexOf(tu) >= 0) return; // already shown as its timetabled trip
    (tu.stop_time_update || []).forEach(function (u) {
      if (u.stop_id !== stop.id || !ev(u).time || u.schedule_relationship === 'SKIPPED') return;
      out.push({ id: tu.trip.trip_id + '|' + tu.trip.start_date, route: data.allRoutes[tu.trip.route_id] || '?', head: '',
                 ms: +ev(u).time * 1000, live: true });
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
