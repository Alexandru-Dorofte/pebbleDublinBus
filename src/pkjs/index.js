var dep = require('./departures');

// Timetable site built daily by build_site.py (see README).
var SITE = 'https://alexandru-dorofte.github.io/pebbleDublinBus/';
var FEED_URL = 'https://api.nationaltransport.ie/gtfsr/v2/TripUpdates?format=json';
var MIN_GAP = 61 * 1000; // NTA usage policy: at most one GTFS-R call per 60 s per key
var NEARBY_M = 1000, NEARBY_MAX = 10, MAX_FAVS = 12;
var TICK = 20 * 1000;     // recount minutes; MIN_GAP still limits API calls
var STALE = 3 * 60 * 1000; // keep using the last good feed this long if a fetch fails
var DEFAULT_FAVS = ['4766'];

var feed = null, feedAt = 0, busy = false;
var lastFetch = 0; // persisted in localStorage, so relaunching can't make two calls inside 60 s
var index = null;         // [[code, name, lat, lon, routes], ...]
var indexError = '';
var byCode = {};
var here = null;          // {lat, lon}, 'off', or null while locating
var current = null;       // stop code open on the watch
var lastSent = '';        // last DATA sent, to skip identical updates (saves Bluetooth)
var timetables = {};      // code -> stop file, for this session

function store(k, v) {
  try {
    if (v === undefined) return localStorage.getItem(k);
    localStorage.setItem(k, v);
  } catch (e) {}
  return null;
}

function key() { return store('apiKey') || ''; } // set in the phone settings page only

// 'ok' | 'bad' | 'checking' — shown on the settings page; updated by every feed call.
function keyStatus(v) { return v === undefined ? store('keyStatus') || '' : store('keyStatus', v); }

function favs() {
  try { return JSON.parse(store('favs')) || DEFAULT_FAVS.slice(); } catch (e) { return DEFAULT_FAVS.slice(); }
}

function today() {
  var d = new Date();
  return d.getFullYear() + '-' + (d.getMonth() + 1) + '-' + d.getDate();
}

function send(msg, retried) {
  Pebble.sendAppMessage(msg, null, function () {
    if (!retried) setTimeout(function () { send(msg, true); }, 500); // watch busy: one retry
  });
}

function get(url, headers, done) {
  var xhr = new XMLHttpRequest();
  xhr.open('GET', url);
  for (var h in headers) xhr.setRequestHeader(h, headers[h]);
  xhr.timeout = 30000;
  xhr.onload = function () {
    if (xhr.status !== 200) return done(xhr.status);
    try { done(null, JSON.parse(xhr.responseText)); } catch (e) { done('bad json'); }
  };
  xhr.onerror = xhr.ontimeout = function () { done('network'); };
  xhr.send();
}

// Cached for the day in localStorage; the site rebuilds daily.
function cached(name, done) {
  var k = 'c:' + name, hit = store(k);
  if (hit) {
    try {
      var c = JSON.parse(hit);
      if (c.day === today()) return done(null, c.data);
    } catch (e) {}
  }
  get(SITE + name, {}, function (err, data) {
    if (err) { // offline: an older copy beats nothing
      try { return done(null, JSON.parse(hit).data); } catch (e) { return done(err); }
    }
    store(k, JSON.stringify({ day: today(), data: data }));
    done(null, data);
  });
}

function metres(a, lat, lon) {
  var x = (lon - a.lon) * Math.cos((lat + a.lat) * Math.PI / 360);
  var y = lat - a.lat;
  return Math.sqrt(x * x + y * y) * 111320;
}

function dist(m) {
  if (m < 1000) return Math.round(m / 10) * 10 + ' m';
  if (m < 10000) return (m / 1000).toFixed(1) + ' km';
  return Math.round(m / 1000) + ' km';
}

function stopDist(code) {
  var s = byCode[code];
  return s && here && here.lat !== undefined ? dist(metres(here, s[2], s[3])) : '';
}

