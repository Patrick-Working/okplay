// Fake Steamworks API for the Android build of PFO 50.
//
// Declares only the Steam types and calls PFO uses. The implementation
// (android/fakesteam_impl.inc) runs them over ENet/UDP with a LAN lobby,
// so PFO's netcode compiles unchanged and works without a Steam client.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <functional>
#include <vector>

typedef uint8_t uint8;
typedef int8_t int8;
typedef int16_t int16;
typedef uint16_t uint16;
typedef int32_t int32;
typedef uint32_t uint32;
typedef long long int64;
typedef unsigned long long uint64;

typedef uint32 HSteamNetConnection;
typedef uint32 HSteamListenSocket;
typedef int64 SteamNetworkingMicroseconds;
constexpr HSteamNetConnection k_HSteamNetConnection_Invalid = 0;
constexpr HSteamListenSocket k_HSteamListenSocket_Invalid = 0;

enum EResult
{
    k_EResultNone = 0,
    k_EResultOK = 1,
    k_EResultFail = 2,
    k_EResultNoConnection = 3,
    k_EResultInvalidParam = 8,
    k_EResultLimitExceeded = 25,
};

constexpr int k_nSteamNetworkingSend_Unreliable = 0;
constexpr int k_nSteamNetworkingSend_NoNagle = 1;
constexpr int k_nSteamNetworkingSend_NoDelay = 4;
constexpr int k_nSteamNetworkingSend_UnreliableNoDelay = k_nSteamNetworkingSend_Unreliable | k_nSteamNetworkingSend_NoDelay | k_nSteamNetworkingSend_NoNagle;
constexpr int k_nSteamNetworkingSend_Reliable = 8;
constexpr int k_cbMaxSteamNetworkingSocketsMessageSizeSend = 512 * 1024;

enum ESteamNetworkingConnectionState
{
    k_ESteamNetworkingConnectionState_None = 0,
    k_ESteamNetworkingConnectionState_Connecting = 1,
    k_ESteamNetworkingConnectionState_FindingRoute = 2,
    k_ESteamNetworkingConnectionState_Connected = 3,
    k_ESteamNetworkingConnectionState_ClosedByPeer = 4,
    k_ESteamNetworkingConnectionState_ProblemDetectedLocally = 5,
};

enum ESteamNetConnectionEnd
{
    k_ESteamNetConnectionEnd_Invalid = 0,
    k_ESteamNetConnectionEnd_App_Generic = 1000,
    k_ESteamNetConnectionEnd_AppException_Generic = 2000,
    k_ESteamNetConnectionEnd_Local_Min = 3000,
    k_ESteamNetConnectionEnd_Remote_Timeout = 4001,
    k_ESteamNetConnectionEnd_Misc_Generic = 5001,
    k_ESteamNetConnectionEnd_Misc_Timeout = 5003,
};

enum ESteamNetworkingConfigValue
{
    k_ESteamNetworkingConfig_SendBufferSize = 9,
    k_ESteamNetworkingConfig_TimeoutConnected = 25,
};

class CSteamID
{
public:
    constexpr CSteamID() : m_id(0) {}
    constexpr explicit CSteamID(uint64 id) : m_id(id) {}
    constexpr uint64 ConvertToUint64() const { return m_id; }
    constexpr bool IsValid() const { return m_id != 0; }
    constexpr bool operator==(const CSteamID& o) const { return m_id == o.m_id; }
    constexpr bool operator!=(const CSteamID& o) const { return m_id != o.m_id; }
private:
    uint64 m_id;
};

struct SteamNetworkingIdentity
{
    uint64 m_id = 0;
    void SetSteamID64(uint64 id) { m_id = id; }
    uint64 GetSteamID64() const { return m_id; }
};

struct SteamNetworkingMessage_t
{
    void* m_pData;
    int m_cbSize;
    HSteamNetConnection m_conn;
    SteamNetworkingIdentity m_identityPeer;
    int64 m_nConnUserData;
    SteamNetworkingMicroseconds m_usecTimeReceived;
    int64 m_nMessageNumber;
    int m_nChannel;
    int m_nFlags;
    int64 m_nUserData;
    uint16 m_idxLane;
    void Release();
};

struct SteamNetConnectionInfo_t
{
    SteamNetworkingIdentity m_identityRemote;
    int64 m_nUserData;
    HSteamListenSocket m_hListenSocket;
    int m_eState;
    int m_eEndReason;
    char m_szEndDebug[128];
    char m_szConnectionDescription[128];
};

struct SteamNetConnectionStatusChangedCallback_t
{
    HSteamNetConnection m_hConn;
    SteamNetConnectionInfo_t m_info;
    ESteamNetworkingConnectionState m_eOldState;
};

