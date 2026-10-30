// Implementation of the fake Steam API (see android/steam/steam_api.h).
// Included at the end of dllmain.cpp on Android so it can use PFO's helpers
// (init_real, init_int64, YYCreateString, CreateDsMap, logging...).
//
// Transport: one ENet host per device (UDP, game port 50650+).
//   channel 0: control (lobby + connection handshakes), reliable
//   channel 1: PFO reliable messages
//   channel 2: PFO unreliable messages (unsequenced; PFO orders them itself)
// Lobby: the lobby owner keeps the authoritative state and pushes full
// snapshots to members. Discovery: UDP broadcast queries on port 50649.

#include <enet/enet.h>
extern "C" int (*enet_virtual_send_hook)(const ENetAddress*, const ENetBuffer*, size_t);
extern "C" int (*enet_virtual_receive_hook)(ENetAddress*, ENetBuffer*, size_t);
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <netdb.h>
#include <sstream>
#include <thread>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <vector>

#define PFO_PACKAGE "com.unofficial.ufo50"

namespace fakesteam
{
    constexpr uint16 c_GamePortBase = 50650;
    constexpr int c_GamePortTries = 8;
    constexpr uint16 c_DiscoveryPort = 50649;
    constexpr int c_Channels = 3;
    constexpr uint8 c_ChanControl = 0, c_ChanReliable = 1, c_ChanUnreliable = 2;
    constexpr uint32 c_Magic = 0x55463530; // "UF50"
    constexpr uint16 c_ProtocolVersion = 1;
    constexpr uint32 c_DisconnectGraceful = 1;
    constexpr int64 c_ListSearchMs = 1200;
    int64 g_ListStartMs = 0;
    constexpr int64 c_JoinTimeoutMs = 6000;

    enum class Msg : uint8
    {
        HelloLobby = 1,   // member -> owner: lobbyId, myId, name
        JoinResult,       // owner -> member: ok
        LobbyState,       // owner -> members: full snapshot
        SetMemberData,    // member -> owner: key, value
        LobbyClosed,      // owner -> members
        HelloP2P,         // connector -> listener: myId, targetId
        P2PAccepted,      // listener -> connector
        Data,             // PFO payload: messageNumber, bytes
        DiscoverQuery,    // UDP broadcast
        DiscoverReply,    // UDP unicast
        PunchRequest,     // member -> owner: targetId (please make target punch toward me)
        PunchRelay,       // owner -> member: fromId, ip, port (punch toward them)
    };

    int64 NowMs()
    {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<int64>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    }

    // ---- tiny binary (de)serializer ----
    struct Out
    {
        std::vector<uint8> b;
        void u8(uint8 v) { b.push_back(v); }
        void u16(uint16 v) { u8(v & 0xff); u8(v >> 8); }
        void u32(uint32 v) { u16(v & 0xffff); u16(v >> 16); }
        void u64(uint64 v) { u32(static_cast<uint32>(v)); u32(static_cast<uint32>(v >> 32)); }
        void str(const std::string& s) { u16(static_cast<uint16>(std::min<size_t>(s.size(), 0xffff))); b.insert(b.end(), s.begin(), s.begin() + std::min<size_t>(s.size(), 0xffff)); }
        void kv(const std::map<std::string, std::string>& m) { u16(static_cast<uint16>(m.size())); for (auto& [k, v] : m) { str(k); str(v); } }
    };

    struct In
    {
        const uint8* p; size_t n; size_t o = 0; bool ok = true;
        In(const void* data, size_t size) : p(static_cast<const uint8*>(data)), n(size) {}
        bool need(size_t k) { if (o + k > n) { ok = false; } return ok; }
        uint8 u8() { return need(1) ? p[o++] : 0; }
        uint16 u16() { uint16 lo = u8(); return static_cast<uint16>(lo | (u8() << 8)); }
        uint32 u32() { uint32 lo = u16(); return lo | (static_cast<uint32>(u16()) << 16); }
        uint64 u64() { uint64 lo = u32(); return lo | (static_cast<uint64>(u32()) << 32); }
        std::string str() { uint16 len = u16(); if (!need(len)) return {}; std::string s(reinterpret_cast<const char*>(p + o), len); o += len; return s; }
        std::map<std::string, std::string> kv() { std::map<std::string, std::string> m; uint16 c = u16(); for (int i = 0; i < c && ok; i++) { auto k = str(); m[k] = str(); } return m; }
    };

    // ---- state ----
    struct Conn
    {
        ENetPeer* peer = nullptr;
        uint64 remoteId = 0;
        bool incoming = false;
        bool p2pHelloSent = false;
        ESteamNetworkingConnectionState state = k_ESteamNetworkingConnectionState_None;
        int64 nextMessageNumber = 1;
        std::deque<SteamNetworkingMessage_t*> inbox;
    };

    struct Member
    {
        uint64 id = 0;
        std::string name;
        uint32 ip = 0;     // network byte order, as seen by the owner (0 = owner itself)
        uint16 port = 0;
        std::map<std::string, std::string> data;
    };

    struct Lobby
    {
        uint64 id = 0;
        uint64 owner = 0;
        bool joinable = true;
        int limit = 8;
        std::map<std::string, std::string> data;
        std::vector<Member> members;

        Member* Find(uint64 memberId) { for (auto& m : members) if (m.id == memberId) return &m; return nullptr; }
    };

    struct LobbyInfo
    {
        uint64 id = 0, owner = 0;
        uint32 ip = 0; uint16 port = 0;
        int members = 0, limit = 0;
        bool joinable = false;
        std::string ownerName;
        std::map<std::string, std::string> data;
        uint32 room = 0;                  // okplay room id
        bool okplay = false;
        std::vector<ENetAddress> cands;   // every address the host might be reachable at
    };

    struct PendingCallback
    {
        ECallback id;
        SteamNetConnectionStatusChangedCallback_t status;
        LobbyDataUpdate_t lobbyData;
    };

    bool g_Initialised = false;
    ENetHost* g_Host = nullptr;
    uint16 g_Port = 0;
    int g_DiscoverySocket = -1;
    uint64 g_MyId = 0;
    std::string g_MyName;

    // Player name chosen in the touch overlay (players panel > NAME). Handed over from the UI
    // thread and applied on the game thread (ApplyPendingName, start of Pump) while not in a
    // lobby, so the network code never sees it change mid-use. Empty = the device's default.
    std::mutex g_NameMutex;
    bool g_NamePending = false;
    std::string g_PendingName, g_NameForUi;
    std::string DeviceName();

    // Private lobbies ("CREATE PRIVATE LOBBY"): not shown in anyone's lobby list unless they've
    // entered the lobby's invite code (overlay: players panel > JOIN CODE). The lobby carries a hash
    // of its code in its data ("pfo_private"), which every way of finding lobbies (LAN, rendezvous,
    // okplay) already passes along; the list keeps only public lobbies and ones whose hash matches
    // a code we were given. INVITE FRIENDS shows the code (overlay).
    std::string g_LobbyCode;                 // our private lobby's code ("" = public / not hosting)
    std::vector<std::string> g_JoinTags;     // hashes of codes we were given (newest last)
    std::vector<std::string> g_JoinCodes;    // the codes themselves, same order
    std::string g_ShareCode;                 // code to show for SHARE CODE (host's, or the one we joined with)
    volatile bool g_AskCodeRequested = false; // JOIN BY CODE pressed in the lobby list
    volatile bool g_InviteRequested = false; // INVITE FRIENDS pressed; the overlay shows the code

    std::string CodeTag(const std::string& code)
    {
        uint64 h = 1469598103934665603ull;
        for (char c : std::string("UFO50-PRIVATE:") + code) { h ^= static_cast<uint8>(c); h *= 1099511628211ull; }
        char buf[17];
        snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
        return buf;
    }

    std::string NewLobbyCode()
    {
        static const std::string abc = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   // no 0/O, 1/I
        std::random_device rd;
        std::string c;
        for (int i = 0; i < 6; i++) c += abc[rd() % abc.size()];
        return c;
    }

    // codes are typed exactly as shown; just uppercase and drop anything that isn't a letter/digit
    std::string NormalizeCode(const std::string& in)
    {
        std::string out;
        for (char ch : in)
        {
            char c = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
            if (isalnum(static_cast<unsigned char>(c))) out += c;
        }
        return out;
    }

    template <class LobbyT> bool VisibleToMe(const LobbyT& li)
    {
        auto it = li.data.find("pfo_private");
        if (it == li.data.end() || it->second.empty()) return true;
        std::lock_guard<std::mutex> lock(g_NameMutex);
        for (auto& t : g_JoinTags) if (t == it->second) return true;
        return false;
    }

