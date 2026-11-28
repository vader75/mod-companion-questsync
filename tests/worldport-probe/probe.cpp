#include <cstdio>
#include <cstdint>
#include <ctime>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <string_view>
using uint32 = uint32_t;
static uint32 clockNow;
#define time(...) clockNow
#define LOG_INFO(...) ((void)0)
#include "cq-exit-timing.inc"
enum class HighGuid { Player };
struct ObjectGuid { template<HighGuid> static uint32 Create(uint32 i) { return i; } uint32 GetCounter() const { return 1; } };
struct Map { uint32 id, instance; bool Instanceable() const { return instance != 0; } bool IsDungeon() const { return Instanceable(); } };
static Map outside{0,0}, dungeon{1,7}, otherInstance{1,9}, differentWorld{2,0};
using MapEntry = Map;
static struct { MapEntry const* LookupEntry(uint32 id) { return id == 0 ? &outside : (id == 1 ? &dungeon : &differentWorld); } } sMapStore;
struct Session { bool IsBot() const { return false; } };
struct Unit {};
struct Player {
    Map* map;
    bool combat = false, alive = true, inWorld = true, falling = false;
    int group = 1;
    Session* GetSession() { static Session session; return &session; }
    char const* GetName() const { return map == &dungeon ? "busy" : "ready"; }
    ObjectGuid GetGUID() const { return {}; }
    uint32 GetInstanceId() const { return map->instance; }
    int GetGroup() const { return group; }
    bool IsInWorld() const { return inWorld; } bool IsBeingTeleported() const { return false; }
    bool IsAlive() const { return alive; } bool IsInCombat() const { return combat; }
    bool GetTransport() const { return false; } bool GetVehicle() const { return false; }
    bool IsFlying() const { return false; } bool IsFalling() const { return falling; }
    Map* GetMap() const { return map; } uint32 GetMapId() const { return map->id; }
    float GetDistance(float, float, float) const { return 0; } float GetDistance(Player*) const { return 0; }
    float GetPositionX() const { return 0; } float GetPositionY() const { return 0; }
    float GetPositionZ() const { return 0; } float GetOrientation() const { return 0; }
    void UpdateGroundPositionZ(float, float, float&) {} void StopMoving() {}
    bool TeleportTo(uint32 id, float, float, float, float) { if (id != 0) return false; map = &outside; return true; }
    void NearTeleportTo(float, float, float, float) {}
};
static Player owner{&outside}, ready{&dungeon}, busy{&dungeon};
struct ObjectAccessor { static Player* FindPlayer(uint32) { return owner.IsInWorld() ? &owner : nullptr; } };
static bool IsOnTaxi(Player*) { return false; }
static bool FindCompanions(uint32, std::vector<uint32>& bots) { bots = {2, 3}; return true; }
static Player* FindCompanionPlayer(uint32 id) { return id == 2 ? &ready : &busy; }
#include "cq-exit-pending.inc"
static std::map<uint32, PendingFollow> g_pendingFollow;
static bool IsControlledCompanion(Player*, Player*) { return true; }
static bool SayToOwner(Player*, Player*, std::string const&, uint32, uint32, bool) { return false; }
static struct { bool followThroughTeleport = true, enable = true; int debug = 0; } g_cfg;
#include "cq-exit-function.inc"
struct PlayerScript {
    virtual bool OnPlayerBeforeTeleport(Player*, uint32, float, float, float, float, uint32, Unit*) = 0;
    virtual void OnPlayerMapChanged(Player*) = 0;
};
struct ExitHooks final : PlayerScript {
#include "cq-exit-hooks.inc"
};
bool run(std::string_view name, uint32 release, bool expectOutside, bool confirmed = true, bool reenter = false,
         Map* botMap = &dungeon, int group = 1, Map* source = &dungeon, bool stuckFall = false)
{
    g_pendingFollow.clear();
    clockNow = 0;
    owner = Player{source};
    ready = Player{&dungeon};
    busy = Player{botMap};
    busy.combat = true;
    busy.group = group;
    busy.falling = stuckFall;
    ExitHooks hooks;
    hooks.OnPlayerBeforeTeleport(&owner, 0, 0, 0, 0, 0, 0, nullptr);
    owner.inWorld = false;
    for (clockNow=0; clockNow<=310; ++clockNow)
    {
        if (clockNow == 7)
        {
            owner.inWorld = true;
            if (confirmed)
            {
                owner.map = &outside;
                hooks.OnPlayerMapChanged(&owner);
            }
        }
        if (clockNow == release) busy.combat = false;
        if (reenter && clockNow == 20)
        {
            hooks.OnPlayerBeforeTeleport(&owner, 1, 0, 0, 0, 0, 0, nullptr);
            owner.map = &dungeon;
            hooks.OnPlayerMapChanged(&owner);
        }
        FollowOwnerThroughTeleport();
    }
    bool const passed = (ready.map == &outside) == confirmed && (busy.map == &outside) == expectOutside && g_pendingFollow.empty();
    std::printf("%s %.*s: ready=%u straggler=%u/%u pending=%zu\n", passed ? "PASS" : "FAIL", int(name.size()), name.data(), ready.GetMapId(), busy.GetMapId(), busy.GetInstanceId(), g_pendingFollow.size());
    return passed;
}
int main()
{
    bool ok = run("normal window", 59, true);
    ok &= run("same-instance straggler after worldport ACK", 72, true);
    ok &= run("missed 5-minute deadline", 309, false);
    ok &= run("different instance of same dungeon", 72, false, true, false, &otherInstance);
    ok &= run("not grouped", 72, false, true, false, &dungeon, 2);
    ok &= run("unconfirmed exit", 62, false, false);
    ok &= run("owner re-enters", 72, false, true, true);
    ok &= run("ordinary outdoor travel expires at 60s", 72, false, true, false, &dungeon, 1, &differentWorld);
    ok &= run("stuck falling flag still follows", 72, true, true, false, &dungeon, 1, &dungeon, true);
    return ok ? 0 : 1;
}