// Home list for the watch, one stop per line:
// kind \t code \t name \t distance \t routes \t fav(1/0)   (kind F favourite, N nearby)
// or "S \t state" for the Nearby section: loading | locating | off | none
function sendHome() {
  var f = favs(), lines = [];
  // Keep the message inside the watch's inbox: the watch shows ~40 chars of routes anyway.
  var routes = function (r) { return r.length > 40 ? r.slice(0, 39) + '…' : r; };
  f.slice(0, MAX_FAVS).forEach(function (code) {
    var s = byCode[code];
    lines.push(['F', code, s ? s[1] : 'Stop ' + code, stopDist(code), s ? routes(s[4]) : '', 1].join('\t'));
  });
  if (indexError) lines.push('S\t' + indexError);
  else if (!index) lines.push('S\tloading');
  else if (here === 'off' || here === 'nofix') lines.push('S\t' + here);
  else if (!here) lines.push('S\tlocating');
  else {
    var near = index.map(function (s) { return [metres(here, s[2], s[3]), s]; })
      .filter(function (p) { return p[0] <= NEARBY_M; })
      .sort(function (a, b) { return a[0] - b[0]; })
      .slice(0, NEARBY_MAX);
    if (!near.length) lines.push('S\tnone');
    near.forEach(function (p) {
      var s = p[1];
      lines.push(['N', s[0], s[1], dist(p[0]), routes(s[4]), f.indexOf(s[0]) >= 0 ? 1 : 0].join('\t'));
    });
  }
  send({ HOME: lines.join('\n') });
}

function locate() {
  if (!navigator.geolocation) { here = 'off'; return sendHome(); }
  navigator.geolocation.getCurrentPosition(function (p) {
    here = { lat: p.coords.latitude, lon: p.coords.longitude };
    sendHome();
  }, function (err) {
    // Permission denied means location is off; a timeout keeps the last fix if we have one.
    if (err && err.code === 1) here = 'off';
    else if (!here || here === 'off') here = 'nofix';
    sendHome();
  }, { timeout: 15000, maximumAge: 60000 });
}

function loadIndex() {
  cached('stops.json', function (err, data) {
    indexError = err ? 'Could not load the stop list. Check your phone\'s connection' : '';
    if (err) return sendHome();
    index = data;
    byCode = {};
    index.forEach(function (s) { byCode[s[0]] = s; });
    sendHome();
  });
}

// Departures for the open stop. Header line: name \t code \t distance \t fav(1/0); then
// one line per bus: route \t headsign \t due \t live(1/0).
function render() {
  var t = timetables[current], now = Date.now(), s = t.stops[0];
  var head = [s.name, s.code, stopDist(current), favs().indexOf(current) >= 0 ? 1 : 0].join('\t');
  return [head].concat(dep.forStop(t, s, feed, now).map(function (b) {
    return [b.route, b.head, dep.due(b.ms, now), b.live ? 1 : 0].join('\t');
  })).join('\n');
}

function sendData() {
  var data = render();
  if (data === lastSent) return;
  lastSent = data;
  send({ DATA: data });
}

// The only place the NTA API is called. Returns false (no call) inside the 60 s policy gap.
function fetchFeed(done) {
  if (busy || !key() || Date.now() - lastFetch < MIN_GAP) return false;
  busy = true;
  lastFetch = Date.now();
  store('lastFetch', String(lastFetch));
  get(FEED_URL, { 'x-api-key': key(), 'Cache-Control': 'no-cache' }, function (err, data) {
    busy = false;
    if (err === 401 || err === 403) keyStatus('bad');
    else if (!err) { keyStatus('ok'); feed = dep.indexFeed(data); feedAt = Date.now(); }
    else console.log('NTA feed: ' + err); // keep the last good feed until STALE, then timetable only
    done(err);
  });
  return true;
}

function refresh() {
  if (!current || !timetables[current]) return;
  if (feed && Date.now() - feedAt > STALE) feed = null;
  if (!key()) return send({ STATUS: 'Add your NTA API key in the app settings on your phone' });
  var calling = fetchFeed(function (err) {
    if (!current || !timetables[current]) return; // user left, or the stop's timetable is still loading
    if (err === 401 || err === 403) return send({ STATUS: 'NTA API key rejected. Check settings' });
    sendData();
  });
  if (!calling && !busy) sendData(); // inside the 60 s gap: recount minutes, no call
}

