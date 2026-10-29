<?php
/*
 * Oldkid.net Online Play - rendezvous for the okplay DirectPlay provider.
 * ----------------------------------------------------------------------
 * Single file, flat-JSON store, no database. Deploy next to the old lobby:
 *     https://oldkid.net/aoe2net/okplay.php   (PHP must be able to write here)
 *
 * The game's okplay.dll POSTs form fields and reads plain-text lines, so the
 * DLL needs no JSON parser. The same API is mirrored by rdv_standin.py for
 * local testing - keep the two in sync.
 *
 *   a=announce room peer tok cands enum name players maxp flags ver wire app
 *                (app = the game's application GUID as 32 hex digits, 0.8.1+; Connection Check's
 *                 TEST_APP marks a test game, which is never shown on the web page or in games)
 *                -> "ok <ip>\n" + "knock <peer> <cands>\n"*   (host, ~1/s)
 *   a=list       -> "ok <ip>\n" + "room <room> <peer> <age> <cands> <enum> <name> <wire>\n"*
 *                   app=<32 hex>: only that game's rooms (plus rooms of drop-ins too old to say which
 *                   game); test=1: only Connection Check's test games. Without either: every room
 *                   except test games.
 *                   (wire = the host's network protocol version; 0.1 hosts don't send it -> 1)
 *   a=knock room peer cands  -> "ok <ip>\n"                   (joiner, once)
 *   a=close room tok         -> "ok\n"
 *   a=list&fmt=json          -> rooms as JSON for the web page (index.html). Public: no key
 *                               needed, and it never includes addresses, tokens or knocks.
 *
 * Every "ok <ip>" reply also carries "latest <version> <note>" (note url-encoded, "-" if empty):
 * the newest drop-in, so older ones can offer an update. Drop-ins before 0.7.0 skip the line.
 *
 * Rooms vanish TTL seconds after their last announce, so crashed hosts never
 * leave ghosts. The host proves ownership with the random tok from its first
 * announce.
 *
 * The store (okplay_rooms.php, next to this script) holds those tokens and every host's and
 * knocker's addresses, so it must not be readable over the web: it is written as a PHP file
 * whose first line answers any direct request with 404 and exits, the JSON follows on the next
 * line (with < > & hex-escaped, so nothing in it can ever be a PHP tag). Before 0.7.1 it was
 * okplay_rooms.json - plain JSON in the web folder, readable by anyone - that file is deleted
 * when found.
 *
 * Limits: MAX_ROOMS in all and MAX_ROOMS_PER_IP per client address (one script can't fill the
 * list for everyone); MAX_KNOCKS joiners waiting per room, a repeat knock from the same joiner
 * replacing its earlier one (a flood of knocks can't rewrite the store on every request).
 *
 * Browser games (okweb.js, docs/WEB_NETPLAY.md): a host announces with wire=100 (Windows drop-ins skip
 * rooms of another wire), then joiners connect over WebRTC; this server only passes the first offer/
 * answer of each joiner through a mailbox in the room. JSON replies:
 *   a=sjoin room peer               -> {"ok":true,"tok":"<joiner token>"}
 *   a=signal room peer tok to data  -> {"ok":true}        (host <-> its joiners only, data <= 16 KB)
 *   a=poll room peer tok            -> {"ok":true,"msgs":[{"from","data"}]}
 *
 * Usage stats and reports (0.8.3; read them with okstats.php):
 *   a=report kind=crash|conn|check ver body   -> "ok <ref>\n"   (ref: a short id the player can quote)
 *                (crash: sent only after the player said yes; conn: only when the player turned
 *                 connection reports on; check: Connection Check's "Send report" button)
 * The stats (okplay_stats.php) are daily counts: games hosted, join attempts, versions, games,
 * the busiest hours, and how many different hosts / joiners / list viewers. "Different" is counted
 * with a salted hash of the address; the salt is new every day and the old one is thrown away, so
 * no address is stored and one day's hashes can't be matched with another's. Reports go to
 * okplay_reports/<date>.php with the same daily hash (to see that one PC sent several), never the
 * address, and are deleted after REPORT_DAYS. Both are PHP files that answer 404 like the room store.
 */

