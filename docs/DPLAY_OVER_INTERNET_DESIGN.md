# Generic DirectPlay over a hole-punched Internet connection: design sketch

Status: design notes only (2026-10-09), written from a study of the drop-in's source. Evidence tags: [V-RT] read in source, [INF] reasoned, [UNV] needs a check.

## 1. Goal
Let any DirectPlay (DP 1-4 era) game play over the Internet without port forwarding or VPN, with the same semantics the game saw on the original IPX / TCP/IP / modem providers, by implementing the DirectPlay layers ourselves and carrying them over one hole-punched path per peer pair.

## 2. Where it can plug in (choose per target)
| Plug point | What we write | Pros | Cons |
|---|---|---|---|
| A. Service provider under Windows' dplayx | transport only (SP callbacks: Send/SendEx, EnumSessions, CreatePlayer ...) | small; keeps Microsoft's session logic | needs the optional "DirectPlay" Windows feature; Windows' dplayx may mishandle unreliable SPs (okplay found 2 bugs) |
| B. Drop-in dplayx.dll (Wine-based, what the netplay project does) | session layer + protocol layer + SP | no Windows feature; full control; same file works on old and new Windows | Wine's dplayx is incomplete: it has no guaranteed-delivery layer [V-RT: dplayx_main.c notes]; must be completed |
| C. Per-game shim (hook the game's own DP calls) | only what one game uses | most robust for one game | does not generalise |

Recommendation: B as the foundation, with per-game profiles (section 6).

## 3. Layers to implement or verify
1. **API**: IDirectPlay / 2 / 3 / 4 and IDirectPlayLobby (CreatePlayer, DestroyPlayer, groups, Send/SendEx/Receive, SetPlayerData, EnumPlayers, GetCaps, Open/Close, EnumSessions, SetSessionDesc). Games differ in which they use; trace real games first.
2. **Session layer**: session enumeration (broadcast / directed), join handshake, player/group lifecycle, DPSYS_* system messages (CREATEPLAYERORGROUP, DESTROYPLAYERORGROUP, ADDPLAYERTOGROUP, SESSIONLOST, HOST, SETPLAYERORGROUPDATA ...), host migration (DPSESSION_MIGRATEHOST), keep-alive and timeouts.
3. **Protocol layer** (DX6+): sequencing, ack/retransmit and ordering for DPSEND_GUARANTEED when the SP is unreliable; fragmentation to the SP's dwMaxBufferSize; priority (HIGHPRIORITY), async sends and send-complete messages. [UNV] confirm the exact ordering guarantees of real DirectPlay (per sender, per destination, vs unguaranteed messages).
4. **Transport**: one UDP path per peer pair, see section 4.
5. **Rendezvous / NAT traversal**: see section 5.

## 4. Transport design (the part the discussion was about)
- **Four delivery classes** mapped from DirectPlay flags and a per-game profile: reliable-ordered (DPSEND_GUARANTEED and all system messages), reliable-unordered (optional), unreliable-sequenced (latest wins; for periodic state), plain unreliable. Reliable data never blocks unreliable data: separate queues and independent retransmission.
- **Why not plain TCP**: head-of-line blocking delays the 4-8 Hz state packets of MTM2-like games behind a retransmission, and TCP hole punching is harder (simultaneous open, fewer NAT behave). **Why not UDP-only with everything best-effort**: loses lobby/barrier/event messages. So: UDP with our own reliable lane, or a QUIC-style connection (streams + datagrams over one 5-tuple; one hole punch, reliable streams per message class, no cross-stream blocking). [INF] An existing library (QUIC, ENet or similar) could replace the hand-written lane; a hand-written lane exists and is fuzzed in the netplay source.
- **Congestion/pacing**: DirectPlay-era games send small packets at low rates, so a simple window plus pacing is enough; cap bursts after stalls.
- **Message size**: report a large dwMaxBufferSize and fragment ourselves (the netplay project found Windows' packetizer corrupts big player lists).
- **Failure rule**: a reliable message that cannot be delivered must fail the peer (drop player), not stall the whole stream forever (netplay README lists this as a known limitation).

## 5. Rendezvous and NAT traversal
- Candidate gathering ICE-style: local, IPv6, STUN-reflexive; simultaneous probes (hole punching); prefer IPv6 and LAN.
- Add **port mapping** (UPnP IGD / NAT-PMP / PCP) as a first try for hosts that can open a port (suggested in the discussion).
- Add a **relay fallback** for symmetric-NAT pairs (UDP relay or TCP/443 relay); the current drop-in has none, so both-symmetric pairs fail.
- Matchmaker stays discovery-only and never lists IP addresses; peer ids instead of addresses; token on every packet; hash passwords with the session id.

## 6. Per-game profiles
Games differ in which messages need reliability. A profile keyed by the application GUID tells the transport: default reliable for everything, plus droppable message ids (for MTM2: 0x02 vehicle state, 0x17 positions, 0x89 voice) and optional ordering rules. Unknown games run reliable-for-all (safe, slightly slower). Profiles are produced by tracing (our hook DLL's API trace can log every IDirectPlay Send with flags, size and first bytes) and then confirmed with the loss tests.

## 7. Verification
- Conformance tests of DirectPlay semantics (ordering, completion, system messages, host migration) against a recorded Windows DirectPlay baseline where possible.
- Network simulators (the drop-in has loss, NAT-type matrices and a soak test): 5/15/30% loss, jitter, reordering, 8 players, host quits mid-game.
- Per-game acceptance: lobby, start barrier, 10-minute session, disconnect/rejoin.

## 8. Open questions (verify before building)
1. Exact DirectPlay guaranteed-delivery ordering rules [UNV].
2. RESOLVED [V-RT, docs/NETPLAY_ARCHITECTURE.md]: the drop-in v0.8.0 does NOT honour DPSEND_GUARANTEED on the wire: the flag reaches the provider but `okp_send_data` has no flags parameter; the reliable lane is chosen only for the "play" system envelope and messages over 1200 bytes. DirectPlay's own guaranteed-delivery layer is not implemented either. Fix list P1-P6 is in section 4.5 of that doc.
3. Which games (beyond AoE2 and MTM2) use groups, lobby launching, secure sessions or modem/serial-specific caps.
4. Licensing: Wine's dplayx is LGPL; anything built from it keeps that licence.
