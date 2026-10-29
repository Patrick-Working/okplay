# Self-hosting Classic Netplay for AoE2

*Draft — to be finalized when the project wraps up.*

Classic Netplay for AoE2 adds two connection types to Age of Empires II's Multiplayer screen:
**Online** (internet games found through a small web server) and **LAN** (found by broadcast, no
server at all). This page is for running your own server, so your group has its own game list
and its own names in the game.

> It's a standard DirectPlay 4 connection type, so it should also help other classic DirectPlay 4
> games — but only AoE2 has been tested.

## What the server does (and doesn't)

- It only **introduces** players: hosts list their game, joiners find it and say "I'm coming".
  The game itself flows **directly between players**; it never passes through your server.
- It's one PHP file (`okplay.php`) plus an optional web page (`index.html`). No database, no
  background process, no UDP ports to open on the server.
- **LAN games never touch the server.**

## You need

- Web hosting with **PHP 7.4 or newer** (8.x is fine) that can **write files** in the folder you
  use (it keeps the room list in `okplay_rooms.php` next to the script - a PHP file that answers
  404 to anyone who fetches it, because it holds hosts' tokens and addresses; the plain
  `okplay_rooms.json` that versions before 0.7.1 wrote is deleted automatically).
- HTTPS recommended (the game connects with Windows' built-in HTTPS client).

## 1. Set up the server

1. Pick a folder on your site, e.g. `https://example.com/netplay/`.
2. Open `okplay.php` and change `APP_KEY` to your own random string (letters and digits,
   20+ characters). It's not a password — every player's settings contain it — it just keeps
   random internet traffic out.
3. Upload `okplay.php`, and optionally `index.html` (live game list + download button), your
   release zip and the `source` folder (the LGPL source the page links to).
4. Check it: open `https://example.com/netplay/okplay.php?a=list&fmt=json` in a browser. You should
   see `{"rooms": [], ...}`.
5. Optional full check from the source tree (19 tests):
   `OKP_KEY=<your key> python tests/rdv_conformance.py https://example.com/netplay/okplay.php`

## 2. Point the players at it

Edit **`okplay.ini`** in the release folder (it goes next to `dplayx.dll` in the game folder):

```ini
online_name=MyClan Online Play     ; what players pick in the game for internet games
lan_name=MyClan LAN Play           ; ... and for same-network games
server=https://example.com/netplay/okplay.php
key=<the same APP_KEY you put in okplay.php>
```

Players copy `dplayx.dll`, `okindeo.dll` and your `okplay.ini` into the game folder, as the
README says. Changing the names later? Hand out the new `okplay.ini`; the game reads it at start.

**Building from source instead?** Put the same values (plus `brand=`, the name on the download
page and the release folder) in `okplay/package/okplay.ini` and run `winedplay/package.sh`: it
fills in the README, `okplay.ini` and `index.html`, and produces the release folder, the zip and an
"upload to <your host>" folder with `okplay.php`, `index.html`, the zip and the LGPL source.

## 3. Usage stats and reports (optional, from v0.8.3)

`okplay.php` keeps daily counts of how the server is used. Players' PCs can also send it reports:
crash reports (only after the player says yes) and connection reports (only after the player
turns them on). `okstats.php` shows both, behind a password.

1. Upload `okstats.php` next to `okplay.php`.
2. Create `okplay_admin.php` in the same folder with your own password (8+ characters):
   ```php
   <?php define('OKSTATS_PASSWORD', 'pick-a-long-password');
   ```
   It is a PHP file, so the web server never shows its contents.
3. Open `https://example.com/netplay/okstats.php` and sign in.

What you see:
- **Overview:** games hosted, join attempts, how many different hosts, joiners and game-list
  viewers, the most games at once, Connection Check test games, versions and games in use, and
  the busiest hours (UTC). Kept 400 days.
- **Crash reports:** grouped by where the crash happened, with a count, versions, game exe and
  Windows versions. Open the latest to read it.
- **Connection reports:** one line per game, e.g. `result=ok`, `no_path` (no connection to the
  host could be made) or `refused_<error>` (the host turned the join down). Includes the ping and
  how many messages had to be resent.
- **Connection Check reports:** what players sent with Connection Check's *Send report...*
  button. Each report has an id (like `1017-a3f09c`) that the player is shown, and the *Find* box
  opens it.

What is stored: the server counts "different" PCs with a salted hash of the address. The salt
changes every day and the old one is thrown away, so no address is kept and one day can't be
matched with another. Reports carry the same daily hash (to see that one PC sent several), never
the address, and are deleted after 90 days. The files are `okplay_stats.php` and the
`okplay_reports/` folder. Like the room store, they are PHP files that answer 404. Limits: 40
reports per address per day; at most 5000 different PCs counted per day and kind.

## Good to know

- **Separate servers are separate worlds.** Players only see games on the server their
  `okplay.ini` points to. One PC points at one server at a time.
- **Keep versions in step.** Everyone in a game needs the same Classic Netplay version; upload the
  `okplay.php` that came with the release you hand out.
- **What the server stores**, per open game: its name, player counts, the host's connection
  addresses (IP:port), an opaque DirectPlay session record, timestamps. A game disappears 15 seconds
  after its host stops announcing. A joiner's addresses wait at most 20 seconds for the host to
  collect them. The public list (`fmt=json`, used by `index.html`) never includes addresses; the
  keyed list the game asks for does include hosts' addresses, and the key is in every player's
  `okplay.ini` - treat it as public.
- **Limits:** 200 games in all, 4 per client address, 16 joiners waiting per game (a 5th game or a
  17th joiner gets HTTP 429 and the game retries). Change the constants at the top of `okplay.php`.
- **Load is tiny:** a hosting player makes about one request per second; a player on the Join
  screen about one every 2–3 seconds; replies are a few hundred bytes. Shared hosting is plenty
  for dozens of players.
- **Offline / local testing:** `server/rdv_standin.py` implements the same API
  (`python server/rdv_standin.py --www server` also serves the web page).
- **Finding your public IP** uses public STUN servers (Cloudflare, Google, Nextcloud), with
  your server's view of the player's IP as a fallback. You can set your own list with
  `stun=host:port,host:port` in `okplay.ini`.