const APP_KEY   = 'okp_7Qm2xV9rLs4Hd8Wn';   // must match okplay.ini / the DLL default
const STORE     = __DIR__ . '/okplay_rooms.php';
const OLD_STORE = __DIR__ . '/okplay_rooms.json';   // pre-0.7.1: readable by anyone - removed
const GUARD     = "<?php http_response_code(404); exit; ?>\n";
const TTL       = 15;
const KNOCK_TTL = 20;
const MAX_ROOMS = 200;
const MAX_ROOMS_PER_IP = 4;
const MAX_KNOCKS = 16;
// browser rooms (okweb.js, docs/WEB_NETPLAY.md section 2): wire 100 - Windows drop-ins skip other wires
const WEB_WIRE       = 100;
const SIG_MAX_DATA   = 16384;   // one signaling message (SDP or ICE candidates as JSON)
const SIG_MAX_BOX    = 64;      // messages waiting per recipient
const SIG_MAX_JOIN   = 32;      // joiners being connected at once per room
const SIG_JOINER_TTL = 60;      // a joiner token dies this long after its last poll
const SIG_MSG_TTL    = 60;      // an unread message is dropped after this long
// the relay for browser players whose routers won't let them connect directly (okweb.js falls back
// to it): a mailbox per room, one file each, outside the room store so a waiting rpoll never holds
// the rooms lock
const RELAY_DIR       = __DIR__ . '/okplay_relay';
const RELAY_MAX_FRAME = 8192;     // one message, base64 (a game message of up to ~6 KB)
const RELAY_MAX_BATCH = 64;       // messages per rsend
const RELAY_MAX_QUEUE = 512;      // waiting per recipient
const RELAY_MAX_BYTES = 524288;   // waiting per recipient (base64 bytes)
const RELAY_MAX_WAIT  = 2000;     // ms an rpoll may wait for messages
const RELAY_IDLE      = 300;      // s: a room's relay file is removed this long after its last use
const STATS       = __DIR__ . '/okplay_stats.php';
const REPORTS_DIR = __DIR__ . '/okplay_reports';
const STATS_DAYS  = 400;     // daily totals kept
const REPORT_DAYS = 90;      // reports kept
const UNIQUE_CAP  = 5000;    // per day and kind; past it the count shows as "5000+"
const MAX_REPORTS_PER_DAY = 40;   // per address (by its daily hash)
const REPORT_MAX  = ['crash' => 16384, 'conn' => 8192, 'check' => 16384];
// Connection Check (netcheck.exe) test games: {0E7C4B61-93A2-4F0B-8D35-6A1F2C9B7E58}
const TEST_APP = '0e7c4b6193a24f0b8d356a1f2c9b7e58';
// game names for rooms from hosts older than 0.8.1 (they send app= but not game=)
const KNOWN_APPS = [
    '5de93f3ffc904ee1ae5a63dafa055950' => 'Age of Empires II: The Conquerors',
    'fb69a260503111d3a2d4006097ba6550' => 'Age of Empires II',
];

// The newest drop-in: older ones show "A newer version is available" (once, never blocking).
// Change these when you release. The note is shown as plain text under the version (keep it short).
const LATEST_VERSION = '0.8.2';
const LATEST_NOTE    = 'Room list: matches on slower PCs no longer show as between games; a lobby settings fix.';

function latest_line() {
    return 'latest ' . LATEST_VERSION . ' ' . (LATEST_NOTE === '' ? '-' : rawurlencode(LATEST_NOTE)) . "\n";
}

header('Content-Type: text/plain; charset=utf-8');
header('Cache-Control: no-store');
// browser games on any site use this server (okweb.js): their requests are simple form POSTs, so no
// preflight; nothing here is secret from a page (the key is in every player's okplay.ini)
header('Access-Control-Allow-Origin: *');

function reply($code, $text) {
    http_response_code($code);
    echo $text;
    exit;
}

function field($k, $max = 4096) {
    $v = isset($_POST[$k]) ? (string)$_POST[$k] : (isset($_GET[$k]) ? (string)$_GET[$k] : '');
    return substr($v, 0, $max);
}

// (\z, not $: $ also matches before a trailing newline, which then ends up in the stored value)
function clean($v, $re) { return preg_match($re, $v) ? $v : ''; }

// ---- usage stats (see the header). $fn($s, $day, $who) changes the day's counts and returns
// true if anything changed; $who is the caller's daily address hash. Never fails the request.
function guarded_read($fh) {
    $raw = stream_get_contents($fh);
    if (strncmp($raw, '<?php', 5) === 0) {
        $nl  = strpos($raw, "\n");
        $raw = $nl === false ? '' : substr($raw, $nl + 1);
    }
    $v = json_decode($raw ?: '[]', true);
    return is_array($v) ? $v : [];
}

function guarded_write($fh, $v) {
    rewind($fh);
    ftruncate($fh, 0);
    fwrite($fh, GUARD . json_encode($v, JSON_HEX_TAG | JSON_HEX_AMP));
    fflush($fh);
}

