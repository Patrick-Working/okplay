<?php
/*
 * Oldkid.net Online Play - public usage statistics (read-only, no sign-in).
 * ------------------------------------------------------------------------
 * Script version: 2026-11-25b (change this line with every upload).
 * Deploy next to okplay.php. Reads what okplay.php keeps, forever:
 *   okplay_stats.php            daily totals (games hosted, join attempts, how many different hosts / joiners /
 *                               list viewers that day, games, okplay versions, hours, connection reports' results)
 *   okplay_sessions/<YYYY-MM>.php  one line per game that ended: day, start hour (UTC), game, okplay version,
 *                               minutes open, minutes racing, most players, players who asked to join
 * and publishes totals and charts. Never shown: the day's salt and address hashes, reports and what is in them,
 * room or player names (not stored), addresses (not stored). Game and version names come from players' PCs:
 * only names that look like a game or a version number are shown as they are, the rest count as "other".
 * Every value is escaped. The page is built at most every CACHE_S seconds (okplay_stats_page.php, a PHP file that
 * answers 404 when opened directly).
 */

const STATS        = __DIR__ . '/okplay_stats.php';
const SESSIONS_DIR = __DIR__ . '/okplay_sessions';
const CACHE        = __DIR__ . '/okplay_stats_page.php';
const CACHE_S      = 300;
const GUARD        = "<?php http_response_code(404); exit; ?>\n";
const UNIQUE_CAP   = 5000;   // okplay.php's cap per day and kind
const RECENT       = 100;    // games in the table

header('Content-Type: text/html; charset=utf-8');
header('Cache-Control: public, max-age=' . CACHE_S);
header('X-Frame-Options: DENY');
header("Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; img-src data:");
header('Referrer-Policy: no-referrer');

// ---- the cached page
$c = @file_get_contents(CACHE);
if ($c !== false && strncmp($c, GUARD, strlen(GUARD)) === 0 && time() - (int)@filemtime(CACHE) < CACHE_S) {
    echo substr($c, strlen(GUARD));
    exit;
}