    void SetMyName(const std::string& n)
    {
        g_MyName = n;
        std::lock_guard<std::mutex> lock(g_NameMutex);
        g_NameForUi = n;
    }

    void ApplyPendingName(bool inLobby)
    {
        if (inLobby) return;
        std::string n;
        {
            std::lock_guard<std::mutex> lock(g_NameMutex);
            if (!g_NamePending) return;
            g_NamePending = false;
            n = g_PendingName;
        }
        SetMyName(n.empty() ? DeviceName() : n);
        DebugConsoleOutput(("PFO LAN: player name is now " + g_MyName + "\n").c_str());
    }
    uint32 g_TimeoutConnectedMs = 60000;

    std::map<HSteamNetConnection, Conn> g_Conns;
    HSteamNetConnection g_NextConn = 1;
    bool g_Listening = false;
    constexpr HSteamListenSocket c_ListenHandle = 1;
    std::map<uint64, ENetAddress> g_AddressBook;
    std::map<uint64, std::string> g_Names;

    std::map<ECallback, std::vector<std::function<void(void*)>>> g_Callbacks;
    std::deque<PendingCallback> g_PendingCallbacks;
    std::deque<std::function<void()>> g_GmlEvents;

    Lobby g_Lobby;
    bool g_InLobby = false;
    bool g_IsOwner = false;
    ENetPeer* g_OwnerPeer = nullptr;              // member side
    std::map<ENetPeer*, uint64> g_MemberPeers;    // owner side
    bool g_JoinPending = false;
    int64 g_JoinStartMs = 0;
    bool g_ForceRelay = false;          // relay fallback: when the join started, whom we join, whether we tried it
    uint64 g_JoinHostPeer = 0;
    bool g_JoinRelayTried = false;
    uint64 g_JoinLobbyId = 0;
    int64 g_JoinDeadline = 0;

    std::vector<LobbyInfo> g_LobbyList;        // last completed search (what GML reads)
    std::vector<LobbyInfo> g_PendingLobbyList; // search in progress
    std::map<uint64, LobbyInfo> g_LobbyCache;
    bool g_ListLoading = false;
    int64 g_ListDeadline = 0;

    // Tagging ENet peers: peer->data holds one of these.
    enum class PeerKind : uintptr_t { Unknown = 0, LobbyToOwner = 1, LobbyMember = 2, JoinCandidate = 3, P2PBase = 0x10000 };
    std::vector<ENetPeer*> g_JoinCandidates; // one ENet connect per host candidate while joining
    void* P2PTag(HSteamNetConnection h) { return reinterpret_cast<void*>(static_cast<uintptr_t>(PeerKind::P2PBase) + h); }
    HSteamNetConnection P2PFromTag(void* tag) { auto v = reinterpret_cast<uintptr_t>(tag); return v >= static_cast<uintptr_t>(PeerKind::P2PBase) ? static_cast<HSteamNetConnection>(v - static_cast<uintptr_t>(PeerKind::P2PBase)) : 0; }

    void RegisterCallback(ECallback id, std::function<void(void*)> fn) { g_Callbacks[id].push_back(std::move(fn)); }

    // Rendezvous server (lobby list + UDP hole punching), see "rendezvous server" section below.
    int ENET_CALLBACK RendezvousIntercept(ENetHost* host, ENetEvent* event);
    void LoadRendezvousConfig();
    void RendezvousPunch(uint64 targetId);
    void RendezvousRequestList();
    void PumpRendezvous();
    bool StunHandle(const uint8* b, size_t n);
    void LoadOkplayConfig();
    void OkpRequestList();
    void OkpClose();
    void PumpOkplay();
    bool OkpListInFlight();                          // an okplay list request hasn't answered yet
    void RelayStart(uint32 room, int64 armMs);       // netplus.inc
    void StartBackgroundPump();
    void RelayStop();
    void UpnpUnmapAsync();
    bool UpnpCandidate(ENetAddress& out);
    void OkpAddRecent(std::vector<LobbyInfo>& list); // okplay rooms seen in the last few seconds
    extern bool g_OkpEnabled;
    extern uint32 g_OkpJoinRoom;
    extern int64 g_OkpNextKnock;

    void QueueStatus(HSteamNetConnection h, Conn& c, ESteamNetworkingConnectionState oldState, int endReason, const char* debug)
    {
        PendingCallback p = {};
        p.id = ECallback::NetConnectionStatusChanged;
        p.status.m_hConn = h;
        p.status.m_eOldState = oldState;
        p.status.m_info.m_identityRemote.SetSteamID64(c.remoteId);
        p.status.m_info.m_hListenSocket = c.incoming ? c_ListenHandle : k_HSteamListenSocket_Invalid;
        p.status.m_info.m_eState = c.state;
        p.status.m_info.m_eEndReason = endReason;
        snprintf(p.status.m_info.m_szEndDebug, sizeof(p.status.m_info.m_szEndDebug), "%s", debug ? debug : "");
        g_PendingCallbacks.push_back(p);
    }

    void Send(ENetPeer* peer, uint8 channel, const Out& out, bool reliable)
    {
        if (!peer) return;
        ENetPacket* pkt = enet_packet_create(out.b.data(), out.b.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);
        if (enet_peer_send(peer, channel, pkt) != 0)
        {
            enet_packet_destroy(pkt);
        }
    }

    std::string DeviceName()
    {
        char model[PROP_VALUE_MAX] = {};
        __system_property_get("ro.product.model", model);
        char buf[96];
        snprintf(buf, sizeof(buf), "%s %04X", model[0] ? model : "ANDROID", static_cast<unsigned>(g_MyId & 0xffff));
        return buf;
    }