function stats_update($fn) {
    global $ip;
    $fh = @fopen(STATS, 'c+');
    if (!$fh) return;
    flock($fh, LOCK_EX);
    $s = guarded_read($fh);
    $today = gmdate('Y-m-d');
    $dirty = false;
    if (($s['day'] ?? '') !== $today) {   // a new day: new salt, yesterday's hashes are forgotten
        $s['day']  = $today;
        $s['salt'] = bin2hex(random_bytes(16));
        $s['seen'] = [];
        $s['rpt']  = [];
        $days = $s['days'] ?? [];
        ksort($days);
        while (count($days) > STATS_DAYS) array_shift($days);
        $s['days'] = $days;
        $dirty = true;
        reports_prune();
    }
    if (!isset($s['days'][$today])) {
        $s['days'][$today] = ['hosted' => 0, 'tests' => 0, 'joins' => 0, 'hosts' => 0, 'joiners' => 0,
                              'viewers' => 0, 'peak' => 0, 'ver' => [], 'game' => [], 'hour' => array_fill(0, 24, 0),
                              'reports' => [], 'conn_ok' => 0, 'conn_fail' => 0];
    }
    $who = substr(hash('sha256', $s['salt'] . '|' . $ip), 0, 12);
    if ($fn($s, $s['days'][$today], $who)) $dirty = true;
    if ($dirty) guarded_write($fh, $s);
    flock($fh, LOCK_UN);
    fclose($fh);
}

// count $who once today under $kind ('host', 'join', 'list'); true if it was new
function stats_seen(&$s, &$d, $who, $kind) {
    $cnt = ['host' => 'hosts', 'join' => 'joiners', 'list' => 'viewers'][$kind];
    if (isset($s['seen'][$kind][$who])) return false;
    if ($d[$cnt] >= UNIQUE_CAP) return false;
    $s['seen'][$kind][$who] = 1;
    $d[$cnt]++;
    return true;
}

function bump(&$arr, $key, $cap = 60) {   // a breakdown with at most $cap keys (the rest under "other")
    $key = $key === '' ? '?' : $key;
    if (!isset($arr[$key]) && count($arr) >= $cap) $key = 'other';
    $arr[$key] = ($arr[$key] ?? 0) + 1;
}

function reports_prune() {
    foreach (glob(REPORTS_DIR . '/*.php') ?: [] as $f) {
        if (preg_match('/(\d{4}-\d{2}-\d{2})\.php$/', $f, $m) && strtotime($m[1]) < time() - REPORT_DAYS * 86400)
            @unlink($f);
    }
}

// a=rsend / a=rpoll: the relay for browser players who can't connect directly (okweb.js falls back to
// it; docs/WEB_NETPLAY.md). Handled before the room store is opened, like reports. One mailbox file
// per player: okplay_relay/<room>-<peer>.php.
//   rsend: room, peer, ptok, f = "to.data*to.data..." (data: base64url, no padding)   -> {"ok":true,"dropped":n}
//          (okweb 1.1.0: frames = JSON [[to, base64], ...] - still accepted)
//   rpoll: room, peer, ptok, wait (ms), c=1 -> {"ok":true,"f":"from.data*..."}, waiting up to `wait` for
//          the first message (without c=1: "frames":[[from, base64], ...], as 1.1.0 reads it)
// ptok is the caller's own mailbox key: its first request sets it, later ones must bring it (nobody
// else can read or empty its mailbox). Anyone may drop messages into a mailbox, as on the internet.
// A waiting rpoll only looks at its own file's size every 20 ms (a full read when it changed, and
// every 200 ms in any case), so a message is on its way back within ~20 ms of arriving.
function relay_file($room, $peer) { return RELAY_DIR . '/' . $room . '-' . $peer . '.php'; }
function relay_open($room, $peer) {
    if (!is_dir(RELAY_DIR)) {
        @mkdir(RELAY_DIR, 0755);
        @file_put_contents(RELAY_DIR . '/index.html', '');   // no folder listing
    }
    $fh = @fopen(relay_file($room, $peer), 'c+');
    if (!$fh) reply(500, '{"ok":false,"error":"relay not writable"}');
    flock($fh, LOCK_EX);
    return $fh;
}
function relay_read($fh) {   // -> ['tok' => key or '', 'q' => [[from, data]], 'n' => bytes waiting]
    rewind($fh);
    $raw = stream_get_contents($fh);
    if (strncmp($raw, GUARD, strlen(GUARD)) === 0) $raw = substr($raw, strlen(GUARD));
    $d = json_decode($raw, true);
    return is_array($d) && isset($d['q']) ? $d : ['tok' => '', 'q' => [], 'n' => 0];
}
function relay_done($fh, $box = null) {   // ($box: write it back first)
    if ($box !== null) {
        rewind($fh);
        ftruncate($fh, 0);
        fwrite($fh, GUARD . json_encode($box));
        fflush($fh);
    }
    flock($fh, LOCK_UN);
    fclose($fh);
}
// the caller's own key: 0 = someone else's, 1 = ok, 2 = ok and just set (write it back)
function relay_key(&$box, $ptok) {
    if (($box['tok'] ?? '') === '') { $box['tok'] = $ptok; return 2; }
    return hash_equals($box['tok'], $ptok) ? 1 : 0;
}
function b64url_in($s) { return rtrim(strtr($s, '+/', '-_'), '='); }
function b64url_out($s) { $s = strtr($s, '-_', '+/'); return $s . str_repeat('=', (4 - strlen($s) % 4) % 4); }