function h($s) { return htmlspecialchars((string)$s, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8'); }
function n($v) { return is_numeric($v) ? (int)$v : 0; }
function num($v) { return number_format((float)$v); }

function strip_guard($raw) {
    if (strncmp($raw, '<?php', 5) === 0) { $nl = strpos($raw, "\n"); return $nl === false ? '' : substr($raw, $nl + 1); }
    return $raw;
}
function read_stats() {
    $fh = @fopen(STATS, 'r');
    if (!$fh) return [];
    flock($fh, LOCK_SH); $raw = stream_get_contents($fh); flock($fh, LOCK_UN); fclose($fh);
    $v = json_decode(strip_guard($raw) ?: '[]', true);
    return is_array($v) ? $v : [];
}
function read_sessions() {
    $out = [];
    foreach (glob(SESSIONS_DIR . '/*.php') ?: [] as $f) {
        if (!preg_match('/\/\d{4}-\d{2}\.php$/', str_replace('\\', '/', $f))) continue;
        foreach (explode("\n", strip_guard((string)@file_get_contents($f))) as $line) {
            $r = json_decode($line, true);
            if (is_array($r) && isset($r['d']) && preg_match('/^\d{4}-\d{2}-\d{2}$/', (string)$r['d'])) $out[] = $r;
        }
    }
    usort($out, function ($a, $b) { return strcmp($a['d'], $b['d']) ?: (n($a['h'] ?? 0) <=> n($b['h'] ?? 0)); });
    return $out;
}
// names from players' PCs: shown only when they look like a game name / an okplay version
function safe_game($k) {
    if (!is_string($k) || preg_match('/^[0-9a-f]{32}$/', $k)) return 'other';
    return preg_match("/^[A-Za-z0-9][A-Za-z0-9 .,:'()&+\\/-]{0,47}$/", $k) ? $k : 'other';
}
function safe_ver($k) { return is_string($k) && preg_match('/^\d{1,3}(\.\d{1,4}){0,3}$/', $k) ? $k : 'other'; }

// ---- charts: inline SVG, colours from the page's own tokens
function chart_bars($labels, $values, $title, $unit = '', $label_every = 1) {
    $n = count($values);
    if (!$n || !array_sum($values)) return '<p class="mut">Nothing yet.</p>';
    $W = 900; $H = 220; $L = 44; $B = 28; $T = 8;
    $max = max($values); $max = $max > 0 ? $max : 1;
    $step = ($W - $L - 4) / $n; $bw = max(1, $step * 0.78);
    $o = '<svg viewBox="0 0 ' . $W . ' ' . $H . '" role="img" aria-label="' . h($title) . '" class="chart">';
    foreach ([0, 0.5, 1] as $f) {
        $y = $T + ($H - $T - $B) * (1 - $f);
        $o .= '<line x1="' . $L . '" x2="' . $W . '" y1="' . round($y, 1) . '" y2="' . round($y, 1) . '" class="grid"/>'
            . '<text x="' . ($L - 6) . '" y="' . round($y + 4, 1) . '" class="ax" text-anchor="end">' . h(num(round($max * $f))) . '</text>';
    }
    foreach ($values as $i => $v) {
        $bh = ($H - $T - $B) * $v / $max; $x = $L + 2 + $i * $step; $y = $H - $B - $bh;
        $o .= '<rect x="' . round($x, 1) . '" y="' . round($y, 1) . '" width="' . round($bw, 1) . '" height="' . round($bh, 1) . '" class="b">'
            . '<title>' . h($labels[$i] . ': ' . num($v) . $unit) . '</title></rect>';
        if ($i % $label_every === 0)
            $o .= '<text x="' . round($x + $bw / 2, 1) . '" y="' . ($H - 10) . '" class="ax" text-anchor="middle">' . h($labels[$i]) . '</text>';
    }
    return $o . '</svg>';
}
function chart_hbars($rows, $unit = '') {
    if (!$rows || !array_sum($rows)) return '<p class="mut">Nothing yet.</p>';
    $max = max($rows) ?: 1;
    $o = '<table class="hb">';
    foreach ($rows as $k => $v)
        $o .= '<tr><td class="l">' . h($k) . '</td><td class="v">' . h(num($v) . $unit) . '</td><td class="l w"><span class="bar" style="width:'
            . round(100 * $v / $max, 1) . '%"></span></td></tr>';
    return $o . '</table>';
}

// ---- the numbers
$s    = read_stats();
$days = isset($s['days']) && is_array($s['days']) ? $s['days'] : [];
$days = array_filter($days, function ($d, $k) { return is_array($d) && preg_match('/^\d{4}-\d{2}-\d{2}$/', $k); }, ARRAY_FILTER_USE_BOTH);
ksort($days);
$sess = read_sessions();
$today = gmdate('Y-m-d');
$first = $days ? (string)key($days) : $today;   // (ksort: the first day)

$all = ['hosted' => 0, 'joins' => 0, 'ok' => 0, 'fail' => 0, 'peak' => 0];
$games = []; $vers = []; $hours = array_fill(0, 24, 0); $months = [];
$cut90 = gmdate('Y-m-d', time() - 89 * 86400);
foreach ($days as $k => $d) {
    $all['hosted'] += n($d['hosted'] ?? 0); $all['joins'] += n($d['joins'] ?? 0);
    $all['ok'] += n($d['conn_ok'] ?? 0); $all['fail'] += n($d['conn_fail'] ?? 0);
    $all['peak'] = max($all['peak'], n($d['peak'] ?? 0));
    foreach ((array)($d['game'] ?? []) as $g => $c) { $g = safe_game($g); $games[$g] = ($games[$g] ?? 0) + n($c); }
    if ($k >= $cut90) foreach ((array)($d['ver'] ?? []) as $v => $c) { $v = safe_ver($v); $vers[$v] = ($vers[$v] ?? 0) + n($c); }
    foreach ((array)($d['hour'] ?? []) as $i => $c) if (is_int($i) && $i >= 0 && $i < 24) $hours[$i] += n($c);
    $m = substr($k, 0, 7); $months[$m] = ($months[$m] ?? 0) + n($d['hosted'] ?? 0);
}
arsort($games); arsort($vers);

// daily series, last 90 days (days with nothing count as 0)
$dl = []; $dv = []; $dh = []; $dj = [];
for ($i = 89; $i >= 0; $i--) {
    $k = gmdate('Y-m-d', time() - $i * 86400); $d = $days[$k] ?? [];
    $dl[] = substr($k, 5); $dv[] = n($d['hosted'] ?? 0); $dh[] = min(n($d['hosts'] ?? 0), UNIQUE_CAP); $dj[] = min(n($d['joiners'] ?? 0), UNIQUE_CAP);
}
$dp = array_map(function ($a, $b) { return $a + $b; }, $dh, $dj);

// games that ended (sessions)
$play_min = 0; $race_min = 0; $pl = 0; $plc = 0; $by_players = array_fill(1, 8, 0);
$len_b = ['under 5 min' => 0, '5-15 min' => 0, '15-30 min' => 0, '30-60 min' => 0, '1-2 hours' => 0, 'over 2 hours' => 0];
$per_game = [];
foreach ($sess as $r) {
    $m = max(0, n($r['m'] ?? 0)); $p = max(0, n($r['p'] ?? 0)); $g = safe_game($r['g'] ?? '');
    $play_min += $m; $race_min += max(0, n($r['r'] ?? 0));
    if ($p > 0) { $pl += $p; $plc++; $by_players[min(8, $p)]++; }
    $len_b[$m < 5 ? 'under 5 min' : ($m < 15 ? '5-15 min' : ($m < 30 ? '15-30 min' : ($m < 60 ? '30-60 min' : ($m < 120 ? '1-2 hours' : 'over 2 hours'))))]++;
    $pg = &$per_game[$g]; $pg['n'] = ($pg['n'] ?? 0) + 1; $pg['min'] = ($pg['min'] ?? 0) + $m; $pg['race'] = ($pg['race'] ?? 0) + n($r['r'] ?? 0);
    $pg['p'] = ($pg['p'] ?? 0) + $p; unset($pg);
}
uasort($per_game, function ($a, $b) { return $b['n'] <=> $a['n']; });
$rate = $all['ok'] + $all['fail'] ? round(100 * $all['ok'] / ($all['ok'] + $all['fail'])) . '%' : '-';
$td = $days[$today] ?? [];

function hm($min) { $min = (int)$min; return $min >= 60 ? sprintf('%d h %02d min', intdiv($min, 60), $min % 60) : $min . ' min'; }

ob_start();
?>
<h1>Oldkid.net Online Play - usage</h1>
<p class="mut">Everything the room server counts, since <?= h($first) ?>. No names, addresses or room names are kept or shown;
"different players" are counted per day only and can't be followed from one day to the next. Times are UTC. Updated every few minutes.</p>

<div class="tiles">
  <div class="card tile"><b><?= num($all['hosted']) ?></b><span>games hosted</span></div>
  <div class="card tile"><b><?= num(count($sess)) ?></b><span>games played to the end<?= $sess ? '' : ' (counted since ' . h(gmdate('Y-m-d')) . ')' ?></span></div>
  <div class="card tile"><b><?= h(hm($play_min)) ?></b><span>in games (<?= h(hm($race_min)) ?> racing)</span></div>
  <div class="card tile"><b><?= $plc ? h(number_format($pl / $plc, 1)) : '-' ?></b><span>players per game, on average</span></div>
  <div class="card tile"><b><?= num(n($td['hosts'] ?? 0) + n($td['joiners'] ?? 0)) ?></b><span>players today (hosts + joiners)</span></div>
  <div class="card tile"><b><?= h($rate) ?></b><span>of reported connections worked</span></div>
</div>

<h2>Games hosted per day (last 90 days)</h2>
<div class="card"><?= chart_bars($dl, $dv, 'Games hosted per day', ' games', 15) ?></div>

<h2>Players per day (last 90 days)</h2>
<div class="card"><?= chart_bars($dl, $dp, 'Different hosts and joiners per day', ' players', 15) ?>
<p class="mut">Different hosts plus different joiners each day; someone who hosted and joined on the same day counts twice.</p></div>

<?php if (count($months) > 1): ?>
<h2>Games hosted per month</h2>
<div class="card"><?= chart_bars(array_keys($months), array_values($months), 'Games hosted per month', ' games', max(1, (int)ceil(count($months) / 12))) ?></div>
<?php endif; ?>

<h2>Games</h2>
<div class="card">
<?php if ($per_game): ?>
<table><tr><th class="l">game</th><th>games played</th><th>time in games</th><th>time racing</th><th>players per game</th></tr>
<?php foreach ($per_game as $g => $pg): ?>
<tr><td class="l"><?= h($g) ?></td><td><?= num($pg['n']) ?></td><td><?= h(hm($pg['min'])) ?></td><td><?= h(hm($pg['race'])) ?></td><td><?= h(number_format($pg['p'] / max(1, $pg['n']), 1)) ?></td></tr>
<?php endforeach; ?></table>
<?php endif; ?>
<p class="mut">Games hosted, all time:</p><?= chart_hbars($games) ?></div>

<h2>How many players</h2>
<div class="card"><?= chart_bars(array_map(function ($i) { return $i == 8 ? '8+' : (string)$i; }, array_keys($by_players)), array_values($by_players), 'Games by most players', ' games') ?></div>

<h2>How long games last</h2>
<div class="card"><?= chart_bars(array_keys($len_b), array_values($len_b), 'Games by length', ' games') ?></div>

<h2>Busiest hours (UTC)</h2>
<div class="card"><?= chart_bars(array_map(function ($i) { return sprintf('%02d', $i); }, range(0, 23)), $hours, 'Games hosted by hour', ' games') ?></div>

<h2>okplay versions hosting (last 90 days)</h2>
<div class="card"><?= chart_hbars($vers) ?></div>

<h2>Latest games</h2>
<div class="card">
<?php if (!$sess): ?><p class="mut">Nothing yet: games are listed here once they end.</p><?php else: ?>
<table><tr><th class="l">day</th><th>started (UTC)</th><th class="l">game</th><th>players</th><th>length</th><th>racing</th><th>asked to join</th></tr>
<?php foreach (array_slice(array_reverse($sess), 0, RECENT) as $r): ?>
<tr><td class="l"><?= h($r['d']) ?></td><td><?= h(sprintf('%02d:00', n($r['h'] ?? 0))) ?></td><td class="l"><?= h(safe_game($r['g'] ?? '')) ?></td>
<td><?= num(n($r['p'] ?? 0)) ?></td><td><?= h(hm(n($r['m'] ?? 0))) ?></td><td><?= h(hm(n($r['r'] ?? 0))) ?></td><td><?= num(n($r['j'] ?? 0)) ?></td></tr>
<?php endforeach; ?></table>
<?php endif; ?></div>

<p class="mut">Oldkid.net Online Play is open source: <a href="https://github.com/Patrick-Working/okplay">okplay on GitHub</a>. Page built <?= h(gmdate('Y-m-d H:i')) ?> UTC.</p>
<?php
$body = ob_get_clean();
$page = '<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
   . '<title>Online Play usage</title><style>'
   . ':root{--bg:#f6f7f9;--card:#fff;--fg:#1c2330;--mut:#667085;--line:#e3e6eb;--acc:#2f6fdf}'
   . '@media (prefers-color-scheme:dark){:root{--bg:#12151b;--card:#1b2029;--fg:#e6e9ef;--mut:#98a2b3;--line:#2b3240;--acc:#6b9cff}}'
   . 'body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,Segoe UI,sans-serif}'
   . 'main{max-width:1000px;margin:0 auto;padding:16px}h1{font-size:20px;margin:4px 0 8px}h2{font-size:16px;margin:22px 0 8px}a{color:var(--acc)}'
   . '.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px;margin:10px 0;overflow-x:auto}'
   . 'table{border-collapse:collapse;width:100%}td,th{padding:4px 8px;border-bottom:1px solid var(--line);text-align:right;white-space:nowrap}'
   . 'td:first-child,th:first-child,.l{text-align:left}th{color:var(--mut);font-weight:600}.hb .w{width:60%}.hb .v{width:1%}'
   . '.tiles{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}.tile b{display:block;font-size:22px}.tile span{color:var(--mut)}'
   . '.bar{display:inline-block;height:10px;background:var(--acc);border-radius:2px;vertical-align:middle}.mut{color:var(--mut)}'
   . '.chart{width:100%;height:auto;display:block}.chart .b{fill:var(--acc)}.chart .grid{stroke:var(--line);stroke-width:1}'
   . '.chart .ax{fill:var(--mut);font-size:11px}'
   . '</style></head><body><main>' . $body . '</main></body></html>';
echo $page;
@file_put_contents(CACHE, GUARD . $page, LOCK_EX);
