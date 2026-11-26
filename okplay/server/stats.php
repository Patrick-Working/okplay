<?php
/*
 * Oldkid.net Online Play - public usage statistics (read-only, no sign-in).
 * ------------------------------------------------------------------------
 * Script version: 2026-11-25c (change this line with every upload).
 * Deploy next to okplay.php. Reads what okplay.php keeps, forever:
 *   okplay_stats.php            daily totals (games hosted, join attempts, how many different hosts / joiners /
 *                               list viewers that day, games, okplay versions, hours, connection reports' results)
 *   okplay_sessions/<YYYY-MM>.php  one line per game that ended: day, start hour (UTC), game, okplay version,
 *                               minutes open, minutes racing, most players, players who asked to join
 * and publishes a few totals, one chart, games by title and the latest games. Never shown: the day's salt and address hashes, reports and what is in them,
 * room or player names (not stored), addresses (not stored). Game and version names come from players' PCs:
 * only names that look like a game are shown as they are, the rest count as "other".
 * Every value is escaped. The page is built at most every CACHE_S seconds (okplay_stats_page.php, a PHP file that
 * answers 404 when opened directly).
 */

const STATS        = __DIR__ . '/okplay_stats.php';
const SESSIONS_DIR = __DIR__ . '/okplay_sessions';
const CACHE        = __DIR__ . '/okplay_stats_page.php';
const CACHE_S      = 300;
const GUARD        = "<?php http_response_code(404); exit; ?>\n";
const RECENT       = 20;     // games in the table

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
// names from players' PCs: shown only when they look like a game name
function safe_game($k) {
    if (!is_string($k) || preg_match('/^[0-9a-f]{32}$/', $k)) return 'other';
    return preg_match("/^[A-Za-z0-9][A-Za-z0-9 .,:'()&+\\/-]{0,47}$/", $k) ? $k : 'other';
}

// ---- charts: inline SVG, colours from the page's own tokens
function chart_bars($labels, $values, $title, $unit = '', $label_every = 1) {
    $n = count($values);
    if (!$n || !array_sum($values)) return '<p class="mut">Nothing yet.</p>';
    $W = 900; $H = 150; $L = 44; $B = 28; $T = 8;
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

// ---- the numbers
$s    = read_stats();
$days = isset($s['days']) && is_array($s['days']) ? $s['days'] : [];
$days = array_filter($days, function ($d, $k) { return is_array($d) && preg_match('/^\d{4}-\d{2}-\d{2}$/', $k); }, ARRAY_FILTER_USE_BOTH);
ksort($days);
$sess  = read_sessions();
$today = gmdate('Y-m-d');
$first = $days ? (string)key($days) : $today;   // (ksort: the first day)
$all   = ['hosted' => 0];
foreach ($days as $d) $all['hosted'] += n($d['hosted'] ?? 0);
$td = $days[$today] ?? [];

// games hosted per day, last 90 days (days with nothing count as 0)
$dl = []; $dv = [];
for ($i = 89; $i >= 0; $i--) {
    $k = gmdate('Y-m-d', time() - $i * 86400);
    $dl[] = substr($k, 5); $dv[] = n($days[$k]['hosted'] ?? 0);
}

// games that ended
$play_min = 0; $pl = 0; $plc = 0; $per_game = [];
foreach ($sess as $r) {
    $m = max(0, n($r['m'] ?? 0)); $p = max(0, n($r['p'] ?? 0)); $g = safe_game($r['g'] ?? '');
    $play_min += $m;
    if ($p > 0) { $pl += $p; $plc++; }
    $pg = &$per_game[$g]; $pg['n'] = ($pg['n'] ?? 0) + 1; $pg['min'] = ($pg['min'] ?? 0) + $m; $pg['p'] = ($pg['p'] ?? 0) + $p; unset($pg);
}
uasort($per_game, function ($a, $b) { return $b['n'] <=> $a['n']; });

function hm($min) { $min = (int)$min; return $min >= 60 ? sprintf('%d h %02d min', intdiv($min, 60), $min % 60) : $min . ' min'; }

ob_start();
?>
<h1>Oldkid.net Online Play - usage</h1>
<p class="mut">Since <?= h($first) ?>. No names or addresses are kept. Times are UTC.</p>

<div class="tiles">
  <div class="card tile"><b><?= num($all['hosted']) ?></b><span>games hosted</span></div>
  <div class="card tile"><b><?= num(round($play_min / 60)) ?> h</b><span>played</span></div>
  <div class="card tile"><b><?= $plc ? h(number_format($pl / $plc, 1)) : '-' ?></b><span>players per game</span></div>
  <div class="card tile"><b><?= num(n($td['hosts'] ?? 0) + n($td['joiners'] ?? 0)) ?></b><span>players today</span></div>
</div>

<div class="card"><p class="mut">Games hosted per day, last 90 days</p><?= chart_bars($dl, $dv, 'Games hosted per day', ' games', 15) ?></div>

<?php if ($per_game): ?>
<div class="card"><table><tr><th class="l">game</th><th>games</th><th>time played</th><th>players per game</th></tr>
<?php foreach ($per_game as $g => $pg): ?>
<tr><td class="l"><?= h($g) ?></td><td><?= num($pg['n']) ?></td><td><?= h(hm($pg['min'])) ?></td><td><?= h(number_format($pg['p'] / max(1, $pg['n']), 1)) ?></td></tr>
<?php endforeach; ?></table></div>
<?php endif; ?>

<?php if ($sess): ?>
<div class="card"><p class="mut">Latest games</p>
<table><tr><th class="l">day</th><th class="l">game</th><th>players</th><th>length</th></tr>
<?php foreach (array_slice(array_reverse($sess), 0, RECENT) as $r): ?>
<tr><td class="l"><?= h($r['d']) ?></td><td class="l"><?= h(safe_game($r['g'] ?? '')) ?></td><td><?= num(n($r['p'] ?? 0)) ?></td><td><?= h(hm(n($r['m'] ?? 0))) ?></td></tr>
<?php endforeach; ?></table></div>
<?php endif; ?>

<p class="mut"><a href="https://github.com/Patrick-Working/okplay">okplay on GitHub</a></p>
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
   . 'td:first-child,th:first-child,.l{text-align:left}th{color:var(--mut);font-weight:600}'
   . '.tiles{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}.tile b{display:block;font-size:22px}.tile span{color:var(--mut)}'
   . '.mut{color:var(--mut)}'
   . '.chart{width:100%;height:auto;display:block}.chart .b{fill:var(--acc)}.chart .grid{stroke:var(--line);stroke-width:1}'
   . '.chart .ax{fill:var(--mut);font-size:11px}'
   . '</style></head><body><main>' . $body . '</main></body></html>';
echo $page;
@file_put_contents(CACHE, GUARD . $page, LOCK_EX);