struct SteamNetConnectionRealTimeStatus_t
{
    ESteamNetworkingConnectionState m_eState;
    int m_nPing;
    float m_flConnectionQualityLocal;
    float m_flConnectionQualityRemote;
    float m_flOutPacketsPerSec;
    float m_flOutBytesPerSec;
    float m_flInPacketsPerSec;
    float m_flInBytesPerSec;
    int m_nSendRateBytesPerSecond;
    int m_cbPendingUnreliable;
    int m_cbPendingReliable;
    int m_cbSentUnackedReliable;
    SteamNetworkingMicroseconds m_usecQueueTime;
};

struct SteamNetConnectionRealTimeLaneStatus_t
{
    int m_cbPendingUnreliable;
    int m_cbPendingReliable;
    int m_cbSentUnackedReliable;
    int _reservePad1;
    SteamNetworkingMicroseconds m_usecQueueTime;
};

struct LobbyDataUpdate_t
{
    uint64 m_ulSteamIDLobby;
    uint64 m_ulSteamIDMember;
    uint8 m_bSuccess;
};

class ISteamUser
{
public:
    CSteamID GetSteamID();
};

class ISteamFriends
{
public:
    const char* GetPersonaName();
};

class ISteamMatchmaking
{
public:
    const char* GetLobbyData(CSteamID lobby, const char* key);
    const char* GetLobbyMemberData(CSteamID lobby, CSteamID user, const char* key);
    void SetLobbyMemberData(CSteamID lobby, const char* key, const char* value);
    bool RequestLobbyData(CSteamID lobby);
    int GetNumLobbyMembers(CSteamID lobby);
    int GetLobbyMemberLimit(CSteamID lobby);
};

class ISteamNetworkingSockets
{
public:
    HSteamListenSocket CreateListenSocketP2P(int localVirtualPort, int nOptions, const void* pOptions);
    HSteamNetConnection ConnectP2P(const SteamNetworkingIdentity& identity, int remoteVirtualPort, int nOptions, const void* pOptions);
    EResult AcceptConnection(HSteamNetConnection conn);
    bool CloseConnection(HSteamNetConnection conn, int reason, const char* debug, bool linger);
    bool CloseListenSocket(HSteamListenSocket socket);
    EResult ConfigureConnectionLanes(HSteamNetConnection conn, int numLanes, const int* priorities, const uint16* weights);
    EResult GetConnectionRealTimeStatus(HSteamNetConnection conn, SteamNetConnectionRealTimeStatus_t* status, int numLanes, SteamNetConnectionRealTimeLaneStatus_t* lanes);
    int ReceiveMessagesOnConnection(HSteamNetConnection conn, SteamNetworkingMessage_t** out, int maxMessages);
    void SendMessages(int count, SteamNetworkingMessage_t* const* messages, int64* outResults);
    int InitAuthentication() { return 0; }
};

class ISteamNetworkingUtils
{
public:
    SteamNetworkingMessage_t* AllocateMessage(int cbAllocateBuffer);
    SteamNetworkingMicroseconds GetLocalTimestamp();
    void InitRelayNetworkAccess() {}
    bool SetGlobalConfigValueInt32(ESteamNetworkingConfigValue value, int32 v);
};

ISteamUser* SteamUser();
ISteamFriends* SteamFriends();
ISteamMatchmaking* SteamMatchmaking();
ISteamNetworkingSockets* SteamNetworkingSockets();
ISteamNetworkingUtils* SteamNetworkingUtils();
void SteamAPI_RunCallbacks();

// Callback plumbing: STEAM_CALLBACK declares a member that registers a handler
// for one callback struct type. Handlers run inside SteamAPI_RunCallbacks().
namespace fakesteam
{
    enum class ECallback { NetConnectionStatusChanged, LobbyDataUpdate };

    template<typename T> struct CallbackId;
    template<> struct CallbackId<SteamNetConnectionStatusChangedCallback_t> { static constexpr ECallback value = ECallback::NetConnectionStatusChanged; };
    template<> struct CallbackId<LobbyDataUpdate_t> { static constexpr ECallback value = ECallback::LobbyDataUpdate; };

    void RegisterCallback(ECallback id, std::function<void(void*)> fn);

    template<typename T>
    struct CallbackReg
    {
        explicit CallbackReg(std::function<void(T*)> fn)
        {
            RegisterCallback(CallbackId<T>::value, [fn](void* p) { fn(static_cast<T*>(p)); });
        }
    };
}

#define STEAM_CALLBACK(thisclass, func, param) \
    fakesteam::CallbackReg<param> m_fakesteam_cb_##func{ [this](param* p) { this->func(p); } }; \
    void func(param* pParam)