function handle_relay($a) {
    $room = clean(field('room', 8), '/^[0-9a-f]{8}\z/');
    $peer = clean(field('peer', 16), '/^[0-9a-f]{16}\z/');
    $ptok = clean(field('ptok', 32), '/^[0-9a-f]{16,32}\z/');
    header('Content-Type: application/json');
    if ($room === '' || $peer === '' || $ptok === '') reply(400, '{"ok":false,"error":"missing room/peer/ptok"}');
    if (mt_rand(1, 50) === 1) {   // now and then: remove mailboxes nobody has used for a while
        foreach (glob(RELAY_DIR . '/*.php') ?: [] as $f)
            if (@filemtime($f) < time() - RELAY_IDLE) @unlink($f);
    }
    if ($a === 'rsend') {
        $frames = [];   // [to, data (base64url)]
        $f = field('f', RELAY_MAX_BATCH * (RELAY_MAX_FRAME + 20));
        if ($f !== '') {
            foreach (explode('*', $f) as $one) {
                $parts = explode('.', $one, 2);
                if (count($parts) === 2) $frames[] = $parts;
            }
        } else {
            $old = json_decode(field('frames', RELAY_MAX_BATCH * (RELAY_MAX_FRAME + 40)), true);
            if (!is_array($old)) reply(400, '{"ok":false,"error":"bad frames"}');
            foreach ($old as $one)
                if (is_array($one) && count($one) === 2 && is_string($one[0]) && is_string($one[1]))
                    $frames[] = [$one[0], b64url_in($one[1])];
        }
        if (count($frames) > RELAY_MAX_BATCH) reply(400, '{"ok":false,"error":"bad frames"}');
        $by = [];
        foreach ($frames as [$to, $data]) {
            if (!preg_match('/^[0-9a-f]{16}\z/', $to) || $to === $peer) continue;
            if (strlen($data) > RELAY_MAX_FRAME || !preg_match('/^[A-Za-z0-9_-]*\z/', $data)) continue;
            $by[$to][] = $data;
        }
        $is_new = !glob(RELAY_DIR . '/' . $room . '-*.php');
        $fh = relay_open($room, $peer);   // our own mailbox: its key is ours
        $box = relay_read($fh);
        $k = relay_key($box, $ptok);
        relay_done($fh, $k === 2 ? $box : null);
        if (!$k) reply(403, '{"ok":false,"error":"not your mailbox"}');
        $dropped = 0;
        foreach ($by as $to => $list) {
            $fh = relay_open($room, $to);
            $box = relay_read($fh);
            foreach ($list as $data) {
                if (count($box['q']) >= RELAY_MAX_QUEUE || $box['n'] + strlen($data) > RELAY_MAX_BYTES) { $dropped++; continue; }
                $box['q'][] = [$peer, $data];
                $box['n'] += strlen($data);
            }
            relay_done($fh, $box);
        }
        if ($is_new) stats_update(function (&$s, &$day, $w) { $day['relay'] = ($day['relay'] ?? 0) + 1; return true; });
        reply(200, json_encode(['ok' => true, 'dropped' => $dropped]));
    }
    // rpoll
    $compact = field('c', 2) === '1';
    $until = microtime(true) + max(0, min(RELAY_MAX_WAIT, (int)field('wait', 6))) / 1000;
    $file = relay_file($room, $peer);
    $last = -1;
    for ($i = 0; ; $i++) {
        clearstatcache(true, $file);
        $size = @filesize($file);
        $late = microtime(true) >= $until;
        if ($size !== $last || $i % 10 === 0 || $late) {
            $fh = relay_open($room, $peer);
            $box = relay_read($fh);
            $k = relay_key($box, $ptok);
            if (!$k) { relay_done($fh); reply(403, '{"ok":false,"error":"not your mailbox"}'); }
            $q = $box['q'];
            if ($q || $late) {
                $box['q'] = [];
                $box['n'] = 0;
                relay_done($fh, $q || $k === 2 ? $box : null);
                if ($compact) reply(200, json_encode(['ok' => true, 'f' => implode('*', array_map(function ($m) { return $m[0] . '.' . $m[1]; }, $q))]));
                reply(200, json_encode(['ok' => true, 'frames' => array_map(function ($m) { return [$m[0], b64url_out($m[1])]; }, $q)]));
            }
            relay_done($fh, $k === 2 ? $box : null);
            clearstatcache(true, $file);
            $last = @filesize($file);
        }
        usleep(20000);
    }
}