function open(code) {
  current = code;
  lastSent = '';
  if (timetables[code]) return refresh();
  get(SITE + 's/' + code + '.json', {}, function (err, data) { // up to ~200 KB; kept in memory for this session
    if (current !== code) return;
    if (err) return send({ STATUS: err === 404 ? 'No buses found for stop ' + code : 'Could not load the timetable' });
    timetables[code] = data;
    refresh();
  });
}

function toggleFav(code) {
  var f = favs(), i = f.indexOf(code);
  if (i >= 0) f.splice(i, 1); else f.push(code);
  store('favs', JSON.stringify(f));
  var msg = { TOAST: i >= 0 ? 'Removed' : 'Saved' };
  if (current === code && timetables[code]) msg.DATA = lastSent = render();
  send(msg);
  sendHome();
}

Pebble.addEventListener('ready', function () {
  lastFetch = +store('lastFetch') || 0;
  loadIndex();
  locate();
  setInterval(refresh, TICK);
});

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload;
  if (p.OPEN !== undefined) open(String(p.OPEN));
  if (p.FAV !== undefined) toggleFav(String(p.FAV));
  if (p.REFRESH !== undefined) { lastSent = ''; refresh(); } // watch shows "…" until it hears back
  if (p.CLOSE !== undefined) { current = null; locate(); } // back on the home screen: re-check location
});