    void ChdirToAppFiles()
    {
        // PFO writes logs/replays with relative paths; point them at the app's files dir.
        char cmdline[256] = {};
        FILE* f = fopen("/proc/self/cmdline", "rb");
        if (!f) return;
        size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, f);
        fclose(f);
        cmdline[n] = 0;
        char* colon = strchr(cmdline, ':');
        if (colon) *colon = 0;
        std::string dir = std::string("/data/data/") + cmdline + "/files";
        if (chdir(dir.c_str()) != 0)
        {
            dir = std::string("/data/user/0/") + cmdline + "/files";
            chdir(dir.c_str());
        }
    }

    bool Init()
    {
        if (g_Initialised) return g_Host != nullptr;
        g_Initialised = true;

        ChdirToAppFiles();

        std::random_device rd;
        std::mt19937_64 rng((static_cast<uint64>(rd()) << 32) ^ rd() ^ static_cast<uint64>(NowMs()));
        g_MyId = (rng() & 0x1fffffffffffffffull) | 1;  // user ids < 2^61 (positive in GML)
        SetMyName(DeviceName());
        g_Names[g_MyId] = g_MyName;

        if (enet_initialize() != 0)
        {
            DebugConsoleOutput("PFO LAN: enet_initialize failed\n");
            return false;
        }

        for (int i = 0; i < c_GamePortTries && !g_Host; i++)
        {
            ENetAddress addr = {};
            addr.host = ENET_HOST_ANY;
            addr.port = static_cast<uint16>(c_GamePortBase + i);
            g_Host = enet_host_create(&addr, 32, c_Channels, 0, 0);
            if (g_Host) g_Port = addr.port;
        }
        if (!g_Host)
        {
            g_Host = enet_host_create(nullptr, 32, c_Channels, 0, 0);
            if (g_Host) g_Port = g_Host->address.port;
        }
        if (!g_Host)
        {
            DebugConsoleOutput("PFO LAN: could not create ENet host\n");
            return false;
        }
        g_Host->intercept = RendezvousIntercept;
        LoadRendezvousConfig();
        LoadOkplayConfig();

        g_DiscoverySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_DiscoverySocket >= 0)
        {
            int one = 1;
            setsockopt(g_DiscoverySocket, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
            setsockopt(g_DiscoverySocket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            fcntl(g_DiscoverySocket, F_SETFL, fcntl(g_DiscoverySocket, F_GETFL, 0) | O_NONBLOCK);
            sockaddr_in sa = {};
            sa.sin_family = AF_INET;
            sa.sin_addr.s_addr = htonl(INADDR_ANY);
            sa.sin_port = htons(c_DiscoveryPort);
            if (bind(g_DiscoverySocket, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0)
            {
                // Port taken (another instance on this device): bind anywhere so we can still search.
                sa.sin_port = 0;
                bind(g_DiscoverySocket, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
            }
        }

        char msg[256];
        snprintf(msg, sizeof(msg), "PFO LAN: id=%llu name=%s port=%u\n", static_cast<unsigned long long>(g_MyId), g_MyName.c_str(), g_Port);
        DebugConsoleOutput(msg);
        StartBackgroundPump();   // keeps ENet answering while the game loop is paused (netplus.inc)
        return true;
    }

    // ---- GML async events (event 69 = Async Steam, same as the Steamworks extension) ----
    void QueueGmlEvent(std::function<void(int map)> fill)
    {
        g_GmlEvents.push_back([fill]() {
            int map = CreateDsMap(0, 0);
            fill(map);
            CreateAsyncEventWithDSMap(map, 69);
        });
    }

    void GmlLobbyDataUpdate(uint64 lobbyId, uint64 memberId, bool success)
    {
        QueueGmlEvent([=](int map) {
            DsMapAddString(map, "event_type", "lobby_data_update");
            DsMapAddInt64(map, "lobby_id", static_cast<int64>(lobbyId));
            DsMapAddInt64(map, "member_id", static_cast<int64>(memberId));
            DsMapAddDouble(map, "success", success ? 1 : 0);
        });
    }

    void GmlLobbyChatUpdate(uint64 lobbyId, uint64 userId, int flags)
    {
        QueueGmlEvent([=](int map) {
            DsMapAddString(map, "event_type", "lobby_chat_update");
            DsMapAddInt64(map, "lobby_id", static_cast<int64>(lobbyId));
            DsMapAddInt64(map, "user_id", static_cast<int64>(userId));
            DsMapAddInt64(map, "change_id", static_cast<int64>(userId));
            DsMapAddDouble(map, "change_flags", flags);
        });
    }

    void GmlLobbyResult(const char* type, uint64 lobbyId, bool success)
    {
        std::string t = type;
        QueueGmlEvent([=](int map) {
            DsMapAddString(map, "event_type", t.c_str());
            DsMapAddInt64(map, "lobby_id", static_cast<int64>(lobbyId));
            DsMapAddDouble(map, "success", success ? 1 : 0);
            DsMapAddDouble(map, "result", success ? 1 : 2);
        });
    }

    // Compare two snapshots and raise the events Steam would.
    void DiffLobby(const Lobby& before, const Lobby& after)
    {
        for (auto& m : after.members)
        {
            g_Names[m.id] = m.name;
            bool existed = false;
            for (auto& o : before.members) if (o.id == m.id) { existed = true; if (o.data != m.data) GmlLobbyDataUpdate(after.id, m.id, true); }
            if (!existed) { GmlLobbyChatUpdate(after.id, m.id, 1); if (!m.data.empty()) GmlLobbyDataUpdate(after.id, m.id, true); }
        }
        for (auto& o : before.members)
        {
            bool still = false;
            for (auto& m : after.members) if (m.id == o.id) still = true;
            if (!still) GmlLobbyChatUpdate(after.id, o.id, 2);
        }
        if (before.data != after.data || before.owner != after.owner || before.joinable != after.joinable)
        {
            GmlLobbyDataUpdate(after.id, after.id, true);
        }
    }

    void WriteState(Out& o)
    {
        o.u8(static_cast<uint8>(Msg::LobbyState));
        o.u64(g_Lobby.id); o.u64(g_Lobby.owner); o.u8(g_Lobby.joinable); o.u16(static_cast<uint16>(g_Lobby.limit));
        o.kv(g_Lobby.data);
        o.u16(static_cast<uint16>(g_Lobby.members.size()));
        for (auto& m : g_Lobby.members) { o.u64(m.id); o.str(m.name); o.u32(m.ip); o.u16(m.port); o.kv(m.data); }
    }

    void BroadcastState()
    {
        Out o;
        WriteState(o);
        for (auto& [peer, id] : g_MemberPeers) Send(peer, c_ChanControl, o, true);
    }

    // Owner-side mutation helper: apply, raise local events, push to members.
    template<typename F>
    void OwnerMutate(F&& f)
    {
        Lobby before = g_Lobby;
        f();
        DiffLobby(before, g_Lobby);
        BroadcastState();
    }

    void DropJoinCandidates(ENetPeer* keep)
    {
        for (ENetPeer* p : g_JoinCandidates) if (p != keep) { p->data = nullptr; enet_peer_reset(p); }
        g_JoinCandidates.clear();
    }

    void ResetLobby()
    {
        DropJoinCandidates(nullptr);
        g_OkpJoinRoom = 0;
        g_Lobby = {};
        g_InLobby = false;
        g_IsOwner = false;
        g_JoinPending = false;
        g_OwnerPeer = nullptr;
        g_MemberPeers.clear();
    }

    void LeaveLobby()
    {
        // The relay and the UPnP port stay up: the game leaves the lobby when a match starts and
        // the match itself runs over the same paths. (The relay only polls while a relayed peer
        // is connected; the UPnP mapping goes at Quit, or by itself when its lease ends.)
        {
            std::lock_guard<std::mutex> lock(g_NameMutex);
            g_LobbyCode.clear();
        }
        if (g_IsOwner)
        {
            Out o; o.u8(static_cast<uint8>(Msg::LobbyClosed));
            for (auto& [peer, id] : g_MemberPeers) { Send(peer, c_ChanControl, o, true); peer->data = nullptr; enet_peer_disconnect_later(peer, c_DisconnectGraceful); }
        }
        else if (g_OwnerPeer)
        {
            g_OwnerPeer->data = nullptr;
            enet_peer_disconnect_later(g_OwnerPeer, c_DisconnectGraceful);
        }
        if (g_Host) enet_host_flush(g_Host);
        ResetLobby();
    }

    void OnLobbyLostAsMember()
    {
        uint64 lobbyId = g_Lobby.id;
        bool wasJoining = g_JoinPending;
        ResetLobby();
        if (wasJoining) GmlLobbyResult("lobby_joined", g_JoinLobbyId, false);
        else if (lobbyId) { GmlLobbyDataUpdate(lobbyId, lobbyId, false); GmlLobbyChatUpdate(lobbyId, 0, 2); }
    }

    // ---- discovery ----
    void SendDiscoveryTo(uint32 ipNetOrder)
    {
        if (g_DiscoverySocket < 0) return;
        Out o; o.u32(c_Magic); o.u16(c_ProtocolVersion); o.u8(static_cast<uint8>(Msg::DiscoverQuery));
        sockaddr_in sa = {};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = ipNetOrder;
        sa.sin_port = htons(c_DiscoveryPort);
        sendto(g_DiscoverySocket, o.b.data(), o.b.size(), 0, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    }

    void StartLobbySearch()
    {
        g_PendingLobbyList.clear();
        g_ListLoading = true;
        g_ListStartMs = NowMs();
        g_ListDeadline = NowMs() + c_ListSearchMs;

        RendezvousRequestList();
        OkpRequestList();
        if (g_OkpEnabled) g_ListDeadline = NowMs() + 2500; // an HTTPS round trip takes longer than the LAN
        SendDiscoveryTo(htonl(INADDR_BROADCAST));
        ifaddrs* ifs = nullptr;
        if (getifaddrs(&ifs) == 0)
        {
            for (ifaddrs* i = ifs; i; i = i->ifa_next)
            {
                if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
                if ((i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr)
                    SendDiscoveryTo(reinterpret_cast<sockaddr_in*>(i->ifa_broadaddr)->sin_addr.s_addr);
            }
            freeifaddrs(ifs);
        }
        // Emulators (BlueStacks / Android emulator) reach the host PC at 10.0.2.2.
        SendDiscoveryTo(inet_addr("10.0.2.2"));
        // Extra hosts typed by the user, one IPv4 per line (files/lan_hosts.txt).
        if (FILE* f = fopen("lan_hosts.txt", "r"))
        {
            char line[128];
            while (fgets(line, sizeof(line), f))
            {
                line[strcspn(line, "\r\n \t")] = 0;
                in_addr a;
                if (line[0] && inet_aton(line, &a)) SendDiscoveryTo(a.s_addr);
            }
            fclose(f);
        }
    }

    void PumpDiscovery()
    {
        if (g_DiscoverySocket < 0) return;
        uint8 buf[2048];
        while (true)
        {
            sockaddr_in from = {};
            socklen_t fromLen = sizeof(from);
            ssize_t n = recvfrom(g_DiscoverySocket, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n <= 0) break;
            In in(buf, static_cast<size_t>(n));
            if (in.u32() != c_Magic || in.u16() != c_ProtocolVersion) continue;
            auto type = static_cast<Msg>(in.u8());
            if (type == Msg::DiscoverQuery)
            {
                if (g_IsOwner && g_Lobby.joinable && static_cast<int>(g_Lobby.members.size()) < g_Lobby.limit)
                {
                    Out o; o.u32(c_Magic); o.u16(c_ProtocolVersion); o.u8(static_cast<uint8>(Msg::DiscoverReply));
                    o.u64(g_Lobby.id); o.u64(g_Lobby.owner); o.u16(g_Port);
                    o.u16(static_cast<uint16>(g_Lobby.members.size())); o.u16(static_cast<uint16>(g_Lobby.limit));
                    o.str(g_MyName); o.kv(g_Lobby.data);
                    sendto(g_DiscoverySocket, o.b.data(), o.b.size(), 0, reinterpret_cast<sockaddr*>(&from), fromLen);
                }
            }
            else if (type == Msg::DiscoverReply)
            {
                LobbyInfo li;
                li.id = in.u64(); li.owner = in.u64(); li.port = in.u16();
                li.members = in.u16(); li.limit = in.u16();
                li.ownerName = in.str(); li.data = in.kv();
                li.ip = from.sin_addr.s_addr;
                li.joinable = true;
                if (!in.ok || li.id == 0 || li.id == g_Lobby.id) continue;
                g_LobbyCache[li.id] = li;
                g_Names[li.owner] = li.ownerName;
                bool dup = false;
                for (auto& e : g_PendingLobbyList) if (e.id == li.id) { e = li; dup = true; }
                if (!dup && g_ListLoading) g_PendingLobbyList.push_back(li);
            }
        }

        // A phone's HTTPS round trip to the room server can take seconds: wait for it (up to 6 s)
        // rather than finishing the search without the internet rooms.
        if (g_ListLoading && NowMs() >= g_ListDeadline && !(OkpListInFlight() && NowMs() < g_ListStartMs + 6000))
        {
            OkpAddRecent(g_PendingLobbyList);
            g_ListLoading = false;
            g_LobbyList.clear();             // results stay readable until the next search completes
            for (auto& li : g_PendingLobbyList) if (VisibleToMe(li)) g_LobbyList.push_back(li);
            int count = static_cast<int>(g_LobbyList.size());
            QueueGmlEvent([count](int map) {
                DsMapAddString(map, "event_type", "lobby_list");
                DsMapAddDouble(map, "lobby_count", count);
                DsMapAddDouble(map, "success", 1);
                DsMapAddDouble(map, "result", 1);
            });
        }
    }


    // ---- rendezvous server ----
    // Raw UDP on the ENet socket, so NAT mappings are shared with game traffic:
    //   REG id, name                 keepalive; server learns our public address
    //   PUBLISH lobby info           owner advertises its lobby
    //   LIST -> LIST_REPLY           lobby search
    //   PUNCH me, target             server tells target to send us a packet (opens its NAT)
    //                                and replies with the target's public address
    // A real backend (e.g. okplay) can replace tools/rendezvous_server.py with the same messages.
    constexpr uint32 c_RvMagic = 0x52354655; // "UF5R"
    enum class Rv : uint8 { Reg = 1, RegAck, Publish, Unpublish, List, ListReply, Punch, PunchFrom, Ping };
    bool g_RvEnabled = false;
    ENetAddress g_RvServer = {};
    int64 g_RvNextReg = 0, g_RvNextPublish = 0;
    uint64 g_RvPublishedLobby = 0;

    void RvSend(const ENetAddress& to, const Out& o)
    {
        if (!g_Host) return;
        ENetBuffer buf;
        buf.data = const_cast<uint8*>(o.b.data());
        buf.dataLength = o.b.size();
        enet_socket_send(g_Host->socket, &to, &buf, 1);
    }

    Out RvHeader(Rv type) { Out o; o.u32(c_RvMagic); o.u8(static_cast<uint8>(type)); return o; }

    void LoadRendezvousConfig()
    {
        // files/rendezvous.txt holds "host:port" (or "off"); the default reaches a server
        // running on the PC from an emulator such as BlueStacks.
        std::string spec = "10.0.2.2:50700";
        if (FILE* f = fopen("rendezvous.txt", "r"))
        {
            char line[256] = {};
            if (fgets(line, sizeof(line), f)) { line[strcspn(line, "\r\n \t")] = 0; if (line[0]) spec = line; }
            fclose(f);
        }
        if (spec == "off") return;
        std::string hostPart = spec;
        uint16 port = 50700;
        size_t colon = spec.rfind(':');
        if (colon != std::string::npos) { hostPart = spec.substr(0, colon); port = static_cast<uint16>(atoi(spec.c_str() + colon + 1)); }
        if (enet_address_set_host(&g_RvServer, hostPart.c_str()) != 0) return;
        g_RvServer.port = port;
        g_RvEnabled = true;
        char msg[160];
        snprintf(msg, sizeof(msg), "PFO LAN: rendezvous server %s:%u\n", hostPart.c_str(), port);
        DebugConsoleOutput(msg);
    }

    void RendezvousPunch(uint64 targetId)
    {
        if (!g_RvEnabled) return;
        Out o = RvHeader(Rv::Punch); o.u64(g_MyId); o.u64(targetId);
        RvSend(g_RvServer, o);
    }

    void RendezvousRequestList()
    {
        if (!g_RvEnabled) return;
        Out o = RvHeader(Rv::List); o.u64(g_MyId);
        RvSend(g_RvServer, o);
    }

    void PumpRendezvous()
    {
        if (!g_RvEnabled) return;
        int64 now = NowMs();
        if (now >= g_RvNextReg)
        {
            g_RvNextReg = now + 4000;
            Out o = RvHeader(Rv::Reg); o.u64(g_MyId); o.str(g_MyName);
            RvSend(g_RvServer, o);
        }
        bool advertise = g_InLobby && g_IsOwner && g_Lobby.joinable && static_cast<int>(g_Lobby.members.size()) < g_Lobby.limit;
        if (advertise && now >= g_RvNextPublish)
        {
            g_RvNextPublish = now + 2000;
            Out o = RvHeader(Rv::Publish);
            o.u64(g_Lobby.id); o.u64(g_MyId);
            o.u16(static_cast<uint16>(g_Lobby.members.size())); o.u16(static_cast<uint16>(g_Lobby.limit));
            o.str(g_MyName); o.kv(g_Lobby.data);
            RvSend(g_RvServer, o);
            g_RvPublishedLobby = g_Lobby.id;
        }
        else if (!advertise && g_RvPublishedLobby)
        {
            Out o = RvHeader(Rv::Unpublish); o.u64(g_RvPublishedLobby);
            RvSend(g_RvServer, o);
            g_RvPublishedLobby = 0;
        }
    }

    int ENET_CALLBACK RendezvousIntercept(ENetHost* host, ENetEvent*)
    {
        if (host->receivedDataLength < 5) return 0;
        if (StunHandle(host->receivedData, host->receivedDataLength)) return 1;
        In in(host->receivedData, host->receivedDataLength);
        if (in.u32() != c_RvMagic) return 0; // normal ENet traffic
        auto type = static_cast<Rv>(in.u8());
        switch (type)
        {
        case Rv::ListReply:
        {
            int n = in.u16();
            for (int i = 0; i < n && in.ok; i++)
            {
                LobbyInfo li;
                li.id = in.u64(); li.owner = in.u64(); li.ip = in.u32(); li.port = in.u16();
                li.members = in.u16(); li.limit = in.u16(); li.ownerName = in.str(); li.data = in.kv();
                li.joinable = true;
                if (!in.ok || li.id == 0 || li.id == g_Lobby.id || li.owner == g_MyId) continue;
                g_LobbyCache[li.id] = li;
                g_Names[li.owner] = li.ownerName;
                ENetAddress a; a.host = li.ip; a.port = li.port;
                g_AddressBook[li.owner] = a;
                bool dup = false;
                for (auto& e : g_PendingLobbyList) if (e.id == li.id) { e = li; dup = true; }
                if (!dup && g_ListLoading) g_PendingLobbyList.push_back(li);
            }
            break;
        }
        case Rv::PunchFrom:
        {
            // Someone wants to reach us: send a few packets their way to open our NAT.
            uint64 fromId = in.u64();
            ENetAddress a; a.host = in.u32(); a.port = in.u16();
            if (!in.ok) break;
            g_AddressBook.emplace(fromId, a);
            Out o = RvHeader(Rv::Ping); o.u64(g_MyId);
            for (int k = 0; k < 3; k++) RvSend(a, o);
            break;
        }
        case Rv::Punch: // the server's answer to our PUNCH: the target's public address
        {
            uint64 targetId = in.u64();
            ENetAddress a; a.host = in.u32(); a.port = in.u16();
            if (in.ok) g_AddressBook[targetId] = a;
            break;
        }
        default:
            break; // RegAck / Ping: nothing to do
        }
        return 1;
    }

#include "okplay_client.inc"
#include "netplus.inc"

    // ---- ENet event handling ----
    Conn* ConnFor(HSteamNetConnection h) { auto it = g_Conns.find(h); return it == g_Conns.end() ? nullptr : &it->second; }

    void OnReceive(ENetPeer* peer, ENetPacket* pkt, uint8 channel)
    {
        In in(pkt->data, pkt->dataLength);
        auto type = static_cast<Msg>(in.u8());

        if (type == Msg::Data)
        {
            HSteamNetConnection h = P2PFromTag(peer->data);
            Conn* c = ConnFor(h);
            if (!c || c->state != k_ESteamNetworkingConnectionState_Connected) return;
            int64 number = static_cast<int64>(in.u64());
            if (!in.ok) return;
            size_t size = pkt->dataLength - in.o;
            auto* m = SteamNetworkingUtils()->AllocateMessage(static_cast<int>(size));
            memcpy(m->m_pData, pkt->data + in.o, size);
            m->m_conn = h;
            m->m_identityPeer.SetSteamID64(c->remoteId);
            m->m_nMessageNumber = number;
            m->m_nFlags = channel == c_ChanReliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
            m->m_usecTimeReceived = SteamNetworkingUtils()->GetLocalTimestamp();
            c->inbox.push_back(m);
            return;
        }

        switch (type)
        {
        case Msg::HelloP2P:
        {
            uint64 fromId = in.u64();
            uint64 targetId = in.u64();
            if (!in.ok || targetId != g_MyId || !g_Listening)
            {
                enet_peer_disconnect_later(peer, 0);
                return;
            }
            HSteamNetConnection h = g_NextConn++;
            Conn& c = g_Conns[h];
            c.peer = peer;
            c.remoteId = fromId;
            c.incoming = true;
            c.state = k_ESteamNetworkingConnectionState_Connecting;
            peer->data = P2PTag(h);
            g_AddressBook[fromId] = peer->address;
            QueueStatus(h, c, k_ESteamNetworkingConnectionState_None, 0, "");
            break;
        }
        case Msg::P2PAccepted:
        {
            HSteamNetConnection h = P2PFromTag(peer->data);
            Conn* c = ConnFor(h);
            if (c && c->state == k_ESteamNetworkingConnectionState_Connecting)
            {
                c->state = k_ESteamNetworkingConnectionState_Connected;
                QueueStatus(h, *c, k_ESteamNetworkingConnectionState_Connecting, 0, "");
            }
            break;
        }
        case Msg::HelloLobby: // owner side
        {
            uint64 lobbyId = in.u64();
            uint64 memberId = in.u64();
            std::string name = in.str();
            Out reply; reply.u8(static_cast<uint8>(Msg::JoinResult));
            bool ok = in.ok && g_IsOwner && lobbyId == g_Lobby.id && g_Lobby.joinable
                && static_cast<int>(g_Lobby.members.size()) < g_Lobby.limit && memberId != 0;
            reply.u8(ok);
            Send(peer, c_ChanControl, reply, true);
            if (!ok)
            {
                enet_peer_disconnect_later(peer, 0);
                return;
            }
            peer->data = reinterpret_cast<void*>(PeerKind::LobbyMember);
            g_MemberPeers[peer] = memberId;
            g_AddressBook[memberId] = peer->address;
            OwnerMutate([&] {
                if (Member* existing = g_Lobby.Find(memberId)) { existing->name = name; return; }
                Member m;
                m.id = memberId;
                m.name = name;
                m.ip = peer->address.host;
                m.port = peer->address.port;
                g_Lobby.members.push_back(m);
            });
            break;
        }
        case Msg::JoinResult: // member side
        {
            if (!in.u8())
            {
                g_OwnerPeer = nullptr;
                peer->data = nullptr;
                OnLobbyLostAsMember();
            }
            break;
        }
        case Msg::LobbyState: // member side
        {
            Lobby next;
            next.id = in.u64(); next.owner = in.u64(); next.joinable = in.u8(); next.limit = in.u16();
            next.data = in.kv();
            int count = in.u16();
            for (int i = 0; i < count && in.ok; i++)
            {
                Member m;
                m.id = in.u64(); m.name = in.str(); m.ip = in.u32(); m.port = in.u16(); m.data = in.kv();
                next.members.push_back(m);
            }
            if (!in.ok || peer != g_OwnerPeer) return;

            for (auto& m : next.members)
            {
                g_Names[m.id] = m.name;
                if (m.id == next.owner) g_AddressBook[m.id] = peer->address;
                else if (m.ip != 0 && m.id != g_MyId) { ENetAddress a; a.host = m.ip; a.port = m.port; g_AddressBook[m.id] = a; }
            }

            if (g_JoinPending)
            {
                g_JoinPending = false;
                g_InLobby = true;
                g_Lobby = next;
                GmlLobbyResult("lobby_joined", next.id, true);
                GmlLobbyDataUpdate(next.id, next.id, true);
            }
            else
            {
                Lobby before = g_Lobby;
                g_Lobby = next;
                DiffLobby(before, next);
            }
            break;
        }
        case Msg::SetMemberData: // owner side
        {
            auto it = g_MemberPeers.find(peer);
            std::string key = in.str();
            std::string value = in.str();
            if (it == g_MemberPeers.end() || !in.ok) return;
            uint64 memberId = it->second;
            OwnerMutate([&] { if (Member* m = g_Lobby.Find(memberId)) m->data[key] = value; });
            break;
        }
        case Msg::PunchRequest: // owner side: member wants `target` to punch toward it
        {
            uint64 target = in.u64();
            auto from = g_MemberPeers.find(peer);
            if (!in.ok || from == g_MemberPeers.end() || !g_IsOwner) return;
            for (auto& [mp, mid] : g_MemberPeers)
            {
                if (mid != target) continue;
                Out o; o.u8(static_cast<uint8>(Msg::PunchRelay)); o.u64(from->second); o.u32(peer->address.host); o.u16(peer->address.port);
                Send(mp, c_ChanControl, o, true);
            }
            break;
        }
        case Msg::PunchRelay: // member side: punch toward another member
        {
            uint64 fromId = in.u64();
            ENetAddress a; a.host = in.u32(); a.port = in.u16();
            if (!in.ok || peer != g_OwnerPeer) return;
            g_AddressBook.emplace(fromId, a);
            Out o = RvHeader(Rv::Ping); o.u64(g_MyId);
            for (int k = 0; k < 3; k++) RvSend(a, o);
            break;
        }
        case Msg::LobbyClosed:
        {
            if (peer == g_OwnerPeer) { peer->data = nullptr; OnLobbyLostAsMember(); }
            break;
        }
        default:
            break;
        }
    }

    void OnDisconnect(ENetPeer* peer, uint32 data)
    {
        if (HSteamNetConnection h = P2PFromTag(peer->data))
        {
            Conn* c = ConnFor(h);
            peer->data = nullptr;
            if (!c) return;
            c->peer = nullptr;
            auto old = c->state;
            if (old == k_ESteamNetworkingConnectionState_ClosedByPeer || old == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) return;
            if (data == c_DisconnectGraceful)
            {
                c->state = k_ESteamNetworkingConnectionState_ClosedByPeer;
                QueueStatus(h, *c, old, k_ESteamNetConnectionEnd_App_Generic, "Closed by peer");
            }
            else
            {
                c->state = k_ESteamNetworkingConnectionState_ProblemDetectedLocally;
                QueueStatus(h, *c, old, k_ESteamNetConnectionEnd_Remote_Timeout, "Timed out");
            }
            return;
        }

        if (peer == g_OwnerPeer)
        {
            g_OwnerPeer = nullptr;
            OnLobbyLostAsMember();
            return;
        }

        auto it = g_MemberPeers.find(peer);
        if (it != g_MemberPeers.end())
        {
            uint64 memberId = it->second;
            g_MemberPeers.erase(it);
            if (g_IsOwner)
            {
                OwnerMutate([&] {
                    g_Lobby.members.erase(std::remove_if(g_Lobby.members.begin(), g_Lobby.members.end(), [&](const Member& m) { return m.id == memberId; }), g_Lobby.members.end());
                });
            }
        }
    }

    void PumpNetwork()
    {
        if (!Init()) return;
        ENetEvent ev;
        size_t deferred = 0;
        while (true)
        {
            if (deferred < g_DeferredEvents.size()) ev = g_DeferredEvents[deferred++];   // received while paused
            else
            {
                if (!g_DeferredEvents.empty()) { g_DeferredEvents.clear(); deferred = 0; }
                if (enet_host_service(g_Host, &ev, 0) <= 0) break;
            }
            switch (ev.type)
            {
            case ENET_EVENT_TYPE_CONNECT:
                enet_peer_timeout(ev.peer, 0, 5000, g_TimeoutConnectedMs);
                if (ev.peer->data == reinterpret_cast<void*>(PeerKind::JoinCandidate) && g_JoinPending && !g_OwnerPeer)
                {
                    g_OwnerPeer = ev.peer;
                    g_OwnerPeer->data = reinterpret_cast<void*>(PeerKind::LobbyToOwner);
                    DropJoinCandidates(ev.peer);
                    g_AddressBook[g_LobbyCache[g_JoinLobbyId].owner] = ev.peer->address;
                    __android_log_print(ANDROID_LOG_INFO, "yoyo", "PFO LAN: joined via %s", AddrStr(ev.peer->address).c_str());
                }
                if (ev.peer == g_OwnerPeer)
                {
                    Out o; o.u8(static_cast<uint8>(Msg::HelloLobby)); o.u64(g_JoinLobbyId); o.u64(g_MyId); o.str(g_MyName);
                    Send(ev.peer, c_ChanControl, o, true);
                }
                else if (HSteamNetConnection h = P2PFromTag(ev.peer->data))
                {
                    if (Conn* c = ConnFor(h); c && !c->incoming && !c->p2pHelloSent)
                    {
                        c->p2pHelloSent = true;
                        Out o; o.u8(static_cast<uint8>(Msg::HelloP2P)); o.u64(g_MyId); o.u64(c->remoteId);
                        Send(ev.peer, c_ChanControl, o, true);
                    }
                }
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                OnReceive(ev.peer, ev.packet, ev.channelID);
                enet_packet_destroy(ev.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                OnDisconnect(ev.peer, ev.data);
                break;
            default:
                break;
            }
        }

        // Can't reach the host directly yet: try through the okplay relay too (whichever connects
        // first wins). files/forcerelay skips the direct attempts (for testing).
        if (g_JoinPending && g_OkpJoinRoom && !g_JoinRelayTried && NowMs() - g_JoinStartMs > (g_ForceRelay ? 0 : 2500))
        {
            g_JoinRelayTried = true;
            RelayStart(g_OkpJoinRoom, 20000);
            ENetAddress a = RelayAddressFor(g_JoinHostPeer);
            if (ENetPeer* p = enet_host_connect(g_Host, &a, c_Channels, 0))
            {
                p->data = reinterpret_cast<void*>(PeerKind::JoinCandidate);
                g_JoinCandidates.push_back(p);
                __android_log_print(ANDROID_LOG_INFO, "yoyo", "PFO LAN: no direct answer yet - trying the relay too");
            }
        }
        UpdateConnStatus(g_IsOwner ? nullptr : g_OwnerPeer);

        if (g_JoinPending && NowMs() > g_JoinDeadline)
        {
            DropJoinCandidates(nullptr);
            if (g_OwnerPeer) { g_OwnerPeer->data = nullptr; enet_peer_reset(g_OwnerPeer); g_OwnerPeer = nullptr; }
            OnLobbyLostAsMember();
        }
    }

    void Pump()
    {
        if (g_Initialised)
        {
            std::string before = g_MyName;
            ApplyPendingName(g_InLobby);
            if (g_MyName != before) g_Names[g_MyId] = g_MyName;
        }
        PumpNetwork();
        PumpDiscovery();
        PumpRendezvous();
        PumpOkplay();
        if (g_Host) enet_host_flush(g_Host);
    }

    void DispatchCallbacks()
    {
        while (!g_PendingCallbacks.empty())
        {
            PendingCallback p = g_PendingCallbacks.front();
            g_PendingCallbacks.pop_front();
            void* param = p.id == ECallback::NetConnectionStatusChanged ? static_cast<void*>(&p.status) : static_cast<void*>(&p.lobbyData);
            for (auto& fn : g_Callbacks[p.id]) fn(param);
        }
        while (!g_GmlEvents.empty())
        {
            auto fn = std::move(g_GmlEvents.front());
            g_GmlEvents.pop_front();
            fn();
        }
    }

    // Public lobby operations used by the GML-facing exports.
    uint64 NewLobbyId()
    {
        std::random_device rd;
        uint32 room = static_cast<uint32>(rd() ^ g_MyId) | 1;
        return 0x2000000000000000ull | room; // lobby ids: bit 61 set, low 32 bits = okplay room id
    }

    void CreateLobby(int limit, bool isPrivate)
    {
        Init();
        LeaveLobby();
        g_Lobby.id = NewLobbyId();
        g_Lobby.owner = g_MyId;
        g_Lobby.limit = std::max(2, std::min(limit, 16));
        {
            std::lock_guard<std::mutex> lock(g_NameMutex);
            g_LobbyCode = isPrivate ? NewLobbyCode() : "";
        }
        if (isPrivate) g_Lobby.data["pfo_private"] = CodeTag(g_LobbyCode);
        UpnpMapAsync();
        Member me; me.id = g_MyId; me.name = g_MyName;
        g_Lobby.members.push_back(me);
        g_InLobby = true;
        g_IsOwner = true;
        GmlLobbyResult("lobby_created", g_Lobby.id, true);
    }

    bool JoinLobby(uint64 lobbyId)
    {
        Init();
        auto it = g_LobbyCache.find(lobbyId);
        if (it == g_LobbyCache.end() || !g_Host)
        {
            g_JoinLobbyId = lobbyId;
            GmlLobbyResult("lobby_joined", lobbyId, false);
            return false;
        }
        LobbyInfo info = it->second;
        LeaveLobby();
        RendezvousPunch(info.owner);
        std::vector<ENetAddress> cands = info.cands;
        if (cands.empty()) { ENetAddress a; a.host = info.ip; a.port = info.port; cands.push_back(a); }
        g_ForceRelay = info.okplay && (access("forcerelay", F_OK) == 0 || access("/sdcard/Android/data/" PFO_PACKAGE "/files/forcerelay", F_OK) == 0);
        if (g_ForceRelay) { cands.clear(); __android_log_print(ANDROID_LOG_INFO, "yoyo", "PFO LAN: forcerelay: skipping direct addresses"); }
        g_JoinStartMs = NowMs();
        g_JoinHostPeer = info.owner;
        g_JoinRelayTried = false;
        for (auto& a : cands)
        {
            ENetPeer* p = enet_host_connect(g_Host, &a, c_Channels, 0);
            if (!p) continue;
            p->data = reinterpret_cast<void*>(PeerKind::JoinCandidate);
            g_JoinCandidates.push_back(p);
        }
        if (g_JoinCandidates.empty() && !(info.okplay && g_ForceRelay))
        {
            GmlLobbyResult("lobby_joined", lobbyId, false);
            return false;
        }
        g_JoinPending = true;
        g_JoinLobbyId = lobbyId;
        g_JoinDeadline = NowMs() + (info.okplay ? 19000 : c_JoinTimeoutMs);
        if (info.okplay) { g_OkpJoinRoom = info.room; g_OkpNextKnock = 0; } // knock now and every 3 s
        return true;
    }

    void SetMemberData(const std::string& key, const std::string& value)
    {
        if (!g_InLobby) return;
        if (g_IsOwner)
        {
            OwnerMutate([&] { if (Member* m = g_Lobby.Find(g_MyId)) m->data[key] = value; });
        }
        else
        {
            // Apply locally right away (Steam does too), then tell the owner.
            if (Member* m = g_Lobby.Find(g_MyId)) m->data[key] = value;
            Out o; o.u8(static_cast<uint8>(Msg::SetMemberData)); o.str(key); o.str(value);
            Send(g_OwnerPeer, c_ChanControl, o, true);
        }
    }

    const std::string* FindLobbyData(uint64 lobbyId, const std::string& key)
    {
        if (g_InLobby && lobbyId == g_Lobby.id)
        {
            auto it = g_Lobby.data.find(key);
            return it == g_Lobby.data.end() ? nullptr : &it->second;
        }
        auto li = g_LobbyCache.find(lobbyId);
        if (li == g_LobbyCache.end()) return nullptr;
        auto it = li->second.data.find(key);
        return it == li->second.data.end() ? nullptr : &it->second;
    }

    std::string NameOf(uint64 id)
    {
        auto it = g_Names.find(id);
        return it == g_Names.end() ? std::string() : it->second;
    }
}

// ---- fake Steam interface implementations ----

void SteamNetworkingMessage_t::Release()
{
    free(m_pData);
    delete this;
}

CSteamID ISteamUser::GetSteamID() { fakesteam::Init(); return CSteamID(fakesteam::g_MyId); }
const char* ISteamFriends::GetPersonaName() { fakesteam::Init(); return fakesteam::g_MyName.c_str(); }

const char* ISteamMatchmaking::GetLobbyData(CSteamID lobby, const char* key)
{
    auto* v = fakesteam::FindLobbyData(lobby.ConvertToUint64(), key ? key : "");
    return v ? v->c_str() : "";
}

const char* ISteamMatchmaking::GetLobbyMemberData(CSteamID lobby, CSteamID user, const char* key)
{
    using namespace fakesteam;
    if (!g_InLobby || lobby.ConvertToUint64() != g_Lobby.id) return "";
    Member* m = g_Lobby.Find(user.ConvertToUint64());
    if (!m) return "";
    auto it = m->data.find(key ? key : "");
    return it == m->data.end() ? "" : it->second.c_str();
}

void ISteamMatchmaking::SetLobbyMemberData(CSteamID lobby, const char* key, const char* value)
{
    if (lobby.ConvertToUint64() != fakesteam::g_Lobby.id) return;
    fakesteam::SetMemberData(key ? key : "", value ? value : "");
}

bool ISteamMatchmaking::RequestLobbyData(CSteamID) { return false; }

int ISteamMatchmaking::GetNumLobbyMembers(CSteamID lobby)
{
    using namespace fakesteam;
    if (g_InLobby && lobby.ConvertToUint64() == g_Lobby.id) return static_cast<int>(g_Lobby.members.size());
    auto it = g_LobbyCache.find(lobby.ConvertToUint64());
    return it == g_LobbyCache.end() ? 0 : it->second.members;
}

int ISteamMatchmaking::GetLobbyMemberLimit(CSteamID lobby)
{
    using namespace fakesteam;
    if (g_InLobby && lobby.ConvertToUint64() == g_Lobby.id) return g_Lobby.limit;
    auto it = g_LobbyCache.find(lobby.ConvertToUint64());
    return it == g_LobbyCache.end() ? 0 : it->second.limit;
}

HSteamListenSocket ISteamNetworkingSockets::CreateListenSocketP2P(int, int, const void*)
{
    if (!fakesteam::Init()) return k_HSteamListenSocket_Invalid;
    fakesteam::g_Listening = true;
    return fakesteam::c_ListenHandle;
}

HSteamNetConnection ISteamNetworkingSockets::ConnectP2P(const SteamNetworkingIdentity& identity, int, int, const void*)
{
    using namespace fakesteam;
    if (!Init()) return k_HSteamNetConnection_Invalid;
    RendezvousPunch(identity.GetSteamID64());
    if (g_InLobby && !g_IsOwner && g_OwnerPeer && identity.GetSteamID64() != g_Lobby.owner)
    {
        Out o; o.u8(static_cast<uint8>(Msg::PunchRequest)); o.u64(identity.GetSteamID64());
        Send(g_OwnerPeer, c_ChanControl, o, true);
    }
    auto it = g_AddressBook.find(identity.GetSteamID64());
    if (it == g_AddressBook.end()) return k_HSteamNetConnection_Invalid;
    ENetAddress addr = it->second;
    ENetPeer* peer = enet_host_connect(g_Host, &addr, c_Channels, 0);
    if (!peer) return k_HSteamNetConnection_Invalid;
    HSteamNetConnection h = g_NextConn++;
    Conn& c = g_Conns[h];
    c.peer = peer;
    c.remoteId = identity.GetSteamID64();
    c.state = k_ESteamNetworkingConnectionState_Connecting;
    peer->data = P2PTag(h);
    return h;
}

EResult ISteamNetworkingSockets::AcceptConnection(HSteamNetConnection h)
{
    using namespace fakesteam;
    Conn* c = ConnFor(h);
    if (!c || !c->incoming || c->state != k_ESteamNetworkingConnectionState_Connecting || !c->peer) return k_EResultInvalidParam;
    Out o; o.u8(static_cast<uint8>(Msg::P2PAccepted));
    Send(c->peer, c_ChanControl, o, true);
    enet_host_flush(g_Host);
    c->state = k_ESteamNetworkingConnectionState_Connected;
    QueueStatus(h, *c, k_ESteamNetworkingConnectionState_Connecting, 0, "");
    return k_EResultOK;
}

bool ISteamNetworkingSockets::CloseConnection(HSteamNetConnection h, int, const char*, bool)
{
    using namespace fakesteam;
    auto it = g_Conns.find(h);
    if (it == g_Conns.end()) return false;
    Conn& c = it->second;
    if (c.peer)
    {
        c.peer->data = nullptr;
        enet_peer_disconnect_later(c.peer, c_DisconnectGraceful);
    }
    for (auto* m : c.inbox) m->Release();
    g_Conns.erase(it);
    if (g_Host) enet_host_flush(g_Host);
    return true;
}

bool ISteamNetworkingSockets::CloseListenSocket(HSteamListenSocket)
{
    fakesteam::g_Listening = false;
    return true;
}

EResult ISteamNetworkingSockets::ConfigureConnectionLanes(HSteamNetConnection, int, const int*, const uint16*) { return k_EResultOK; }

EResult ISteamNetworkingSockets::GetConnectionRealTimeStatus(HSteamNetConnection h, SteamNetConnectionRealTimeStatus_t* status, int numLanes, SteamNetConnectionRealTimeLaneStatus_t* lanes)
{
    using namespace fakesteam;
    Conn* c = ConnFor(h);
    if (!c) return k_EResultNoConnection;
    if (status)
    {
        *status = {};
        status->m_eState = c->state;
        status->m_nPing = c->peer ? static_cast<int>(c->peer->roundTripTime) : -1;
        status->m_flConnectionQualityLocal = 1.0f;
        status->m_flConnectionQualityRemote = 1.0f;
        status->m_nSendRateBytesPerSecond = 1 << 20;
    }
    for (int i = 0; lanes && i < numLanes; i++) lanes[i] = {};
    return k_EResultOK;
}

int ISteamNetworkingSockets::ReceiveMessagesOnConnection(HSteamNetConnection h, SteamNetworkingMessage_t** out, int maxMessages)
{
    using namespace fakesteam;
    PumpNetwork();
    Conn* c = ConnFor(h);
    if (!c) return -1;
    int n = 0;
    while (n < maxMessages && !c->inbox.empty())
    {
        out[n++] = c->inbox.front();
        c->inbox.pop_front();
    }
    return n;
}

void ISteamNetworkingSockets::SendMessages(int count, SteamNetworkingMessage_t* const* messages, int64* outResults)
{
    using namespace fakesteam;
    for (int i = 0; i < count; i++)
    {
        SteamNetworkingMessage_t* m = messages[i];
        if (!m) { if (outResults) outResults[i] = -k_EResultInvalidParam; continue; }
        Conn* c = ConnFor(m->m_conn);
        if (!c || !c->peer || c->state != k_ESteamNetworkingConnectionState_Connected)
        {
            if (outResults) outResults[i] = -k_EResultNoConnection;
            m->Release();
            continue;
        }
        bool reliable = (m->m_nFlags & k_nSteamNetworkingSend_Reliable) != 0;
        int64 number = c->nextMessageNumber++;
        Out o;
        o.b.reserve(9 + m->m_cbSize);
        o.u8(static_cast<uint8>(Msg::Data));
        o.u64(static_cast<uint64>(number));
        o.b.insert(o.b.end(), static_cast<uint8*>(m->m_pData), static_cast<uint8*>(m->m_pData) + m->m_cbSize);
        Send(c->peer, reliable ? c_ChanReliable : c_ChanUnreliable, o, reliable);
        if (outResults) outResults[i] = number;
        m->Release();
    }
    if (g_Host) enet_host_flush(g_Host);
}

SteamNetworkingMessage_t* ISteamNetworkingUtils::AllocateMessage(int cb)
{
    auto* m = new SteamNetworkingMessage_t();
    *m = {};
    m->m_pData = malloc(cb > 0 ? cb : 1);
    m->m_cbSize = cb;
    return m;
}

SteamNetworkingMicroseconds ISteamNetworkingUtils::GetLocalTimestamp()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

bool ISteamNetworkingUtils::SetGlobalConfigValueInt32(ESteamNetworkingConfigValue value, int32 v)
{
    if (value == k_ESteamNetworkingConfig_TimeoutConnected && v > 0) fakesteam::g_TimeoutConnectedMs = static_cast<uint32>(v);
    return true;
}

namespace
{
    ISteamUser g_FakeSteamUser;
    ISteamFriends g_FakeSteamFriends;
    ISteamMatchmaking g_FakeSteamMatchmaking;
    ISteamNetworkingSockets g_FakeSteamNetworkingSockets;
    ISteamNetworkingUtils g_FakeSteamNetworkingUtils;
}

ISteamUser* SteamUser() { return fakesteam::Init() ? &g_FakeSteamUser : nullptr; }
ISteamFriends* SteamFriends() { return &g_FakeSteamFriends; }
ISteamMatchmaking* SteamMatchmaking() { return &g_FakeSteamMatchmaking; }
ISteamNetworkingSockets* SteamNetworkingSockets() { return &g_FakeSteamNetworkingSockets; }
ISteamNetworkingUtils* SteamNetworkingUtils() { return &g_FakeSteamNetworkingUtils; }

void SteamAPI_RunCallbacks()
{
    fakesteam::Pump();
    fakesteam::DispatchCallbacks();
}

// ---- GML-facing replacements for the Steamworks extension functions PFO's GML uses ----
// These are registered on the PFO extension (instead of Steamworks) in the Android data.win.

#define LAN_EXPORT extern "C" __attribute__((visibility("default")))
#define LAN_FN(name) LAN_EXPORT void name(RValue& result, CInstance* selfinst, CInstance* otherinst, int argc, RValue* arg)

namespace
{
    std::string ArgString(RValue* arg, int argc, int i)
    {
        if (i >= argc) return {};
        const char* s = YYGetString(arg, i);
        return s ? s : "";
    }
    uint64 ArgId(RValue* arg, int argc, int i) { return i < argc ? static_cast<uint64>(YYGetInt64(arg, i)) : 0; }
}

LAN_FN(steam_update) { SteamAPI_RunCallbacks(); init_real(result, 0); }
// false: the base game would otherwise poll Steam stats/achievements/cloud every frame.
LAN_FN(steam_initialised) { fakesteam::Init(); init_bool(result, false); }
// Base-game Steam polling: answer natively so it does not go through Java each frame.
LAN_FN(steam_is_screenshot_requested) { init_bool(result, false); }
LAN_FN(steam_stats_ready) { init_bool(result, false); }
LAN_FN(steam_is_user_logged_on) { init_bool(result, fakesteam::Init()); }
LAN_FN(steam_is_overlay_activated) { init_bool(result, false); }
LAN_FN(steam_get_user_steam_id) { fakesteam::Init(); init_int64(result, static_cast<int64>(fakesteam::g_MyId)); }
LAN_FN(steam_get_persona_name) { fakesteam::Init(); YYCreateString(&result, fakesteam::g_MyName.c_str()); }
LAN_FN(steam_get_user_persona_name_sync) { fakesteam::Init(); YYCreateString(&result, fakesteam::NameOf(ArgId(arg, argc, 0)).c_str()); }

LAN_FN(steam_lobby_create)
{
    int limit = argc > 1 ? YYGetInt32(arg, 1) : 8;
    int type = argc > 0 ? YYGetInt32(arg, 0) : 2;   // GML: 2 = public, 1 = private (friends only)
    fakesteam::CreateLobby(limit, type != 2);
    init_bool(result, true);
}
LAN_FN(steam_lobby_join_id) { fakesteam::Pump(); init_bool(result, fakesteam::JoinLobby(ArgId(arg, argc, 0))); }
LAN_FN(steam_lobby_leave) { fakesteam::LeaveLobby(); init_real(result, 0); }
LAN_FN(steam_lobby_get_lobby_id) { fakesteam::Pump(); init_int64(result, fakesteam::g_InLobby ? static_cast<int64>(fakesteam::g_Lobby.id) : 0); }
LAN_FN(steam_lobby_is_owner) { init_bool(result, fakesteam::g_InLobby && fakesteam::g_IsOwner); }
LAN_FN(steam_lobby_get_owner_id) { init_int64(result, fakesteam::g_InLobby ? static_cast<int64>(fakesteam::g_Lobby.owner) : 0); }
LAN_FN(steam_lobby_get_member_count) { fakesteam::Pump(); init_real(result, fakesteam::g_InLobby ? static_cast<double>(fakesteam::g_Lobby.members.size()) : 0); }
LAN_FN(steam_lobby_get_member_id)
{
    int i = argc > 0 ? YYGetInt32(arg, 0) : -1;
    auto& members = fakesteam::g_Lobby.members;
    init_int64(result, fakesteam::g_InLobby && i >= 0 && i < static_cast<int>(members.size()) ? static_cast<int64>(members[i].id) : 0);
}
LAN_FN(steam_lobby_get_data)
{
    auto* v = fakesteam::FindLobbyData(fakesteam::g_Lobby.id, ArgString(arg, argc, 0));
    YYCreateString(&result, v ? v->c_str() : "");
}
LAN_FN(steam_lobby_set_data)
{
    using namespace fakesteam;
    if (!g_InLobby || !g_IsOwner) { init_bool(result, false); return; }
    std::string k = ArgString(arg, argc, 0), v = ArgString(arg, argc, 1);
    OwnerMutate([&] { g_Lobby.data[k] = v; });
    init_bool(result, true);
}
LAN_FN(steam_lobby_set_joinable)
{
    using namespace fakesteam;
    bool joinable = argc > 0 && YYGetBool(arg, 0);
    if (g_InLobby && g_IsOwner) OwnerMutate([&] { g_Lobby.joinable = joinable; });
    init_bool(result, g_IsOwner);
}
LAN_FN(steam_lobby_set_type) { init_bool(result, true); }
LAN_FN(steam_lobby_activate_invite_overlay)
{
    // SHARE CODE (was INVITE FRIENDS): the overlay shows this lobby's code. Worked out here, on the
    // game thread: the host's own code, or for a joiner the code they entered that matches.
    using namespace fakesteam;
    std::string code;
    {
        std::lock_guard<std::mutex> lock(g_NameMutex);
        code = g_IsOwner ? g_LobbyCode : "";
    }
    if (code.empty() && g_InLobby)
    {
        auto it = g_Lobby.data.find("pfo_private");
        if (it != g_Lobby.data.end())
        {
            std::lock_guard<std::mutex> lock(g_NameMutex);
            for (auto& c : g_JoinCodes) if (CodeTag(c) == it->second) code = c;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_NameMutex);
        g_ShareCode = code;
    }
    g_InviteRequested = true;
    init_bool(result, true);
}
LAN_FN(steam_lobby_list_add_string_filter) { init_bool(result, true); }
LAN_FN(steam_lobby_list_add_numerical_filter) { init_bool(result, true); }
LAN_FN(steam_lobby_list_add_near_filter) { init_bool(result, true); }
LAN_FN(steam_lobby_list_add_distance_filter) { init_bool(result, true); }
LAN_FN(steam_lobby_list_request) { fakesteam::Init(); fakesteam::StartLobbySearch(); init_bool(result, true); }
LAN_FN(steam_lobby_list_is_loading) { fakesteam::Pump(); init_bool(result, fakesteam::g_ListLoading); }
LAN_FN(steam_lobby_list_get_count) { init_real(result, static_cast<double>(fakesteam::g_LobbyList.size())); }
LAN_FN(steam_lobby_list_get_lobby_id)
{
    int i = argc > 0 ? YYGetInt32(arg, 0) : -1;
    auto& list = fakesteam::g_LobbyList;
    init_int64(result, i >= 0 && i < static_cast<int>(list.size()) ? static_cast<int64>(list[i].id) : 0);
}
LAN_FN(steam_lobby_list_get_data)
{
    int i = argc > 0 ? YYGetInt32(arg, 0) : -1;
    auto& list = fakesteam::g_LobbyList;
    std::string key = ArgString(arg, argc, 1);
    const char* v = "";
    if (i >= 0 && i < static_cast<int>(list.size())) { auto it = list[i].data.find(key); if (it != list[i].data.end()) v = it->second.c_str(); }
    YYCreateString(&result, v);
}

// Extra LAN helpers for a future "join by IP" UI.
LAN_FN(pfo_lan_get_port) { fakesteam::Init(); init_real(result, fakesteam::g_Port); }
LAN_FN(pfo_lan_add_host)
{
    // Remember a host IP to query on the next lobby search (also persisted to lan_hosts.txt).
    std::string ip = ArgString(arg, argc, 0);
    in_addr a;
    bool ok = inet_aton(ip.c_str(), &a) != 0;
    if (ok) { if (FILE* f = fopen("lan_hosts.txt", "a")) { fprintf(f, "%s\n", ip.c_str()); fclose(f); } }
    init_bool(result, ok);
}