// a=report: handled before the room store is opened (it never touches the rooms)
function handle_report() {
    $kind = field('kind', 8);
    if (!array_key_exists($kind, REPORT_MAX)) reply(400, "error unknown report kind\n");
    $ver  = clean(field('ver', 16), '/^[0-9.]{1,16}\z/');
    $body = field('body', REPORT_MAX[$kind]);
    $body = preg_replace('/[\x00-\x08\x0b-\x1f\x7f]/', '', $body) ?? '';
    if (trim($body) === '') reply(400, "error empty report\n");
    $allowed = false;
    $who = '';
    stats_update(function (&$s, &$d, $w) use ($kind, $body, &$allowed, &$who) {
        $who = $w;
        $n = $s['rpt'][$w] ?? 0;
        if ($n >= MAX_REPORTS_PER_DAY) return false;
        $s['rpt'][$w] = $n + 1;
        bump($d['reports'], $kind, 8);
        if ($kind === 'conn') {
            foreach (explode("\n", $body) as $line) {
                if (preg_match('/(^| )result=ok( |$)/', $line)) $d['conn_ok']++;
                elseif (preg_match('/(^| )result=\w/', $line)) $d['conn_fail']++;
            }
        }
        $allowed = true;
        return true;
    });
    if (!$allowed) reply(429, "error too many reports today\n");
    if (!is_dir(REPORTS_DIR)) {
        @mkdir(REPORTS_DIR, 0755);
        @file_put_contents(REPORTS_DIR . '/index.html', '');   // no folder listing
    }
    $file = REPORTS_DIR . '/' . gmdate('Y-m-d') . '.php';
    $ref  = gmdate('md') . '-' . bin2hex(random_bytes(3));   // what a player can quote ("report 1017-a3f09c")
    $line = json_encode(['t' => time(), 'ref' => $ref, 'kind' => $kind, 'ver' => $ver, 'who' => $who, 'body' => $body],
                        JSON_HEX_TAG | JSON_HEX_AMP) . "\n";
    $fh = @fopen($file, 'c+');
    if (!$fh) reply(500, "error reports not writable\n");
    flock($fh, LOCK_EX);
    fseek($fh, 0, SEEK_END);
    if (ftell($fh) === 0) fwrite($fh, GUARD);
    fwrite($fh, $line);
    fflush($fh);
    flock($fh, LOCK_UN);
    fclose($fh);
    reply(200, "ok $ref\n");
}

$a      = field('a', 16);
$public = $a === 'list' && field('fmt', 8) === 'json';   // the web page's read-only view
if (!$public && !hash_equals(APP_KEY, field('k', 128))) reply(403, "error bad key\n");

$ip  = $_SERVER['REMOTE_ADDR'] ?? '';
$now = time();
if ($a === 'report') handle_report();   // exits
if ($a === 'rsend' || $a === 'rpoll') handle_relay($a);   // exits
$events = [];   // for the stats, recorded after the room store is written

$fh = fopen(STORE, 'c+');
if (!$fh) reply(500, "error store not writable\n");
flock($fh, LOCK_EX);
$raw = stream_get_contents($fh);
if (strncmp($raw, '<?php', 5) === 0) {   // the guard line
    $nl  = strpos($raw, "\n");
    $raw = $nl === false ? '' : substr($raw, $nl + 1);
}
$rooms = json_decode($raw ?: '[]', true);
if (!is_array($rooms)) $rooms = [];
if (is_file(OLD_STORE)) @unlink(OLD_STORE);

foreach ($rooms as $id => $r) {
    if ($now - ($r['updated'] ?? 0) > TTL) unset($rooms[$id]);
}

$dirty = false;
$out   = '';
$code  = 200;
$json  = false;   // the browser signaling actions answer JSON

