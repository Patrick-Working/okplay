<?php
/*
 * Oldkid.net Online Play - private usage stats and reports (what okplay.php collects).
 * ----------------------------------------------------------------------------------
 * Deploy next to okplay.php. It needs a password, kept in okplay_admin.php in the same folder:
 *
 *     <?php define('OKSTATS_PASSWORD', 'pick-a-long-password');
 *
 * (a PHP file, so the web server never shows its contents). Without that file the page only says
 * how to set it up. Sign-in lasts for the browser session. Everything shown here comes from players'
 * PCs, so every value is escaped before it reaches the page.
 */

const STATS       = __DIR__ . '/okplay_stats.php';
const REPORTS_DIR = __DIR__ . '/okplay_reports';
const ADMIN_FILE  = __DIR__ . '/okplay_admin.php';

header('Content-Type: text/html; charset=utf-8');
header('Cache-Control: no-store');
header('X-Frame-Options: DENY');
header("Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; form-action 'self'");
header('Referrer-Policy: no-referrer');

function cut($s, $n) { return function_exists('mb_substr') ? mb_substr($s, 0, $n, 'UTF-8') : substr($s, 0, $n); }

function h($s) { return htmlspecialchars((string)$s, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8'); }

function page($title, $body) {
    echo '<!doctype html><html lang="en"><head><meta charset="utf-8">'
       . '<meta name="viewport" content="width=device-width,initial-scale=1">'
       . '<meta name="robots" content="noindex,nofollow"><title>' . h($title) . '</title><style>'
       . ':root{--bg:#f6f7f9;--card:#fff;--fg:#1c2330;--mut:#667085;--line:#e3e6eb;--acc:#2f6fdf;--bad:#c0392b;--ok:#1e8e4e}'
       . '@media (prefers-color-scheme:dark){:root{--bg:#12151b;--card:#1b2029;--fg:#e6e9ef;--mut:#98a2b3;--line:#2b3240;--acc:#6b9cff;--bad:#ff7a6b;--ok:#4cc38a}}'
       . 'body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,Segoe UI,sans-serif}'
       . 'main{max-width:1100px;margin:0 auto;padding:16px}h1{font-size:20px;margin:4px 0 12px}h2{font-size:16px;margin:22px 0 8px}'
       . 'nav a{margin-right:14px}a{color:var(--acc)}.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px;margin:10px 0;overflow-x:auto}'
       . 'table{border-collapse:collapse;width:100%}td,th{padding:4px 8px;border-bottom:1px solid var(--line);text-align:right;white-space:nowrap}'
       . 'td:first-child,th:first-child,.l{text-align:left}th{color:var(--mut);font-weight:600}'
       . '.tiles{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}.tile b{display:block;font-size:22px}.tile span{color:var(--mut)}'
       . '.bar{display:inline-block;height:10px;background:var(--acc);border-radius:2px;vertical-align:middle}'
       . '.mut{color:var(--mut)}.bad{color:var(--bad)}.ok{color:var(--ok)}pre{white-space:pre-wrap;word-break:break-all;font:12px/1.4 Consolas,monospace;margin:0}'
       . 'input,button{font:inherit;padding:6px 10px}</style></head><body><main>' . $body . '</main></body></html>';
    exit;
}

function guarded_json($file) {
    $raw = @file_get_contents($file);
    if ($raw === false) return [];
    if (strncmp($raw, '<?php', 5) === 0) {
        $nl  = strpos($raw, "\n");
        $raw = $nl === false ? '' : substr($raw, $nl + 1);
    }
    $v = json_decode($raw ?: '[]', true);
    return is_array($v) ? $v : [];
}

// ---------------------------------------------------------------- sign-in
if (!is_file(ADMIN_FILE)) {
    page('Stats - setup', '<h1>Online Play stats</h1><div class="card">Not set up yet. Create <code>okplay_admin.php</code> '
       . 'next to this page containing:<pre>&lt;?php define(\'OKSTATS_PASSWORD\', \'pick-a-long-password\');</pre></div>');
}
require ADMIN_FILE;
if (!defined('OKSTATS_PASSWORD') || strlen(OKSTATS_PASSWORD) < 8) {
    page('Stats - setup', '<h1>Online Play stats</h1><div class="card">okplay_admin.php must define OKSTATS_PASSWORD '
       . '(at least 8 characters).</div>');
}
$https = !empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off';
session_name('okstats');
// the cookie only for this folder (dirname gives "\" for the root on a Windows server)
session_set_cookie_params(0, rtrim(strtr(dirname($_SERVER['SCRIPT_NAME']), '\\', '/'), '/') . '/', '', $https, true);
session_start();
$want = hash('sha256', 'okstats|' . OKSTATS_PASSWORD);
if (isset($_POST['logout'])) { $_SESSION = []; session_destroy(); header('Location: ' . basename(__FILE__)); exit; }
if (isset($_POST['password'])) {
    if (hash_equals(OKSTATS_PASSWORD, (string)$_POST['password'])) {
        session_regenerate_id(true);
        $_SESSION['ok'] = $want;
        header('Location: ' . basename(__FILE__));
        exit;
    }
    sleep(2);   // slows down guessing
}
if (!hash_equals($want, (string)($_SESSION['ok'] ?? ''))) {
    page('Stats - sign in', '<h1>Online Play stats</h1><div class="card"><form method="post">'
       . '<input type="password" name="password" placeholder="Password" autofocus required> <button>Sign in</button>'
       . (isset($_POST['password']) ? ' <span class="bad">Wrong password</span>' : '') . '</form></div>');
}

// ---------------------------------------------------------------- data
$stats = guarded_json(STATS);
$days  = $stats['days'] ?? [];
krsort($days);

function reports_for($daysBack, $kind = '') {
    $out = [];
    $files = glob(REPORTS_DIR . '/*.php') ?: [];
    rsort($files);
    foreach (array_slice($files, 0, $daysBack) as $f) {
        $lines = @file($f, FILE_IGNORE_NEW_LINES) ?: [];
        $day = basename($f, '.php');
        foreach ($lines as $i => $line) {
            if ($i === 0 && strncmp($line, '<?php', 5) === 0) continue;
            $r = json_decode($line, true);
            if (!is_array($r) || ($kind !== '' && ($r['kind'] ?? '') !== $kind)) continue;
            $r['id'] = $day . ':' . $i;
            $out[] = $r;
        }
    }
    usort($out, function ($a, $b) { return ($b['t'] ?? 0) <=> ($a['t'] ?? 0); });
    return $out;
}

// "key=value" fields of a report body (first occurrence wins)
function fields($body) {
    $f = [];
    foreach (preg_split('/\r?\n/', $body) as $line) {
        if (preg_match('/^([a-z_]+)=(.*)$/', $line, $m) && !isset($f[$m[1]])) $f[$m[1]] = $m[2];
    }
    return $f;
}

function sum_days($days, $n, $key) {
    $t = 0;
    foreach (array_slice($days, 0, $n, true) as $d) $t += is_array($d[$key] ?? 0) ? array_sum($d[$key]) : ($d[$key] ?? 0);
    return $t;
}

function merge_days($days, $n, $key) {
    $t = [];
    foreach (array_slice($days, 0, $n, true) as $d)
        foreach (($d[$key] ?? []) as $k => $v) $t[$k] = ($t[$k] ?? 0) + $v;
    arsort($t);
    return $t;
}

function bars($map, $limit = 12) {
    if (!$map) return '<span class="mut">none yet</span>';
    $max = max($map) ?: 1;
    $o = '<table>';
    foreach (array_slice($map, 0, $limit, true) as $k => $v)
        $o .= '<tr><td class="l">' . h($k) . '</td><td style="width:60%" class="l"><span class="bar" style="width:'
            . round(100 * $v / $max) . '%"></span></td><td>' . (int)$v . '</td></tr>';
    return $o . '</table>';
}

function when($t) { return $t ? gmdate('Y-m-d H:i', (int)$t) . ' UTC' : ''; }

$v = $_GET['v'] ?? 'overview';
$nav = '<nav><a href="?v=overview">Overview</a><a href="?v=crash">Crash reports</a><a href="?v=conn">Connection reports</a>'
     . '<a href="?v=check">Connection Check reports</a>'
     . '<form method="get" style="display:inline"><input type="hidden" name="v" value="find">'
     . '<input name="ref" placeholder="report id" size="12"> <button>Find</button></form></nav>'
     . '<form method="post" style="float:right;margin-top:-26px"><button name="logout" value="1">Sign out</button></form>';

// ---------------------------------------------------------------- a report by the id players quote
if ($v === 'find') {
    $ref = trim((string)($_GET['ref'] ?? ''));
    foreach (reports_for(90) as $r) {
        if ($ref !== '' && hash_equals((string)($r['ref'] ?? ''), $ref)) {
            header('Location: ?v=report&id=' . rawurlencode($r['id']));
            exit;
        }
    }
    page('Report', $nav . '<p>No report with the id ' . h($ref) . ' (reports are kept 90 days).</p>');
}

// ---------------------------------------------------------------- one report
if ($v === 'report') {
    $id = (string)($_GET['id'] ?? '');
    if (!preg_match('/^(\d{4}-\d{2}-\d{2}):(\d+)$/', $id, $m)) page('Report', $nav . '<p>No such report.</p>');
    $lines = @file(REPORTS_DIR . '/' . $m[1] . '.php', FILE_IGNORE_NEW_LINES) ?: [];
    $r = isset($lines[(int)$m[2]]) && (int)$m[2] > 0 ? json_decode($lines[(int)$m[2]], true) : null;
    if (!is_array($r)) page('Report', $nav . '<p>No such report.</p>');
    page('Report', $nav . '<h1>' . h(ucfirst($r['kind'] ?? '')) . ' report</h1><div class="card"><p class="mut">'
       . h(when($r['t'] ?? 0)) . ' &middot; id ' . h($r['ref'] ?? '-') . ' &middot; v' . h($r['ver'] ?? '?') . ' &middot; sender ' . h($r['who'] ?? '')
       . ' (changes daily)</p><pre>' . h($r['body'] ?? '') . '</pre></div>');
}

// ---------------------------------------------------------------- crash reports, grouped
if ($v === 'crash') {
    $groups = [];
    foreach (reports_for(90, 'crash') as $r) {
        $f = fields($r['body'] ?? '');
        $sig = $f['sig'] ?? 'unknown';
        $g = &$groups[$sig];
        if (!$g) $g = ['n' => 0, 'first' => PHP_INT_MAX, 'last' => 0, 'ver' => [], 'exe' => [], 'win' => [], 'latest' => $r['id'], 'who' => []];
        $g['n']++;
        $g['who'][$r['who'] ?? ''] = 1;
        $g['first'] = min($g['first'], $r['t']);
        if ($r['t'] >= $g['last']) { $g['last'] = $r['t']; $g['latest'] = $r['id']; }
        $g['ver'][$r['ver'] ?? '?'] = 1;
        $g['exe'][$f['exe'] ?? '?'] = 1;
        $g['win'][$f['win'] ?? '?'] = 1;
        unset($g);
    }
    uasort($groups, function ($a, $b) { return $b['n'] <=> $a['n']; });
    $o = '<table><tr><th>Where it crashed</th><th>Reports</th><th>PCs*</th><th class="l">Versions</th><th class="l">Game</th>'
       . '<th class="l">Windows</th><th>Last</th><th></th></tr>';
    foreach ($groups as $sig => $g)
        $o .= '<tr><td class="l">' . h($sig) . '</td><td>' . $g['n'] . '</td><td>' . count($g['who']) . '</td><td class="l">'
            . h(implode(', ', array_keys($g['ver']))) . '</td><td class="l">' . h(implode(', ', array_keys($g['exe'])))
            . '</td><td class="l">' . h(implode(', ', array_keys($g['win']))) . '</td><td>' . h(when($g['last']))
            . '</td><td><a href="?v=report&amp;id=' . h($g['latest']) . '">latest</a></td></tr>';
    page('Crash reports', $nav . '<h1>Crash reports (last 90 days)</h1><div class="card">'
       . ($groups ? $o . '</table>' : '<span class="mut">No crash reports.</span>')
       . '</div><p class="mut">* different senders per day: the same PC on two days counts twice.</p>');
}

// ---------------------------------------------------------------- connection / check reports
if ($v === 'conn' || $v === 'check') {
    $rows = array_slice(reports_for(30, $v), 0, 300);
    $o = '';
    if ($v === 'conn') {
        $why = [];
        foreach ($rows as $r) foreach (preg_split('/\r?\n/', $r['body'] ?? '') as $line) {
            if (preg_match('/(?:^| )result=(\S+)/', $line, $m)) $why[$m[1]] = ($why[$m[1]] ?? 0) + 1;
        }
        arsort($why);
        $o .= '<h2>Results</h2><div class="card">' . bars($why) . '</div>';
    }
    $o .= '<h2>Latest</h2><div class="card"><table><tr><th>Time</th><th class="l">Id</th><th class="l">Version</th><th class="l">Sender*</th><th class="l">Report</th></tr>';
    foreach ($rows as $r) {
        $body = $r['body'] ?? '';
        $short = cut($body, 400);
        $cls = $v === 'conn' ? (preg_match('/result=ok/', $body) && !preg_match('/result=(?!ok)/', $body) ? 'ok' : 'bad') : '';
        $o .= '<tr><td>' . h(when($r['t'] ?? 0)) . '</td><td class="l">' . h($r['ref'] ?? '') . '</td><td class="l">' . h($r['ver'] ?? '') . '</td><td class="l">'
            . h($r['who'] ?? '') . '</td><td class="l ' . $cls . '"><pre>' . h($short) . (strlen($body) > strlen($short) ? ' ...' : '')
            . '</pre><a href="?v=report&amp;id=' . h($r['id']) . '">open</a></td></tr>';
    }
    $title = $v === 'conn' ? 'Connection reports' : 'Connection Check reports';
    page($title, $nav . '<h1>' . $title . ' (last 30 days)</h1>' . ($rows ? $o . '</table></div>' : '<div class="card mut">None yet.</div>')
       . '<p class="mut">* changes daily.</p>');
}

// ---------------------------------------------------------------- overview
$tile = function ($label, $n) { return '<div class="card tile"><b>' . h($n) . '</b><span>' . h($label) . '</span></div>'; };
$cap = function ($n) { return $n >= 5000 ? '5000+' : $n; };
$today = reset($days) ?: [];
$o = '<h1>Online Play stats</h1><p class="mut">Counts from okplay.php. Days and hours are UTC. No addresses are stored: '
   . '"different" hosts/players are counted per day.</p>'
   . '<h2>Today</h2><div class="tiles">'
   . $tile('games hosted', $today['hosted'] ?? 0) . $tile('join attempts', $today['joins'] ?? 0)
   . $tile('different hosts', $cap($today['hosts'] ?? 0)) . $tile('different joiners', $cap($today['joiners'] ?? 0))
   . $tile('opened the game list', $cap($today['viewers'] ?? 0)) . $tile('most games at once', $today['peak'] ?? 0) . '</div>'
   . '<h2>Last 7 / 30 days</h2><div class="tiles">'
   . $tile('games hosted (7 d)', sum_days($days, 7, 'hosted')) . $tile('games hosted (30 d)', sum_days($days, 30, 'hosted'))
   . $tile('join attempts (30 d)', sum_days($days, 30, 'joins'))
   . $tile('connection reports ok / failed (30 d)', sum_days($days, 30, 'conn_ok') . ' / ' . sum_days($days, 30, 'conn_fail'))
   . $tile('crash reports (30 d)', array_sum(array_map(function ($d) { return $d['reports']['crash'] ?? 0; }, array_slice($days, 0, 30))))
   . $tile('Connection Check tests (30 d)', sum_days($days, 30, 'tests')) . '</div>';

$o .= '<h2>Daily</h2><div class="card"><table><tr><th>Day</th><th>Hosted</th><th>Joins</th><th>Hosts</th><th>Joiners</th>'
    . '<th>List viewers</th><th>Peak</th><th>Tests</th><th>Conn ok/fail</th><th>Crashes</th><th class="l"></th></tr>';
$maxh = 1;
foreach (array_slice($days, 0, 30, true) as $d) $maxh = max($maxh, $d['hosted'] ?? 0);
foreach (array_slice($days, 0, 30, true) as $day => $d)
    $o .= '<tr><td>' . h($day) . '</td><td>' . (int)($d['hosted'] ?? 0) . '</td><td>' . (int)($d['joins'] ?? 0) . '</td><td>'
        . h($cap($d['hosts'] ?? 0)) . '</td><td>' . h($cap($d['joiners'] ?? 0)) . '</td><td>' . h($cap($d['viewers'] ?? 0))
        . '</td><td>' . (int)($d['peak'] ?? 0) . '</td><td>' . (int)($d['tests'] ?? 0) . '</td><td>' . (int)($d['conn_ok'] ?? 0)
        . ' / ' . (int)($d['conn_fail'] ?? 0) . '</td><td>' . (int)($d['reports']['crash'] ?? 0) . '</td><td class="l" style="width:25%">'
        . '<span class="bar" style="width:' . round(100 * ($d['hosted'] ?? 0) / $maxh) . '%"></span></td></tr>';
$o .= '</table>' . ($days ? '' : '<span class="mut">No data yet - it starts with the first hosted game.</span>') . '</div>';

$hours = array_fill(0, 24, 0);
foreach (array_slice($days, 0, 30) as $d) foreach (($d['hour'] ?? []) as $i => $n) $hours[$i] += $n;
$hmap = [];
foreach ($hours as $i => $n) $hmap[sprintf('%02d:00', $i)] = $n;
$o .= '<h2>Versions in use (games hosted, 30 days)</h2><div class="card">' . bars(merge_days($days, 30, 'ver')) . '</div>'
    . '<h2>Games (30 days)</h2><div class="card">' . bars(merge_days($days, 30, 'game')) . '</div>'
    . '<h2>Games hosted by hour (UTC, 30 days)</h2><div class="card">' . bars(array_sum($hours) ? $hmap : [], 24) . '</div>';
page('Online Play stats', $nav . $o);