Pebble.addEventListener('showConfiguration', function () {
  var list = favs().map(function (code) {
    var s = byCode[code];
    return { code: code, name: s ? s[1] : '', routes: s ? s[4] : '' };
  });
  var k = key(), st = keyStatus();
  var keyInfo = k ? { full: k, last4: k.slice(-4), status: st || 'checking' } : null; // page runs only on your phone
  var html = '<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">' +
    '<style>body{font-family:-apple-system,Roboto,sans-serif;margin:0;background:#f4f4f4;color:#111}' +
    'header{background:#018555;color:#fff;padding:16px;font-size:20px;font-weight:bold}main{padding:16px}' +
    'h3{margin:20px 0 8px;font-size:15px}input{font-size:16px;padding:10px;border:1px solid #bbb;border-radius:6px;box-sizing:border-box;width:100%}' +
    '.row{display:flex;align-items:center;gap:8px;background:#fff;border-radius:6px;padding:10px;margin:6px 0}.row div{flex:1}' +
    '.row small{color:#555;display:block}button{font-size:15px;border:0;border-radius:6px;padding:10px 14px;font-weight:bold}' +
    '.rm{background:#eee}.add{background:#018555;color:#fff}.save{background:#FFE600;width:100%;margin-top:24px;font-size:17px;padding:14px}' +
    'p{color:#555;font-size:13px}.keycard{background:#fff;border-radius:6px;padding:10px;margin:6px 0}' +
    '.key{font-family:monospace;font-size:14px;height:22px;line-height:22px;white-space:nowrap;overflow-x:auto}' +
    '.keybar{display:flex;align-items:center;gap:6px;margin-top:6px}.keybar .badge{flex:1;margin:0}#cp{flex:0 0 auto;color:#018555}' +
    '.badge{display:inline-block;margin-top:4px;font-size:14px;font-weight:bold}.ok{color:#018555}.bad{color:#c62828}.checking{color:#555}' +
    '.x{background:#eee;font-size:15px;padding:8px 0;width:58px;flex:0 0 58px}</style></head><body><header>Dublin Bus</header><main>' +
    '<h3>NTA API key</h3><div id="keybox"></div>' +
    '<p>Free from developer.nationaltransport.ie. Saved on your phone only.</p>' +
    '<h3>Favourite stops</h3><div id="favs"></div>' +
    '<div class="row"><input id="n" inputmode="numeric" placeholder="Stop number, e.g. 4766"><button class="add" onclick="add()">Add</button></div>' +
    '<p>The number is on the stop\'s pole and in the TFI Live app.</p>' +
    '<button class="save" onclick="save()">Save to watch</button>' +
    '<p>Data: National Transport Authority, CC BY 4.0, provided as is.</p></main><script>' +
    'var clearKey=false,keyInfo=' + JSON.stringify(keyInfo).replace(/</g, '\\u003c') + ';' +
    'var favs=' + JSON.stringify(list).replace(/</g, '\\u003c').replace(/\u2028/g, '\\u2028').replace(/\u2029/g, '\\u2029') + ';' +
    'function e(s){return String(s).replace(/[&<>"\']/g,function(c){return "&#"+c.charCodeAt(0)+";"})}' +
    'function draw(){document.getElementById("favs").innerHTML=favs.map(function(f,i){return "<div class=row><div><b>"+e(f.name||"Stop "+f.code)+"</b><small>"+e(f.code)+(f.routes?" · "+e(f.routes):"")+"</small></div><button class=rm onclick=\\"rm("+i+")\\">Remove</button></div>"}).join("")||"<p>No favourites yet.</p>"}' +
    'function rm(i){favs.splice(i,1);draw()}' +
    'var badges={ok:"✓ Working",bad:"✗ Rejected. Check the key",checking:"Checking… (open a stop on your watch)"};' +
    'var shown=false;' +
    'function drawKey(){var b=document.getElementById("keybox");if(!keyInfo){b.innerHTML="<input id=k type=password autocomplete=off placeholder=\\"Paste your API key\\">";return}' +
    'b.innerHTML="<div class=keycard><div id=kv class=key></div><div class=keybar><span class=\\"badge "+keyInfo.status+"\\">"+badges[keyInfo.status]+"</span><span id=cp class=badge></span>"+' +
    '"<button id=eye class=x aria-label=\\"Show key\\" onclick=\\"toggleKey()\\"></button><button class=x onclick=\\"copyKey()\\">Copy</button>"+' +
    '"<button class=x aria-label=\\"Remove key\\" onclick=\\"clearKey=true;keyInfo=null;drawKey()\\">✕</button></div></div>";showKey()}' +
    // The eye only swaps text: the card keeps the same size.
    'function showKey(){document.getElementById("kv").textContent=shown?keyInfo.full:"•••• •••• •••• "+keyInfo.last4;document.getElementById("eye").textContent=shown?"Hide":"Show"}' +
    'function toggleKey(){shown=!shown;showKey()}' +
    'function copyKey(){var done=function(){document.getElementById("cp").textContent="Copied"};' +
    'if(navigator.clipboard&&navigator.clipboard.writeText){navigator.clipboard.writeText(keyInfo.full).then(done,sel)}else sel();' +
    'function sel(){shown=true;showKey();var r=document.createRange();r.selectNodeContents(document.getElementById("kv"));var s=window.getSelection();s.removeAllRanges();s.addRange(r);' +
    'try{if(document.execCommand("copy"))return done()}catch(x){}document.getElementById("cp").textContent="Tap and hold to copy"}}' +
    'function add(){var n=document.getElementById("n").value.replace(/\\D/g,"");if(n&&!favs.some(function(f){return f.code==n}))favs.push({code:n});document.getElementById("n").value="";draw()}' +
    'function save(){var k=document.getElementById("k");location.href="pebblejs://close#"+encodeURIComponent(JSON.stringify({key:k?k.value.trim():"",clearKey:clearKey,favs:favs.map(function(f){return f.code})}))}' +
    'draw();drawKey()</script></body></html>';
  Pebble.openURL('data:text/html;charset=utf-8,' + encodeURIComponent(html));
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  try {
    var r = JSON.parse(decodeURIComponent(e.response));
    if (r.clearKey) { try { localStorage.removeItem('apiKey'); } catch (err) {} keyStatus(''); feed = null; }
    if (r.key) {
      store('apiKey', r.key);
      keyStatus('checking');
      fetchFeed(function () {}); // check it now if the 60 s gap allows, else on the next refresh
    }
    if (Array.isArray(r.favs)) store('favs', JSON.stringify(r.favs.filter(function (c) { return /^\d+$/.test(c); })));
  } catch (err) { return; }
  sendHome();
  refresh();
});