$room  = clean(field('room', 8), '/^[0-9a-f]{8}\z/');
$peer  = clean(field('peer', 16), '/^[0-9a-f]{16}\z/');
$cands = clean(field('cands', 1024), '/^[0-9a-fA-F.:\[\],]*\z/');
if ($cands === '') $cands = '-';

switch ($a) {
case 'announce':
    $tok = clean(field('tok', 32), '/^[0-9a-f]{8,32}\z/');
    if ($room === '' || $peer === '' || $tok === '') { $code = 400; $out = "error missing room/peer/tok\n"; break; }
    if (isset($rooms[$room]) && !hash_equals($rooms[$room]['tok'], $tok)) {
        $code = 403; $out = "error room owned by another host\n"; break;
    }
    if (!isset($rooms[$room])) {
        if (count($rooms) >= MAX_ROOMS) { $code = 503; $out = "error full\n"; break; }
        $mine = 0;
        foreach ($rooms as $r) if (($r['ip'] ?? '') === $ip) $mine++;
        if ($mine >= MAX_ROOMS_PER_IP) { $code = 429; $out = "error too many games from this address\n"; break; }
    }
    $is_new = !isset($rooms[$room]);
    $r = $rooms[$room] ?? ['tok' => $tok, 'knocks' => [], 'created' => $now];
    $r['peer']    = $peer;
    $r['cands']   = $cands;
    $r['enum']    = clean(field('enum', 2048), '/^[A-Za-z0-9+\/=]*\z/');
    $r['name']    = function_exists('mb_substr') ? mb_substr(field('name', 256), 0, 64, 'UTF-8')
                                                 : substr(field('name', 256), 0, 64);
    $r['players'] = (int)field('players', 8);
    $r['maxp']    = (int)field('maxp', 8);
    $r['flags']   = (int)field('flags', 12);
    $r['ver']     = clean(field('ver', 16), '/^[0-9.]{1,16}\z/');
    $r['wire']    = (int)field('wire', 4);
    $r['app']     = clean(field('app', 32), '/^[0-9a-f]{32}\z/');
    // 0.8.1+: the game's own name and exe version (shown on the web page only)
    $game = preg_replace('/[\x00-\x1f\x7f]/u', '', field('game', 256)) ?? '';
    $r['game']    = function_exists('mb_substr') ? mb_substr($game, 0, 64, 'UTF-8') : substr($game, 0, 64);
    $r['gamever'] = clean(field('gamever', 24), '/^[0-9A-Za-z .,_-]{0,24}\z/');
    // 0.8.1+: what the room is doing, judged by the host (lobby / playing / between games)
    $r['state']   = clean(field('state', 8), '/^(lobby|playing|between)\z/');
    // 0.8.1+: the host's game exe - a fingerprint (joiners on another build are warned) and its file name
    $r['build']   = clean(field('build', 16), '/^[0-9a-f]{16}\z/');
    $exe = preg_replace('/[\x00-\x1f\x7f\s]/u', '', field('exe', 128)) ?? '';
    $r['exe']     = function_exists('mb_substr') ? mb_substr($exe, 0, 48, 'UTF-8') : substr($exe, 0, 48);
    if ($r['wire'] === 0 && strpos($r['ver'], '0.1') === 0) $r['wire'] = 1;   // 0.1 hosts predate the field
    $r['updated'] = $now;
    $r['ip']      = $ip;   // for MAX_ROOMS_PER_IP only; never sent to anyone
    $out = "ok $ip\n" . latest_line();
    foreach ($r['knocks'] as $k) {
        if ($now - $k['t'] <= KNOCK_TTL) $out .= "knock {$k['peer']} {$k['cands']}\n";
    }
    $r['knocks'] = [];
    $rooms[$room] = $r;
    $dirty = true;
    if ($is_new) $events[] = ['host', $r['app'] === TEST_APP, $r['ver'],
                              $r['game'] !== '' ? $r['game'] : (KNOWN_APPS[$r['app']] ?? $r['app']), count($rooms)];
    break;

case 'list':
    if (field('fmt', 8) === 'json') {
        header('Content-Type: application/json');
        $pub = [];
        $tests = 0;
        foreach ($rooms as $id => $r) {
            if (($r['app'] ?? '') === TEST_APP) { $tests++; continue; }   // test games stay off the page
            $state = ($r['state'] ?? '') !== '' ? $r['state'] : (($r['flags'] & 0x21) ? 'playing' : 'lobby');
            $pub[] = ['name' => $r['name'], 'players' => $r['players'], 'maxp' => $r['maxp'],
                      'started' => $state === 'playing', 'state' => $state, 'age' => $now - $r['created'],
                      'pw' => ($r['flags'] & 0x400) ? true : false,   // DPSESSION_PASSWORDREQUIRED
                      'ver' => $r['ver'] ?? '', 'wire' => $r['wire'] ?? 0,
                      'game' => ($r['game'] ?? '') !== '' ? $r['game'] : (KNOWN_APPS[$r['app'] ?? ''] ?? ''),
                      'gamever' => $r['gamever'] ?? '',
                      'build' => $r['build'] ?? '', 'exe' => $r['exe'] ?? ''];
        }
        header('Access-Control-Allow-Origin: *');
        $out = json_encode(['rooms' => $pub, 'tests' => $tests, 'now' => $now, 'latest' => LATEST_VERSION]);
        break;
    }
    $out = "ok $ip\n" . latest_line();
    $want = clean(field('app', 32), '/^[0-9a-f]{32}\z/');
    $only_tests = field('test', 2) === '1' || $want === TEST_APP;
    if (!$only_tests) $events[] = ['list'];
    foreach ($rooms as $id => $r) {
        $app = $r['app'] ?? '';
        if ($only_tests ? $app !== TEST_APP : $app === TEST_APP) continue;
        if ($want !== '' && !$only_tests && $app !== '' && $app !== $want) continue;   // another game's room
        $name = rawurlencode($r['name'] === '' ? '-' : $r['name']);
        $age  = $now - $r['updated'];
        $wire = (int)($r['wire'] ?? 0);
        $build = ($r['build'] ?? '') !== '' ? $r['build'] : '-';   // (clients before 0.8.1 stop reading at wire)
        $exe   = rawurlencode(($r['exe'] ?? '') !== '' ? $r['exe'] : '-');
        $out .= "room $id {$r['peer']} $age {$r['cands']} {$r['enum']} $name $wire $build $exe\n";
    }
    break;

case 'knock':
    if ($room === '' || $peer === '' || !isset($rooms[$room])) { $code = 404; $out = "error no such room\n"; break; }
    $pending = [];
    foreach ($rooms[$room]['knocks'] as $k) {
        // expired ones go; a repeat from the same joiner (it knocks again every 3 s) replaces its earlier one
        if ($now - $k['t'] <= KNOCK_TTL && $k['peer'] !== $peer) $pending[] = $k;
    }
    if (count($pending) >= MAX_KNOCKS) { $code = 429; $out = "error too many joiners waiting, knock again\n"; break; }
    $pending[] = ['peer' => $peer, 'cands' => $cands, 't' => $now];
    $rooms[$room]['knocks'] = $pending;
    $out = "ok $ip\n" . latest_line();
    $dirty = true;
    // stats: one join attempt per joiner and room (it knocks every 3 s until it gets through)
    $seen = $rooms[$room]['joiners'] ?? [];
    if (!in_array($peer, $seen, true)) {
        if (count($seen) < 64) $seen[] = $peer;
        $rooms[$room]['joiners'] = $seen;
        if (($rooms[$room]['app'] ?? '') !== TEST_APP) $events[] = ['join'];
    }
    break;

// ---- browser signaling (okweb.js; docs/WEB_NETPLAY.md section 2): a mailbox per web room, polled by
// both sides only while a joiner's first connection (to the host) is set up
case 'sjoin':
case 'signal':
case 'poll':
    $json = true;
    $r = ($room !== '' && isset($rooms[$room])) ? $rooms[$room] : null;
    if ($peer === '' || !$r || (int)($r['wire'] ?? 0) !== WEB_WIRE) {
        $code = 404; $out = json_encode(['ok' => false, 'error' => 'no such room']); break;
    }
    $sig = $r['sig'] ?? ['j' => [], 'box' => []];
    foreach ($sig['j'] as $jp => $j) {   // joiners that stopped polling, and their mail
        if ($now - $j['t'] > SIG_JOINER_TTL) { unset($sig['j'][$jp]); unset($sig['box'][$jp]); }
    }
    foreach ($sig['box'] as $to => $msgs) {
        $sig['box'][$to] = array_values(array_filter($msgs, function ($m) use ($now) { return $now - $m['t'] <= SIG_MSG_TTL; }));
    }
    if ($a === 'sjoin') {
        if ($peer === $r['peer']) { $code = 400; $out = json_encode(['ok' => false, 'error' => 'the host cannot join']); break; }
        if (!isset($sig['j'][$peer]) && count($sig['j']) >= SIG_MAX_JOIN) {
            $code = 429; $out = json_encode(['ok' => false, 'error' => 'too many joiners connecting, try again']); break;
        }
        $jt = isset($sig['j'][$peer]) ? $sig['j'][$peer]['tok'] : bin2hex(random_bytes(8));
        $sig['j'][$peer] = ['tok' => $jt, 't' => $now];
        $rooms[$room]['sig'] = $sig;
        $dirty = true;
        $out = json_encode(['ok' => true, 'tok' => $jt]);
        if (!isset($r['joiners']) || !in_array($peer, $r['joiners'], true)) {   // stats: a join attempt
            $rooms[$room]['joiners'] = array_slice(array_merge($r['joiners'] ?? [], [$peer]), 0, 64);
            $events[] = ['join'];
        }
        break;
    }
    // signal / poll: the caller must prove who it is - the host with the room token, a joiner with its own
    $tok = field('tok', 32);
    $is_host = $peer === $r['peer'];
    $known = $is_host ? hash_equals($r['tok'], $tok) : (isset($sig['j'][$peer]) && hash_equals($sig['j'][$peer]['tok'], $tok));
    if (!$known) { $code = 403; $out = json_encode(['ok' => false, 'error' => 'not a member of this room']); break; }
    if ($a === 'signal') {
        $to = clean(field('to', 16), '/^[0-9a-f]{16}\z/');
        $raw_len = strlen(isset($_POST['data']) ? (string)$_POST['data'] : (string)($_GET['data'] ?? ''));
        $data = field('data', SIG_MAX_DATA);
        // a joiner writes only to the host, the host only to its joiners
        $to_ok = $is_host ? isset($sig['j'][$to]) : $to === $r['peer'];
        if (!$to_ok) { $code = 403; $out = json_encode(['ok' => false, 'error' => 'no such recipient']); break; }
        if ($raw_len === 0 || $raw_len > SIG_MAX_DATA) { $code = 413; $out = json_encode(['ok' => false, 'error' => 'empty or too big']); break; }
        if (count($sig['box'][$to] ?? []) >= SIG_MAX_BOX) { $code = 429; $out = json_encode(['ok' => false, 'error' => 'recipient mailbox full']); break; }
        $sig['box'][$to][] = ['from' => $peer, 'data' => $data, 't' => $now];
        $rooms[$room]['sig'] = $sig;
        $dirty = true;
        $out = json_encode(['ok' => true]);
        break;
    }
    // poll: hand over (and forget) what waits for the caller
    $msgs = [];
    foreach ($sig['box'][$peer] ?? [] as $m) $msgs[] = ['from' => $m['from'], 'data' => $m['data']];
    if ($msgs) { unset($sig['box'][$peer]); $dirty = true; }
    if (!$is_host && $now - $sig['j'][$peer]['t'] >= 10) { $sig['j'][$peer]['t'] = $now; $dirty = true; }   // still connecting
    if ($dirty) $rooms[$room]['sig'] = $sig;
    $out = json_encode(['ok' => true, 'msgs' => $msgs]);
    break;

case 'close':
    $tok = field('tok', 32);
    if ($room !== '' && isset($rooms[$room]) && hash_equals($rooms[$room]['tok'], $tok)) {
        unset($rooms[$room]);
        $dirty = true;
    }
    $out = "ok\n";
    break;

default:
    $code = 400;
    $out = "error unknown action\n";
}

if ($dirty) {
    rewind($fh);
    ftruncate($fh, 0);
    fwrite($fh, GUARD . json_encode($rooms, JSON_HEX_TAG | JSON_HEX_AMP));
    fflush($fh);
}
flock($fh, LOCK_UN);
fclose($fh);
if ($json) header('Content-Type: application/json');
if ($events) stats_update(function (&$s, &$d, $who) use ($events, $now) {
    $changed = false;
    foreach ($events as $e) {
        if ($e[0] === 'host') {
            if ($e[1]) { $d['tests']++; $changed = true; continue; }   // Connection Check's test games
            $d['hosted']++;
            bump($d['ver'], $e[2]);
            bump($d['game'], $e[3]);
            $d['hour'][(int)gmdate('G', $now)]++;
            if ($e[4] > $d['peak']) $d['peak'] = $e[4];
            stats_seen($s, $d, $who, 'host');
            $changed = true;
        } elseif ($e[0] === 'join') {
            $d['joins']++;
            stats_seen($s, $d, $who, 'join');
            $changed = true;
        } elseif ($e[0] === 'list') {
            if (stats_seen($s, $d, $who, 'list')) $changed = true;   // most lists change nothing
        }
    }
    return $changed;
});
reply($code, $out);
