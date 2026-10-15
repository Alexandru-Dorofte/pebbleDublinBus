var dep = require('./departures');
var data = require('./stops.json');
var env = require('./env.json'); // written by wscript from .env

var FEED_URL = 'https://api.nationaltransport.ie/gtfsr/v2/TripUpdates?format=json';
var MIN_GAP = 61 * 1000; // NTA usage policy: at most one GTFS-R call per 60 s per key
var feed = null, lastFetch = 0, busy = false;

function key() {
  try { return localStorage.getItem('apiKey') || env.apiKey; } catch (e) { return env.apiKey; }
}

function send(msg) {
  Pebble.sendAppMessage(msg, null, function () { console.log('send failed'); });
}

// One string for the watch: stops separated by \x1e, rows by \n, fields by \t.
// Stop line: "name\tnumber"; bus line: "route\thead\tdue\tlive(1/0)".
function render() {
  var now = Date.now();
  return data.stops.map(function (s) {
    return [s.name + '\t' + s.code].concat(dep.forStop(data, s, feed, now).map(function (b) {
      return [b.route, b.head, dep.due(b.ms, now), b.live ? 1 : 0].join('\t');
    })).join('\n');
  }).join('\x1e');
}

function refresh() {
  if (!key()) return send({ STATUS: 'Add your NTA API key in the app settings on your phone' });
  if (busy) return;
  if (Date.now() - lastFetch < MIN_GAP) return send({ DATA: render() }); // recount minutes, no call
  busy = true;
  lastFetch = Date.now();
  var xhr = new XMLHttpRequest();
  xhr.open('GET', FEED_URL);
  xhr.setRequestHeader('x-api-key', key());
  xhr.setRequestHeader('Cache-Control', 'no-cache');
  xhr.timeout = 30000;
  xhr.onload = function () {
    busy = false;
    if (xhr.status !== 200) {
      // Timetable still works without live data; say so instead of showing nothing.
      console.log('NTA HTTP ' + xhr.status);
      if (xhr.status === 401 || xhr.status === 403) return send({ STATUS: 'NTA API key rejected. Check settings' });
      feed = null;
      return send({ DATA: render() });
    }
    try { feed = JSON.parse(xhr.responseText); } catch (e) { feed = null; }
    send({ DATA: render() });
  };
  xhr.onerror = xhr.ontimeout = function () {
    busy = false;
    feed = null;
    send({ DATA: render() }); // offline: timetable only
  };
  xhr.send();
}

Pebble.addEventListener('ready', function () {
  refresh();
  setInterval(refresh, MIN_GAP);
});

Pebble.addEventListener('appmessage', function () { refresh(); }); // Select = refresh

Pebble.addEventListener('showConfiguration', function () {
  var html = '<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">' +
    '<style>body{font-family:sans-serif;padding:16px;background:#fff;color:#111}input,button{font-size:16px;width:100%;padding:10px;box-sizing:border-box;margin:8px 0}' +
    'button{background:#ffaa00;border:0;font-weight:bold}small{color:#555}</style></head><body>' +
    '<h2>Dublin Bus</h2><label>NTA API key<input id="k" autocomplete="off" value="' + key().replace(/[^\w-]/g, '') + '"></label>' +
    '<small>Free from developer.nationaltransport.ie (primary or secondary key).</small>' +
    '<p><small>Stops: ' + data.stops.map(function (s) { return s.code + ' ' + s.name; }).join(', ').replace(/[<>&"]/g, '') +
    '. To change stops run <b>./stops.py</b> and reinstall.</small></p>' +
    '<button onclick="location.href=\'pebblejs://close#\'+encodeURIComponent(document.getElementById(\'k\').value.trim())">Save</button>' +
    '<p><small>Data: National Transport Authority, CC BY 4.0, provided as is.</small></p></body></html>';
  Pebble.openURL('data:text/html,' + encodeURIComponent(html));
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  try { localStorage.setItem('apiKey', decodeURIComponent(e.response)); } catch (err) {}
  lastFetch = 0;
  refresh();
});
