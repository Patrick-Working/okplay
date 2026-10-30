    // ---- netplus: keeping online play connected, whatever the network ----
    // Included inside namespace fakesteam after okplay_client.inc.
    //  1. Background pump: Android pauses the game loop when the app loses the screen; a thread
    //     keeps ENet answering (acks, pings; received events saved for the game thread) and keeps
    //     the okplay room announced, so a short app switch doesn't drop the session or the lobby.
    //     Everything that touches ENet or the lobby runs under g_NetMutex (the game thread holds it
    //     inside every PFO call, see runner_bridge.inc).
    //  2. Keep-alive: while in a lobby, a STUN request every 20 s keeps the NAT mapping (and our
    //     advertised public address) fresh.
    //  3. Relay: okplay's rsend/rpoll mailboxes, for joiners that can't reach the host directly.
    //     Relayed peers get stand-in addresses in 0.0.0.0/8; ENet's socket hooks (tools/enet unix.c)
    //     route their datagrams through the mailboxes.
    //  4. UPnP: a host asks its router to forward the game port (okplay's upnp.c, ported), so
    //     joiners behind strict NATs can connect straight in. On by default; the overlay can turn
    //     it off. The mapping stays up through the match (the game leaves the lobby when a match
    //     starts) and is removed at Quit, or ends by itself with its 1-hour lease.
    //  5. Status for the overlay: how we're connected (LAN / DIRECT / RELAY).

    std::recursive_mutex g_NetMutex;
    volatile int64 g_LastGameCallMs = 0;
    std::vector<ENetEvent> g_DeferredEvents;      // received while the game loop was paused
    bool g_BackgroundStarted = false;

    // ---- status ----
    enum ConnKind { ConnNone = 0, ConnLan = 1, ConnDirect = 2, ConnRelay = 3 };
    volatile int g_ConnKind = ConnNone;           // ours (joiner) or the worst of our members' (host)

    bool IsVirtual(const ENetAddress& a) { return a.host != 0 && (ntohl(a.host) >> 24) == 0; }
    bool IsPrivate(const ENetAddress& a)
    {
        uint32 ip = ntohl(a.host);
        return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE;
    }
    int KindOf(const ENetAddress& a) { return IsVirtual(a) ? ConnRelay : IsPrivate(a) ? ConnLan : ConnDirect; }

    // ---- relay (okplay rsend / rpoll) ----
    namespace relay
    {
        std::mutex m;
        std::condition_variable cv;
        bool on = false;
        std::string room, me, tok;                                  // host's room (hex8), our peer id, our mailbox key
        int64 armedUntil = 0;                                       // host: poll this long after a knock
        int peers = 0;                                              // ENet peers connected through the relay
        std::deque<std::pair<std::string, std::string>> out;        // (to peer, base64url)
        std::deque<std::pair<uint32, std::vector<uint8>>> in;       // (stand-in address, datagram)
        std::map<std::string, uint32> virtOf;
        std::map<uint32, std::string> peerOf;
        uint32 nextVirt = 1;
        bool threads = false;
        int64 lastOk = 0;

        uint32 VirtualFor(const std::string& peer)   // under m
        {
            auto it = virtOf.find(peer);
            if (it != virtOf.end()) return it->second;
            uint32 v = htonl(nextVirt++ & 0xffffff);
            virtOf[peer] = v;
            peerOf[v] = peer;
            return v;
        }
    }

    std::string B64Url(const uint8* p, size_t n)
    {
        static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string o;
        o.reserve((n + 2) / 3 * 4);
        for (size_t i = 0; i < n; i += 3)
        {
            uint32 v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
            o += t[v >> 18 & 63]; o += t[v >> 12 & 63];
            if (i + 1 < n) o += t[v >> 6 & 63];
            if (i + 2 < n) o += t[v & 63];
        }
        return o;
    }

    std::vector<uint8> B64UrlDec(const std::string& s)
    {
        std::vector<uint8> o;
        uint32 v = 0; int bits = 0;
        for (char c : s)
        {
            int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
                  : c == '-' || c == '+' ? 62 : c == '_' || c == '/' ? 63 : -1;
            if (d < 0) continue;
            v = v << 6 | d; bits += 6;
            if (bits >= 8) { bits -= 8; o.push_back(static_cast<uint8>(v >> bits)); }
        }
        return o;
    }

    extern "C" int RelaySendHook(const ENetAddress* a, const ENetBuffer* bufs, size_t n)
    {
        size_t total = 0;
        std::vector<uint8> d;
        for (size_t i = 0; i < n; i++)
        {
            const uint8* p = static_cast<const uint8*>(bufs[i].data);
            d.insert(d.end(), p, p + bufs[i].dataLength);
            total += bufs[i].dataLength;
        }
        std::lock_guard<std::mutex> l(relay::m);
        auto it = relay::peerOf.find(a->host);
        if (!relay::on || it == relay::peerOf.end()) return static_cast<int>(total);   // nowhere to go: dropped, like UDP
        relay::out.emplace_back(it->second, B64Url(d.data(), d.size()));
        relay::cv.notify_all();
        return static_cast<int>(total);
    }

    extern "C" int RelayReceiveHook(ENetAddress* a, ENetBuffer* bufs, size_t n)
    {
        std::lock_guard<std::mutex> l(relay::m);
        if (relay::in.empty() || n == 0) return 0;
        auto& front = relay::in.front();
        size_t len = std::min(front.second.size(), bufs[0].dataLength);
        memcpy(bufs[0].data, front.second.data(), len);
        a->host = front.first;
        a->port = 1;
        relay::in.pop_front();
        return static_cast<int>(len);
    }

    std::string RelayBase(const char* action)
    {
        return OkpBase(action) + "&room=" + relay::room + "&peer=" + relay::me + "&ptok=" + relay::tok;
    }

    void RelaySender()
    {
        while (true)
        {
            std::string body;
            {
                std::unique_lock<std::mutex> l(relay::m);
                relay::cv.wait(l, [] { return relay::on && !relay::out.empty(); });
                std::string f;
                for (int k = 0; k < 64 && !relay::out.empty(); k++)
                {
                    if (!f.empty()) f += '*';
                    f += relay::out.front().first + "." + relay::out.front().second;
                    relay::out.pop_front();
                }
                body = RelayBase("rsend") + "&f=" + f;
            }
            std::string r = g_HttpPost ? g_HttpPost(g_OkpUrl, body) : "0\n";
            if (r.compare(0, 4, "200\n") != 0)
                __android_log_print(ANDROID_LOG_WARN, "yoyo", "PFO LAN: relay send failed: %s", r.substr(0, 120).c_str());
        }
    }

    void RelayPoller()
    {
        while (true)
        {
            bool go;
            std::string body;
            {
                std::lock_guard<std::mutex> l(relay::m);
                go = relay::on && (relay::peers > 0 || NowMs() < relay::armedUntil);
                if (go) body = RelayBase("rpoll") + "&wait=1500&c=1";
            }
            if (!go) { std::this_thread::sleep_for(std::chrono::milliseconds(150)); continue; }
            std::string r = g_HttpPost ? g_HttpPost(g_OkpUrl, body) : "0\n";
            if (r.compare(0, 4, "200\n") != 0)
            {
                __android_log_print(ANDROID_LOG_WARN, "yoyo", "PFO LAN: relay poll failed: %s", r.substr(0, 120).c_str());
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
                continue;
            }
            size_t at = r.find("\"f\":\"");
            if (at == std::string::npos) continue;
            at += 5;
            size_t end = r.find('"', at);
            std::string f = r.substr(at, end == std::string::npos ? std::string::npos : end - at);
            std::lock_guard<std::mutex> l(relay::m);
            relay::lastOk = NowMs();
            size_t s = 0;
            while (s < f.size())
            {
                size_t e = f.find('*', s);
                std::string one = f.substr(s, e == std::string::npos ? std::string::npos : e - s);
                size_t dot = one.find('.');
                if (dot == 16)
                {
                    uint32 v = relay::VirtualFor(one.substr(0, 16));
                    relay::in.emplace_back(v, B64UrlDec(one.substr(17)));
                }
                if (e == std::string::npos) break;
                s = e + 1;
            }
        }
    }

    // Use the relay for this room (the host's room id); armMs: how long a host keeps polling for
    // joiners after a knock (0 = leave as is).
    void RelayStart(uint32 room, int64 armMs)
    {
        std::lock_guard<std::mutex> l(relay::m);
        std::string rh = Hex(room, 8);
        if (relay::room != rh) { relay::out.clear(); relay::in.clear(); }
        relay::room = rh;
        relay::me = Hex(g_MyId, 16);
        if (relay::tok.empty())
        {
            std::random_device rd;
            relay::tok = Hex((static_cast<uint64>(rd()) << 32) ^ rd(), 16) + Hex((static_cast<uint64>(rd()) << 32) ^ rd(), 16);
        }
        relay::on = true;
        if (armMs > 0) relay::armedUntil = std::max(relay::armedUntil, NowMs() + armMs);
        if (!relay::threads)
        {
            relay::threads = true;
            enet_virtual_send_hook = &RelaySendHook;
            enet_virtual_receive_hook = &RelayReceiveHook;
            std::thread(RelaySender).detach();
            std::thread(RelayPoller).detach();
        }
        relay::cv.notify_all();
    }

    void RelayStop()
    {
        std::lock_guard<std::mutex> l(relay::m);
        relay::on = false;
        relay::armedUntil = 0;
        relay::peers = 0;
        relay::out.clear();
        relay::in.clear();
        relay::virtOf.clear();
        relay::peerOf.clear();
    }

    // joiner: a stand-in address for the host, to ENet-connect to through the relay
    ENetAddress RelayAddressFor(uint64 hostPeer)
    {
        std::lock_guard<std::mutex> l(relay::m);
        ENetAddress a;
        a.host = relay::VirtualFor(Hex(hostPeer, 16));
        a.port = 1;
        return a;
    }

    // ---- UPnP (okplay/src/upnp.c, ported to POSIX sockets) ----
    namespace upnp
    {
        volatile bool enabled = true;        // the overlay's UPNP switch
        std::mutex m;
        bool mapped = false, busy = false;
        ENetAddress publicCand = {};         // the router's public address + our forwarded port
        int extPort = 0;
        std::string ctlHost, control, service, lanIp;
        uint16 ctlPort = 0;

        const char* istrstr(const char* hay, const char* needle)
        {
            size_t n = strlen(needle);
            for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return hay;
            return nullptr;
        }

        bool xml_value(const char* from, const char* tag, std::string& out)
        {
            std::string open = std::string("<") + tag + ">";
            const char* a = istrstr(from, open.c_str());
            if (!a) { open = std::string(":") + tag + ">"; a = istrstr(from, open.c_str()); if (!a) return false; }
            a += open.size();
            const char* b = istrstr(a, "</");
            if (!b) return false;
            out.assign(a, b - a);
            return true;
        }

        bool split_url(const std::string& url, std::string& host, uint16& port, std::string& path)
        {
            if (strncasecmp(url.c_str(), "http://", 7)) return false;
            std::string r = url.substr(7);
            size_t slash = r.find('/');
            host = r.substr(0, slash);
            path = slash == std::string::npos ? "/" : r.substr(slash);
            port = 80;
            size_t c = host.find(':');
            if (c != std::string::npos) { port = static_cast<uint16>(atoi(host.c_str() + c + 1)); host = host.substr(0, c); }
            return !host.empty();
        }

        bool wait_sock(int s, bool write, int ms)
        {
            fd_set f; FD_ZERO(&f); FD_SET(s, &f);
            timeval tv = { ms / 1000, (ms % 1000) * 1000 };
            return select(s + 1, write ? nullptr : &f, write ? &f : nullptr, nullptr, &tv) > 0;
        }

        // one HTTP request; the body ("" on failure); local: our address towards the router
        std::string http(const std::string& host, uint16 port, const std::string& req, std::string* local)
        {
            sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons(port);
            if (inet_aton(host.c_str(), &a.sin_addr) == 0) return "";
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) return "";
            fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
            connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
            if (!wait_sock(s, true, 3000)) { close(s); return ""; }
            fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
            if (local)
            {
                sockaddr_in me = {}; socklen_t ml = sizeof(me);
                if (!getsockname(s, reinterpret_cast<sockaddr*>(&me), &ml)) *local = inet_ntoa(me.sin_addr);
            }
            if (send(s, req.data(), req.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(req.size())) { close(s); return ""; }
            std::string buf;
            char tmp[4096];
            while (wait_sock(s, false, 3000))
            {
                ssize_t got = recv(s, tmp, sizeof(tmp), 0);
                if (got <= 0) break;
                buf.append(tmp, got);
                if (buf.size() > (1 << 20)) break;
            }
            close(s);
            if (buf.compare(0, 5, "HTTP/")) return "";
            int status = 0;
            sscanf(buf.c_str(), "HTTP/%*s %d", &status);
            size_t body = buf.find("\r\n\r\n");
            if (body == std::string::npos || (status != 200 && status != 500)) return "";
            return buf.substr(body + 4);
        }

        bool fetch_description(const std::string& location)
        {
            std::string host, path; uint16 port;
            if (!split_url(location, host, port, path)) return false;
            std::string xml = http(host, port, "GET " + path + " HTTP/1.0\r\nHost: " + host + ":" + std::to_string(port) + "\r\nConnection: close\r\n\r\n", &lanIp);
            if (xml.empty()) return false;
            for (const char* kind : { "WANIPConnection:", "WANPPPConnection:" })
            {
                const char* p = xml.c_str();
                while ((p = istrstr(p, "<serviceType>")) != nullptr)
                {
                    std::string type, ctl;
                    if (xml_value(p, "serviceType", type) && istrstr(type.c_str(), kind) && xml_value(p, "controlURL", ctl))
                    {
                        service = type;
                        if (!strncasecmp(ctl.c_str(), "http://", 7))
                        {
                            std::string h2, p2; uint16 port2;
                            if (!split_url(ctl, h2, port2, p2)) return false;
                            ctlHost = h2; ctlPort = port2; control = p2;
                        }
                        else
                        {
                            ctlHost = host; ctlPort = port;
                            control = ctl[0] == '/' ? ctl : "/" + ctl;
                        }
                        return true;
                    }
                    p += 13;
                }
            }
            return false;
        }

        bool discover(int timeoutMs)
        {
            int s = socket(AF_INET, SOCK_DGRAM, 0);
            if (s < 0) return false;
            sockaddr_in to = {}; to.sin_family = AF_INET; to.sin_port = htons(1900);
            inet_aton("239.255.255.250", &to.sin_addr);
            for (const char* t : { "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                   "urn:schemas-upnp-org:service:WANIPConnection:1",
                                   "urn:schemas-upnp-org:service:WANPPPConnection:1" })
            {
                std::string msg = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + t + "\r\n\r\n";
                sendto(s, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
            }
            int64 end = NowMs() + timeoutMs;
            bool found = false;
            while (!found && NowMs() < end)
            {
                if (!wait_sock(s, false, 200)) continue;
                char buf[1500];
                ssize_t got = recv(s, buf, sizeof(buf) - 1, 0);
                if (got <= 0) continue;
                buf[got] = 0;
                const char* loc = istrstr(buf, "\nLOCATION:");
                if (!loc) continue;
                loc += 10;
                while (*loc == ' ') loc++;
                std::string url;
                while (*loc && *loc != '\r' && *loc != '\n' && url.size() < 300) url += *loc++;
                found = fetch_description(url);
            }
            close(s);
            return found;
        }

        std::string soap(const char* action, const std::string& args)
        {
            std::string body = "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                               "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + std::string(action) +
                               " xmlns:u=\"" + service + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
            std::string req = "POST " + control + " HTTP/1.0\r\nHost: " + ctlHost + ":" + std::to_string(ctlPort) +
                              "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nContent-Length: " + std::to_string(body.size()) +
                              "\r\nSOAPAction: \"" + service + "#" + action + "\"\r\nConnection: close\r\n\r\n" + body;
            return http(ctlHost, ctlPort, req, nullptr);
        }

        bool add_mapping(int ext, int internal)
        {
            std::string r = soap("AddPortMapping", "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(ext) +
                "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(internal) +
                "</NewInternalPort><NewInternalClient>" + lanIp + "</NewInternalClient><NewEnabled>1</NewEnabled>"
                "<NewPortMappingDescription>UFO 50 online</NewPortMappingDescription><NewLeaseDuration>3600</NewLeaseDuration>");
            return !r.empty() && !istrstr(r.c_str(), "Fault>");
        }

        void delete_mapping(int ext)
        {
            soap("DeletePortMapping", "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(ext) +
                 "</NewExternalPort><NewProtocol>UDP</NewProtocol>");
        }
    }

    // host: ask the router to forward our game port (worker thread; quick no-op if no UPnP router)
    void UpnpMapAsync()
    {
        if (!upnp::enabled) return;
        {
            std::lock_guard<std::mutex> l(upnp::m);
            if (upnp::busy || upnp::mapped) return;
            upnp::busy = true;
        }
        uint16 port = g_Port;
        std::thread([port] {
            bool ok = false;
            std::string ext;
            int extPort = 0;
            if (upnp::discover(2500))
            {
                std::string r = upnp::soap("GetExternalIPAddress", "");
                upnp::xml_value(r.c_str(), "NewExternalIPAddress", ext);
                for (int k = 0; k < 4 && !ok; k++) { extPort = port + k; ok = upnp::add_mapping(extPort, port); }   // the port may be taken by another device
            }
            std::lock_guard<std::mutex> l(upnp::m);
            upnp::busy = false;
            in_addr ia;
            if (ok && inet_aton(ext.c_str(), &ia))
            {
                upnp::mapped = true;
                upnp::extPort = extPort;
                upnp::publicCand.host = ia.s_addr;
                upnp::publicCand.port = static_cast<uint16>(extPort);
                __android_log_print(ANDROID_LOG_INFO, "yoyo", "PFO LAN: UPnP: router forwards %s:%d to us", ext.c_str(), extPort);
            }
            else __android_log_print(ANDROID_LOG_INFO, "yoyo", "PFO LAN: UPnP: no mapping (%s)", upnp::ctlHost.empty() ? "no UPnP router found" : "router refused");
        }).detach();
    }

    void UpnpUnmapAsync()
    {
        int ext;
        {
            std::lock_guard<std::mutex> l(upnp::m);
            if (!upnp::mapped) return;
            upnp::mapped = false;
            ext = upnp::extPort;
            upnp::publicCand = {};
        }
        std::thread([ext] { upnp::delete_mapping(ext); }).detach();
    }

    bool UpnpCandidate(ENetAddress& out)
    {
        std::lock_guard<std::mutex> l(upnp::m);
        if (!upnp::mapped) return false;
        out = upnp::publicCand;
        return true;
    }

    // ---- background pump ----
    void BackgroundPump()
    {
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!g_Host || NowMs() - g_LastGameCallMs < 300) continue;   // the game loop is running: it pumps
            std::unique_lock<std::recursive_mutex> l(g_NetMutex, std::try_to_lock);
            if (!l.owns_lock()) continue;
            ENetEvent ev;
            while (g_DeferredEvents.size() < 4096 && enet_host_service(g_Host, &ev, 0) > 0) g_DeferredEvents.push_back(ev);
            PumpOkplay();      // keep our room announced / knocks answered
            PumpRendezvous();
        }
    }

    void StartBackgroundPump()
    {
        if (g_BackgroundStarted) return;
        g_BackgroundStarted = true;
        std::thread(BackgroundPump).detach();
    }

    // game thread, end of PumpNetwork: how we're connected, for the overlay
    void UpdateConnStatus(ENetPeer* ownerPeer)
    {
        int kind = ConnNone, relayed = 0;
        if (g_Host)
            for (size_t i = 0; i < g_Host->peerCount; i++)
            {
                ENetPeer& p = g_Host->peers[i];
                if (p.state != ENET_PEER_STATE_CONNECTED) continue;
                int k = KindOf(p.address);
                if (k == ConnRelay) relayed++;
                if (ownerPeer ? &p == ownerPeer : true) kind = std::max(kind, k);
            }
        g_ConnKind = (g_InLobby || !g_Conns.empty()) ? kind : ConnNone;   // lobby, or a match after it
        std::lock_guard<std::mutex> l(relay::m);
        relay::peers = relayed;
    }
