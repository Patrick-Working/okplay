<?php
// latest.php - a link that always gives the newest Oldkid.net Online Play download.
//
// Put it in the folder that holds the release zips (https://oldkid.net/aoe2net/). Each call looks at the
// zips there and redirects to the one with the highest version, so nothing needs editing at release time:
// upload the new Oldkid.net-Online-Play-v<version>.zip next to the old ones and the link follows.
//
//   latest.php          -> 302 redirect to Oldkid.net-Online-Play-v<newest>.zip
//   latest.php?info=1   -> plain text: "<version> <file name>" (for scripts and the web page)
//
// Only full packs (Oldkid.net-Online-Play-v*.zip) count; the older drop-in-only zips
// (...-dropin-v*.zip) are a different name and are never picked.

$best = null;
$best_ver = null;
foreach (glob(__DIR__ . '/Oldkid.net-Online-Play-v*.zip') ?: [] as $path) {
    if (!preg_match('/^Oldkid\.net-Online-Play-v(\d+(?:\.\d+){0,3})\.zip$/', basename($path), $m)) continue;
    if ($best_ver === null || version_compare($m[1], $best_ver, '>')) {
        $best_ver = $m[1];
        $best = basename($path);
    }
}

header('Cache-Control: no-store');   // a new release must show at once
if ($best === null) {
    http_response_code(404);
    header('Content-Type: text/plain; charset=utf-8');
    echo "No download found.\n";
    exit;
}
if (isset($_GET['info'])) {
    header('Content-Type: text/plain; charset=utf-8');
    echo $best_ver . ' ' . $best . "\n";
    exit;
}
header('Location: ' . rawurlencode($best), true, 302);
