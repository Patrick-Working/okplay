
    // ---- okplay room server (internet play) ----
    // Speaks the okplay rendezvous API (github.com/Patrick-Working/okplay, okplay/server/okplay.php):
    //   host:   a=announce (~1/s) -> knocks from joiners, whom it punches toward
    //   search: a=list&app=<UFO 50 app id>
    //   joiner: a=knock (every 3 s while joining), then ENet-connects to every host candidate
    //   host:   a=close when the lobby ends
    // Candidates are "ip:port" lists: our interface addresses plus the public address STUN reports
    // for the ENet socket. HTTP runs on a worker thread (through Java, see PFO_Android.okpPost);
    // everything else stays on the game thread.
    // Config: files/okplay.txt (or Android/data/<pkg>/files/okplay.txt): first line the server URL,
    // or "off". Default: the public oldkid.net server.
    constexpr const char* c_OkpDefaultUrl = "https://oldkid.net/aoe2net/okplay.php";
    constexpr const char* c_OkpKey = "okp_7Qm2xV9rLs4Hd8Wn";
    constexpr const char* c_OkpApp = "5f0a50e8c3b14d2a9e7f6c1b2d3a4e50"; // UFO 50 + PFO 50 (Android port)
    constexpr int c_OkpWire = 50;            // Windows DirectPlay drop-ins (wire 4) and web (100) skip these rooms
    constexpr const char* c_StunServers[] = { "stun.cloudflare.com:3478", "stun.l.google.com:19302", "stun1.l.google.com:19302" };
    constexpr uint32 c_StunMagic = 0x2112A442u;

    std::string (*g_HttpPost)(const std::string& url, const std::string& body) = nullptr; // set by the runner bridge

    bool g_OkpEnabled = false;
    std::string g_OkpUrl;
    std::string g_OkpTok;
    uint32 g_OkpPublicIp = 0;          // what the room server saw (network order)
    ENetAddress g_StunAddr[4] = {};
    int g_StunCount = 0;
    bool g_StunResolved = false;
    uint8 g_StunTx[12] = {};
    int g_StunTries = 0;
    int64 g_StunNext = 0;
    bool g_HaveSrflx = false;
    ENetAddress g_Srflx = {};
    int64 g_OkpNextAnnounce = 0, g_OkpNextKnock = 0;
    bool g_OkpAnnounced = false;
    uint32 g_OkpAnnouncedRoom = 0;
    uint32 g_OkpJoinRoom = 0;          // room we are knocking on

    enum class OkpJob { Announce, List, Knock, Close, ResolveStun };
    struct OkpTask { OkpJob kind; std::string body; };
    struct OkpResult { OkpJob kind; int status; std::string text; std::vector<ENetAddress> addrs; };
    std::mutex g_OkpMutex;
    std::condition_variable g_OkpCv;
    std::deque<OkpTask> g_OkpTasks;
    std::deque<OkpResult> g_OkpResults;
    bool g_OkpBusy[5] = {};            // one request of each kind in flight at a time
    bool g_OkpThreadStarted = false;

    std::string UrlEnc(const std::string& s)
    {
        static const char* hex = "0123456789ABCDEF";
        std::string o;
        for (unsigned char ch : s)
        {
            if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') o += static_cast<char>(ch);
            else { o += '%'; o += hex[ch >> 4]; o += hex[ch & 15]; }
        }
        return o;
    }

    std::string UrlDec(const std::string& s)
    {
        std::string o;
        for (size_t i = 0; i < s.size(); i++)
        {
            if (s[i] == '%' && i + 2 < s.size()) { o += static_cast<char>(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; }
            else if (s[i] == '+') o += ' ';
            else o += s[i];
        }
        return o;
    }

    std::string B64Enc(const std::vector<uint8>& b)
    {
        static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string o;
        for (size_t i = 0; i < b.size(); i += 3)
        {
            uint32 v = b[i] << 16 | (i + 1 < b.size() ? b[i + 1] << 8 : 0) | (i + 2 < b.size() ? b[i + 2] : 0);
            o += t[v >> 18 & 63]; o += t[v >> 12 & 63];
            o += i + 1 < b.size() ? t[v >> 6 & 63] : '=';
            o += i + 2 < b.size() ? t[v & 63] : '=';
        }
        return o;
    }

    std::vector<uint8> B64Dec(const std::string& s)
    {
        auto val = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+' || c == '-') return 62;
            if (c == '/' || c == '_') return 63;
            return -1;
        };
        std::vector<uint8> o;
        uint32 acc = 0; int bits = 0;
        for (char c : s)
        {
            int v = val(c);
            if (v < 0) continue;
            acc = acc << 6 | v; bits += 6;
            if (bits >= 8) { bits -= 8; o.push_back(static_cast<uint8>(acc >> bits)); }
        }
        return o;
    }

    std::string AddrStr(const ENetAddress& a)
    {
        char ip[32];
        inet_ntop(AF_INET, &a.host, ip, sizeof(ip));
        return std::string(ip) + ":" + std::to_string(a.port);
    }

    bool ParseAddr(const std::string& s, ENetAddress& a)
    {
        size_t colon = s.rfind(':');
        if (colon == std::string::npos || s[0] == '[') return false; // IPv4 only
        in_addr in;
        if (!inet_aton(s.substr(0, colon).c_str(), &in)) return false;
        a.host = in.s_addr;
        a.port = static_cast<uint16>(atoi(s.c_str() + colon + 1));
        return a.port != 0;
    }

    std::vector<uint32> LocalIps()
    {
        std::vector<uint32> ips;
        ifaddrs* ifs = nullptr;
        if (getifaddrs(&ifs) == 0)
        {
            for (ifaddrs* i = ifs; i; i = i->ifa_next)
                if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !(i->ifa_flags & IFF_LOOPBACK) && (i->ifa_flags & IFF_UP))
                    ips.push_back(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
            freeifaddrs(ifs);
        }
        return ips;
    }

    std::string LocalCands()
    {
        std::vector<std::string> c;
        ENetAddress upnpCand;
        if (UpnpCandidate(upnpCand)) c.push_back(AddrStr(upnpCand));   // the router forwards this port to us
        if (g_HaveSrflx) c.push_back(AddrStr(g_Srflx));
        for (uint32 ip : LocalIps()) { ENetAddress a; a.host = ip; a.port = g_Port; c.push_back(AddrStr(a)); }
        if (!g_HaveSrflx && g_OkpPublicIp) { ENetAddress a; a.host = g_OkpPublicIp; a.port = g_Port; c.push_back(AddrStr(a)); } // port-preserving NAT guess
        // Inside an emulator (BlueStacks / Android emulator, guest address 10.0.2.x) the host PC is
        // 10.0.2.2 and the emulator's NAT socket on the PC usually has the same port the router shows
        // STUN, so other emulators on this PC can reach us there without hairpin NAT on the router.
        bool emulator = false;
        for (uint32 ip : LocalIps()) if ((ntohl(ip) & 0xffffff00u) == 0x0a000200u) emulator = true;
        if (emulator && g_HaveSrflx) { ENetAddress a; inet_aton("10.0.2.2", reinterpret_cast<in_addr*>(&a.host)); a.port = g_Srflx.port; c.push_back(AddrStr(a)); }
        std::string o;
        for (size_t i = 0; i < c.size() && i < 6; i++) { if (i) o += ','; o += c[i]; }
        return o;
    }

    bool IsSelf(const ENetAddress& a)
    {
        if (a.port != g_Port) return false;
        for (uint32 ip : LocalIps()) if (ip == a.host) return true;
        return g_HaveSrflx && g_Srflx.host == a.host && g_Srflx.port == a.port;
    }

    void OkpWorker()
    {
        while (true)
        {
            OkpTask t;
            {
                std::unique_lock<std::mutex> l(g_OkpMutex);
                g_OkpCv.wait(l, [] { return !g_OkpTasks.empty(); });
                t = std::move(g_OkpTasks.front());
                g_OkpTasks.pop_front();
            }
            OkpResult r{ t.kind, 0, {}, {} };
            if (t.kind == OkpJob::ResolveStun)
            {
                for (const char* spec : c_StunServers)
                {
                    std::string s = spec;
                    size_t colon = s.rfind(':');
                    addrinfo hints = {}, *res = nullptr;
                    hints.ai_family = AF_INET;
                    hints.ai_socktype = SOCK_DGRAM;
                    if (getaddrinfo(s.substr(0, colon).c_str(), s.substr(colon + 1).c_str(), &hints, &res) == 0 && res)
                    {
                        auto* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
                        ENetAddress a; a.host = sin->sin_addr.s_addr; a.port = ntohs(sin->sin_port);
                        r.addrs.push_back(a);
                        freeaddrinfo(res);
                    }
                }
            }
            else if (g_HttpPost)
            {
                std::string reply = g_HttpPost(g_OkpUrl, t.body);
                size_t nl = reply.find('\n');
                r.status = atoi(reply.c_str());
                r.text = nl == std::string::npos ? std::string() : reply.substr(nl + 1);
            }
            std::lock_guard<std::mutex> l(g_OkpMutex);
            g_OkpResults.push_back(std::move(r));
        }
    }

    void OkpSubmit(OkpJob kind, std::string body)
    {
        int k = static_cast<int>(kind);
        if (g_OkpBusy[k] && kind != OkpJob::Close) return;
        g_OkpBusy[k] = true;
        if (!g_OkpThreadStarted) { g_OkpThreadStarted = true; std::thread(OkpWorker).detach(); }
        std::lock_guard<std::mutex> l(g_OkpMutex);
        g_OkpTasks.push_back({ kind, std::move(body) });
        g_OkpCv.notify_one();
    }

    std::string OkpBase(const char* action)
    {
        return std::string("k=") + c_OkpKey + "&a=" + action;
    }

    std::string Hex(uint64 v, int digits)
    {
        char b[24];
        snprintf(b, sizeof(b), "%0*llx", digits, static_cast<unsigned long long>(v));
        return b;
    }

    uint32 RoomOf(uint64 lobbyId) { return static_cast<uint32>(lobbyId & 0xffffffffu); }
    uint64 LobbyOfRoom(uint32 room) { return 0x2000000000000000ull | room; }

    void LoadOkplayConfig()
    {
        g_OkpUrl = c_OkpDefaultUrl;
        std::string ext = "/sdcard/Android/data/" PFO_PACKAGE "/files/okplay.txt";
        for (const char* path : { "okplay.txt", ext.c_str() })
        {
            if (FILE* f = fopen(path, "r"))
            {
                char line[512] = {};
                if (fgets(line, sizeof(line), f)) { line[strcspn(line, "\r\n \t")] = 0; if (line[0]) g_OkpUrl = line; }
                fclose(f);
                break;
            }
        }
        g_OkpEnabled = g_OkpUrl != "off";
        char tok[20];
        snprintf(tok, sizeof(tok), "%08x%08x", static_cast<unsigned>(rand()), static_cast<unsigned>(g_MyId));
        g_OkpTok = tok;
        if (g_OkpEnabled) OkpSubmit(OkpJob::ResolveStun, {});
        __android_log_print(ANDROID_LOG_INFO, "yoyo", "PFO LAN: okplay server %s", g_OkpUrl.c_str());
    }

    // STUN binding request/response on the ENet socket (so the mapping is the game's own)
    void StunSend()
    {
        uint8 req[20] = { 0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42 };
        for (int i = 0; i < 12; i++) g_StunTx[i] = req[8 + i] = static_cast<uint8>(rand());
        ENetBuffer buf; buf.data = req; buf.dataLength = sizeof(req);
        for (int i = 0; i < g_StunCount; i++) enet_socket_send(g_Host->socket, &g_StunAddr[i], &buf, 1);
    }

    bool StunHandle(const uint8* b, size_t n)
    {
        if (n < 20 || b[0] != 0x01 || b[1] != 0x01) return false;
        if (((uint32)b[4] << 24 | (uint32)b[5] << 16 | (uint32)b[6] << 8 | b[7]) != c_StunMagic) return false;
        if (memcmp(b + 8, g_StunTx, 12) != 0) return true; // a late answer to an older request
        size_t o = 20;
        while (o + 4 <= n)
        {
            uint16 type = static_cast<uint16>(b[o] << 8 | b[o + 1]);
            uint16 len = static_cast<uint16>(b[o + 2] << 8 | b[o + 3]);
            if (o + 4 + len > n) break;
            const uint8* v = b + o + 4;
            if ((type == 0x0020 || type == 0x0001) && len >= 8 && v[1] == 0x01)
            {
                uint16 port = static_cast<uint16>(v[2] << 8 | v[3]);
                uint32 ip = (uint32)v[4] << 24 | (uint32)v[5] << 16 | (uint32)v[6] << 8 | v[7];
                if (type == 0x0020) { port ^= 0x2112; ip ^= c_StunMagic; }
                g_Srflx.host = htonl(ip);
                g_Srflx.port = port;
                g_HaveSrflx = true;
                __android_log_print(ANDROID_LOG_INFO, "yoyo", "PFO LAN: public address %s (STUN)", AddrStr(g_Srflx).c_str());
                return true;
            }
            o += 4 + ((len + 3) & ~3u);
        }
        return true;
    }

    void PunchToward(const std::string& cands)
    {
        size_t s = 0;
        while (s < cands.size())
        {
            size_t e = cands.find(',', s);
            std::string one = cands.substr(s, e == std::string::npos ? std::string::npos : e - s);
            ENetAddress a;
            if (ParseAddr(one, a) && !IsSelf(a))
            {
                Out o = RvHeader(Rv::Ping); o.u64(g_MyId);
                for (int k = 0; k < 3; k++) RvSend(a, o);
            }
            if (e == std::string::npos) break;
            s = e + 1;
        }
    }

    std::string EnumBlob()
    {
        Out o;
        o.u16(static_cast<uint16>(g_Lobby.members.size())); o.u16(static_cast<uint16>(g_Lobby.limit));
        o.str(g_MyName); o.kv(g_Lobby.data);
        return B64Enc(o.b);
    }

    void OkpAnnounce()
    {
        auto it = g_Lobby.data.find("name");
        std::string name = it != g_Lobby.data.end() ? it->second : g_MyName;
        std::string body = OkpBase("announce")
            + "&room=" + Hex(RoomOf(g_Lobby.id), 8) + "&peer=" + Hex(g_MyId, 16) + "&tok=" + g_OkpTok
            + "&cands=" + UrlEnc(LocalCands()) + "&enum=" + UrlEnc(EnumBlob()) + "&name=" + UrlEnc(name)
            + "&players=" + std::to_string(g_Lobby.members.size()) + "&maxp=" + std::to_string(g_Lobby.limit)
            + "&flags=0&ver=1.0&wire=" + std::to_string(c_OkpWire) + "&app=" + c_OkpApp
            + "&game=" + UrlEnc("UFO 50 (PFO 50)") + "&gamever=1.9.5&state=" + (g_Lobby.joinable ? "lobby" : "playing")
            + "&exe=ufo50";
        g_OkpAnnouncedRoom = RoomOf(g_Lobby.id);
        g_OkpAnnounced = true;
        OkpSubmit(OkpJob::Announce, body);
    }

    void OkpClose()
    {
        if (!g_OkpAnnounced) return;
        g_OkpAnnounced = false;
        OkpSubmit(OkpJob::Close, OkpBase("close") + "&room=" + Hex(g_OkpAnnouncedRoom, 8) + "&tok=" + g_OkpTok);
    }

    // Rooms from okplay list replies, kept for a few seconds whenever the reply arrives, so a slow
    // answer still lands in the next search instead of being dropped.
    std::map<uint32, std::pair<LobbyInfo, int64>> g_OkpSeen;
    int64 g_OkpListSentMs = 0;
    constexpr int64 c_OkpSeenMs = 12000;

    bool OkpListInFlight() { return g_OkpEnabled && g_OkpBusy[static_cast<int>(OkpJob::List)]; }

    void OkpAddRecent(std::vector<LobbyInfo>& list)
    {
        int64 now = NowMs();
        for (auto it = g_OkpSeen.begin(); it != g_OkpSeen.end();)
        {
            if (now - it->second.second > c_OkpSeenMs) { it = g_OkpSeen.erase(it); continue; }
            const LobbyInfo& li = it->second.first;
            bool dup = li.id == g_Lobby.id;
            for (auto& e : list) if (e.id == li.id) dup = true;
            if (!dup) list.push_back(li);
            ++it;
        }
    }

    void OkpRequestList()
    {
        if (!g_OkpEnabled || g_OkpBusy[static_cast<int>(OkpJob::List)]) return;
        g_OkpListSentMs = NowMs();
        OkpSubmit(OkpJob::List, OkpBase("list") + "&app=" + c_OkpApp);
    }

    void OkpKnock()
    {
        OkpSubmit(OkpJob::Knock, OkpBase("knock") + "&room=" + Hex(g_OkpJoinRoom, 8) + "&peer=" + Hex(g_MyId, 16)
            + "&cands=" + UrlEnc(LocalCands()));
    }

    // "ok <ip>" first line -> our public IP as the server saw it
    bool OkpOk(const std::string& text, std::istringstream& lines)
    {
        std::string first;
        if (!std::getline(lines, first) || first.compare(0, 3, "ok ") != 0) return text.compare(0, 2, "ok") == 0;
        in_addr in;
        if (inet_aton(first.substr(3).c_str(), &in)) g_OkpPublicIp = in.s_addr;
        return true;
    }

    void OkpHandle(OkpResult& r)
    {
        g_OkpBusy[static_cast<int>(r.kind)] = false;
        if (r.kind == OkpJob::ResolveStun)
        {
            g_StunCount = 0;
            for (auto& a : r.addrs) if (g_StunCount < 4) g_StunAddr[g_StunCount++] = a;
            g_StunResolved = true;
            return;
        }
        if (r.status != 200)
        {
            __android_log_print(ANDROID_LOG_WARN, "yoyo", "PFO LAN: okplay request %d failed: %d %s", static_cast<int>(r.kind), r.status, r.text.substr(0, 120).c_str());
            return;
        }
        std::istringstream lines(r.text);
        if (!OkpOk(r.text, lines)) return;
        std::string line;
        if (r.kind == OkpJob::Announce)
        {
            while (std::getline(lines, line))
            {
                if (line.compare(0, 6, "knock ") != 0) continue;
                std::istringstream ls(line);
                std::string w, peer, cands;
                ls >> w >> peer >> cands;
                __android_log_print(ANDROID_LOG_INFO, "yoyo", "PFO LAN: okplay knock from %s cands=%s", peer.c_str(), cands.c_str());
                PunchToward(cands);
                RelayStart(RoomOf(g_Lobby.id), 60000);   // and listen on the relay, in case they can't reach us
            }
        }
        else if (r.kind == OkpJob::List)
        {
            int rooms = 0, usable = 0, badWire = 0, badEnum = 0, noCands = 0, self = 0;
            while (std::getline(lines, line))
            {
                if (line.compare(0, 5, "room ") != 0) continue;
                rooms++;
                std::istringstream ls(line);
                std::string w, room, peer, cands, enumb, name;
                int age = 0, wire = 0;
                ls >> w >> room >> peer >> age >> cands >> enumb >> name >> wire;
                if (wire != c_OkpWire) { badWire++; continue; }
                LobbyInfo li;
                li.room = static_cast<uint32>(strtoul(room.c_str(), nullptr, 16));
                li.id = LobbyOfRoom(li.room);
                li.owner = strtoull(peer.c_str(), nullptr, 16);
                li.okplay = true;
                std::vector<uint8> blob = B64Dec(enumb);
                In in(blob.data(), blob.size());
                li.members = in.u16(); li.limit = in.u16(); li.ownerName = in.str(); li.data = in.kv();
                if (!in.ok) { badEnum++; continue; }
                if (li.owner == g_MyId || li.id == g_Lobby.id) { self++; continue; }
                size_t s = 0;
                while (s < cands.size())
                {
                    size_t e = cands.find(',', s);
                    ENetAddress a;
                    if (ParseAddr(cands.substr(s, e == std::string::npos ? std::string::npos : e - s), a) && !IsSelf(a)) li.cands.push_back(a);
                    if (e == std::string::npos) break;
                    s = e + 1;
                }
                if (li.cands.empty()) { noCands++; continue; }
                usable++;
                li.ip = li.cands[0].host; li.port = li.cands[0].port;
                li.joinable = true;
                g_LobbyCache[li.id] = li;
                g_Names[li.owner] = li.ownerName;
                g_OkpSeen[li.room] = { li, NowMs() };
                bool dup = false;
                for (auto& e : g_PendingLobbyList) if (e.id == li.id) { e = li; dup = true; }
                if (!dup && g_ListLoading) g_PendingLobbyList.push_back(li);
            }
            __android_log_print(ANDROID_LOG_INFO, "yoyo", "PFO LAN: okplay list in %lld ms: %d rooms, %d usable (wire %d, bad info %d, no address %d, own %d)%s",
                static_cast<long long>(NowMs() - g_OkpListSentMs), rooms, usable, badWire, badEnum, noCands, self,
                g_ListLoading ? "" : " - after the search finished; kept for the next one");
        }
    }

    void PumpOkplay()
    {
        if (!g_OkpEnabled) return;
        std::deque<OkpResult> results;
        { std::lock_guard<std::mutex> l(g_OkpMutex); results.swap(g_OkpResults); }
        for (auto& r : results) OkpHandle(r);

        int64 now = NowMs();
        if (g_StunResolved && !g_HaveSrflx && g_StunCount > 0 && g_StunTries < 4 && now >= g_StunNext)
        {
            StunSend();
            g_StunTries++;
            g_StunNext = now + 700;
        }
        // keep-alive: while in a lobby or joining, refresh the NAT mapping (and our public address)
        // every 20 s - an idle mapping can expire and make the advertised address stale
        static int64 s_nextKeepAlive = 0;
        if (g_StunResolved && g_StunCount > 0 && (g_InLobby || g_JoinPending) && now >= s_nextKeepAlive)
        {
            s_nextKeepAlive = now + 20000;
            if (g_HaveSrflx) StunSend();
        }
        if (g_InLobby && g_IsOwner && now >= g_OkpNextAnnounce)
        {
            g_OkpNextAnnounce = now + 1000;
            OkpAnnounce();
        }
        else if ((!g_InLobby || !g_IsOwner) && g_OkpAnnounced)
        {
            OkpClose();
        }
        if (g_JoinPending && g_OkpJoinRoom && now >= g_OkpNextKnock)
        {
            g_OkpNextKnock = now + 3000;
            OkpKnock();
        }
    }
