// BlockerPasses — Plugify port of the Metamod plugin BlockerPasses (ABKAM)
// (ref/src/BlockerPasses/BlockerPasses.cpp, 2552 lines).
//
// Ported 1:1 (the whole feature set: item/wall placement, the eleven menus, the ping flows, the
// per-map data file, the permission/temporary access, the player-count gate):
//   * "mm_bp" (console) / "!bp" (chat) opens the admin menu for a player with the configured
//     permission (@admin/bp) or a SteamID64 granted this map via "mm_bp_access"; every other
//     command is unchanged;
//   * place a model from settings.ini at the crosshair position, or a wall between two ping
//     points; walls are a hidden collision box (func_brush with OBB bounds through the
//     SetCollisionBounds signature) plus a 24-segment env_beam wireframe (optional rainbow);
//   * the item menu (move/rotate/scale/invisibility/item colour/ping-teleport/crosshair-move/
//     delete), the wall menu (move/scale/rotate/beam colour/ping-teleport/crosshair-move);
//   * the pass is closed below min_players_to_open (humans, optionally ignoring spectators): the
//     live entities are removed ("Chat_ClosedMsg" announcement) and respawned when the round with
//     enough players starts;
//   * everything is stored per map in addons/data/bp_data.ini (KV1, the section of the other maps
//     is preserved), the phrases come from addons/translations/blockerpasses.phrases.txt.
//
// Port notes (engine access that changed):
//   * raw entity pointers / CHandle<> -> s2sdk entity *handles* (the int CreateEntityByName
//     returns); the schema calls convert them with pisex::EntHandleToEntIndex.
//   * CEntityKeyValues + DispatchSpawn -> s2sdk DispatchSpawn2(handle, keys, values).
//   * g_pPlayers->RayTrace (cs2-menus' UTIL_TraceShape) -> s2sdk TraceLine from the eye position
//     along the view direction (16384 units, MASK_SHOT, the player's pawn ignored); the impact
//     point is the same m_vEndPos the original used.
//   * m_Collision().m_* -> the embedded component CBaseModelEntity::m_Collision is resolved with
//     GetEntSchemaPtr (func_brush / prop_dynamic are CBaseModelEntity-derived) and the
//     CCollisionProperty fields are written at component + schema offset (SetEntData2 /
//     SetEntDataVector3D2). The class+field string helpers of pisexcompat must NOT be used with the
//     entity base here: the CCollisionProperty offsets are component-relative (the component sits
//     at +2192), so entity+offset would land in the entity's own early fields. The nested
//     VPhysicsCollisionAttribute_t::m_nCollisionGroup (offset 46 in the attribute struct) goes
//     through m_collisionAttribute's offset + that field's offset, one byte.
//   * m_CBodyComponent->m_pSceneNode->m_flScale -> s2sdk SetEntityAbsScale (the same scene-node
//     scale); m_fEffects/EF_NODRAW -> s2sdk Add/RemoveEntityEffects; m_clrRender ->
//     s2sdk Set/GetEntityRenderColor; m_nRenderMode -> s2sdk SetEntityRenderMode.
//   * the SetCollisionBounds pattern of the original is resolved through the plugin's own
//     gamedata.jsonc (s2sdk LoadGameConfigFile/GetGameConfigSignature, verified unique against
//     the current libserver.so, see assets/gamedata.jsonc).
//   * g_pAdmin (the Pisex IAdminApi) -> plugins/admin_system/include/admin_api.hpp.
//   * the map name comes from pisex::CurrentMap() instead of gv->mapname.ToCStr().
//   * timers and the 0-second deferred SetCollisionBounds use pisexcompat timers (they run from
//     Poll(), i.e. the next frame, one frame later than under Metamod - the original's 0.0 delay
//     had the same purpose).

#include "bp_core.hpp"

#include <admin_api.hpp>

#include <pisex/pisex.hpp>

#include <plg/any.hpp>
#include <plg/plugin.hpp>
#include <plg/source_location.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr const char* kLogTag = "[BlockerPasses]";
constexpr int kMaxSlots = bp::kMaxSlots;

// Entity values of hl2sdk-cs2 (public/const.h and the schema enums).
constexpr int kEfNoDraw = 0x20;		 // EF_NODRAW
constexpr int kSolidNone = 0;		 // SOLID_NONE
constexpr int kSolidObb = 3;		 // SOLID_OBB
constexpr int kSurroundSpecified = 3; // SurroundingBoundsType_t::USE_SPECIFIED_BOUNDS
constexpr int kMaskShot = 0x400000B1; // MASK_SHOT (the player trace mask)

constexpr const char* kSettingsPath = "addons/configs/BlockerPasses/settings.ini";
constexpr const char* kPhrasesFile = "blockerpasses.phrases.txt";
constexpr const char* kDataFile = "addons/data/bp_data.ini";

// The env_beam / func_brush / prop_dynamic classes of the original.
constexpr const char* kBeamClass = "env_beam";
constexpr const char* kCollisionBoxClass = "func_brush";
constexpr const char* kPropClasses[] = {"prop_dynamic", "prop_dynamic_override"};

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

std::string ToString(std::string_view view) {
	return std::string(view);
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

enum PingMode {
	PING_NONE = 0,
	PING_TELEPORT = 1,
	PING_WALL_POS1 = 2,
	PING_WALL_POS2 = 3,
};

class BlockerPassesPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// --- configuration / phrases (LoadSettings / LoadPhrases of the original) ---
	void LoadSettings();
	void LoadPhrases();
	void Dbg(const char* format, ...) __attribute__((format(printf, 2, 3)));

	// --- prints (PrintChatKey / PrintChatAllKey of the original; every call site of the original
	// used a phrase key except the two hard-coded ones, which pass their text as the fallback) ---
	void PrintChatKey(int iSlot, const char* key, const char* fallbackFormat, ...)
			__attribute__((format(printf, 4, 5)));
	void PrintChatAllKey(const char* key, const char* fallbackFormat, ...)
			__attribute__((format(printf, 3, 4)));

	// --- commands / events (registered on the game thread) ---
	bool OnBpCommand(int iSlot);
	bool OnAccessCommand(int iSlot, const char* args);
	void OnMapStart(const char* map);
	void OnMapEnd();
	void OnRoundStart();
	void OnPlayerPing(const pisex::IGameEvent& event);

	// --- the data file ---
	void SaveData();
	void LoadDataForMap(const char* map);
	void EnsureCorrectMapLoaded();

	// --- state applied to the world ---
	void ApplyState();
	void ClearLive(bool removeEntities);

private:
	// --- live item bookkeeping ---
	void RemoveLiveBeams(bp::LiveEnt& live);
	void KillWallCollision(int handle);
	void SpawnLiveEntry(int index, bp::LiveEnt& live);
	// SpawnOne() of the original: creates the prop entity of m_items[index] and adds its live
	// entry; returns the entity handle (-1 when nothing was created).
	int SpawnOne(int index);
	void RespawnLive(int index);
	void RespawnWallBeams(int index);
	void TeleportLive(int index);
	void MakeLiveIfMissing(int index);
	void EnsureClosed();
	void ApplyVisualScaleToLive(int index);
	void ApplyInvisibilityToLive(int index);
	void ApplyItemColorToLive(int index);
	void StartRainbowTimer();

	// --- entity helpers (s2sdk) ---
	int CreateEntity(const char* className);
	void DispatchSpawnKv(int handle, const std::vector<std::pair<const char*, plg::any>>& keysValues);
	void TeleportEnt(int handle, const plg::vec3& origin, const plg::vec3& angles);
	void RemoveEnt(int handle);
	void SetModelOnEnt(int handle, const char* model);
	int EntityIndexOf(int handle);
	void SetStateChangedField(int handle, const char* className, const char* field);
	// The embedded CCollisionProperty component (CBaseModelEntity::m_Collision) of a handle, or
	// nullptr: the collision fields are component-relative, the entity base must not be used.
	void* CollisionPropertyOf(int handle);
	void SetCollisionInt(int handle, const char* field, int value);
	void SetCollisionVec(int handle, const char* field, const plg::vec3& value);
	void SetNoDraw(int handle, bool on);
	void ApplyRenderAlpha(int handle, int alpha);
	void ApplyRenderColor(int handle, int r, int g, int b);
	int CreateBeamLine(const plg::vec3& start, const plg::vec3& end, int r, int g, int b, float width);
	std::vector<int> DrawWireframe(const bp::BPItem& item);
	int SpawnOneCollisionBox(const plg::vec3& center, const plg::vec3& mins, const plg::vec3& maxs, float yaw);
	std::vector<int> SpawnWallCollisions(const bp::BPItem& item);

	// --- engine queries ---
	int HumansOnline();
	bool ShouldBeOpen();
	bool HasBpAccess(int iSlot);
	bool AimPoint(int iSlot, plg::vec3& out) const;
	bool PlayerAlive(int iSlot);

	// --- menus (all of them) ---
	int FindItemByCrosshair(int iSlot, float maxDist);
	void OpenMainMenu(int iSlot);
	void OpenModelMenu(int iSlot);
	void OpenEditListMenu(int iSlot);
	void OpenItemMenu(int iSlot, int index);
	void OpenMoveMenu(int iSlot, int index);
	void OpenRotateMenu(int iSlot, int index);
	void OpenScaleMenu(int iSlot, int index);
	void OpenItemColorMenu(int iSlot, int index);
	void OpenBeamColorMenu(int iSlot, int index);
	void OpenWallMoveMenu(int iSlot, int index);
	void OpenWallScaleMenu(int iSlot, int index);
	void OpenWallRotateMenu(int iSlot, int index);

	// --- gamedata ---
	bool EnsureCollisionBoundsSignature();

	const char* Phrase(const char* key, const char* def);

	bp::Settings m_settings;
	pisex::PhraseMap m_phrases;
	std::vector<bp::BPItem> m_items;
	std::vector<bp::LiveEnt> m_live;
	std::string m_currentMap;

	std::array<PingMode, kMaxSlots> m_pingMode{};
	std::array<int, kMaxSlots> m_pingTargetIndex{};
	std::array<plg::vec3, kMaxSlots> m_wallTempPos{};
	std::vector<uint64_t> m_tempAccessSteamIds;

	float m_rainbowHue = 0.0f;
	bool m_rainbowTimerActive = false;

	// SetCollisionBounds (gamedata): void (*)(CBaseEntity*, const Vector*, const Vector*).
	using SetCollisionBoundsFn = void (*)(void*, const plg::vec3*, const plg::vec3*);
	SetCollisionBoundsFn m_setCollisionBounds = nullptr;
	s2sdk::ConfigId m_gamedata = 0;
	bool m_gamedataTried = false;
};

BlockerPassesPlugin g_blockerPassesPlugin;

// --- phrases / prints ------------------------------------------------------------------------

const char* BlockerPassesPlugin::Phrase(const char* key, const char* def) {
	const auto it = m_phrases.find(key);
	return (it != m_phrases.end() && !it->second.empty()) ? it->second.c_str() : def;
}

void BlockerPassesPlugin::LoadPhrases() {
	m_phrases.clear();
	if (!pisex::LoadPhrasesFile(kPhrasesFile, m_phrases)) {
		LogError(std::string(kLogTag) + " failed to load " + kPhrasesFile + " - using the built-in texts");
	}
}

void BlockerPassesPlugin::Dbg(const char* format, ...) {
	if (!m_settings.debugLog) {
		return;
	}
	char buffer[1024];
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	LogMessage(std::string(kLogTag) + " " + buffer, plg::Severity::Debug);
}

void BlockerPassesPlugin::PrintChatKey(int iSlot, const char* key, const char* fallbackFormat, ...) {
	const char* format = Phrase(key, fallbackFormat);
	char buffer[1024];
	va_list args;
	va_start(args, fallbackFormat);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	const char* tag = Phrase("Chat_Prefix", "");
	if (*tag != '\0') {
		pisex::g_pUtils->PrintToChat(iSlot, "%s %s", tag, buffer);
	} else {
		pisex::g_pUtils->PrintToChat(iSlot, "%s", buffer);
	}
}

void BlockerPassesPlugin::PrintChatAllKey(const char* key, const char* fallbackFormat, ...) {
	const char* format = Phrase(key, fallbackFormat);
	char buffer[1024];
	va_list args;
	va_start(args, fallbackFormat);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	const char* tag = Phrase("Chat_Prefix", "");
	if (*tag != '\0') {
		pisex::g_pUtils->PrintToChatAll("%s %s", tag, buffer);
	} else {
		pisex::g_pUtils->PrintToChatAll("%s", buffer);
	}
}

// --- settings ---------------------------------------------------------------------------------

void BlockerPassesPlugin::LoadSettings() {
	pisex::KeyValues kv("BlockerPasses");
	const bool loaded = kv.LoadFromFile(kSettingsPath);
	m_settings = bp::ParseSettings(loaded ? &kv : nullptr, loaded);
	Dbg("settings: min_players_to_open=%d, debug=%d, perm='%s', flag='%s', chat='%s', concmd='%s', concmd_access='%s', models=%d",
		m_settings.minPlayersToOpen, static_cast<int>(m_settings.debugLog), m_settings.accessPermission.c_str(),
		m_settings.accessFlag.c_str(), m_settings.chatCommand.c_str(), m_settings.conCmdBp.c_str(),
		m_settings.conCmdAccess.c_str(), static_cast<int>(m_settings.models.size()));
}

// --- engine queries ---------------------------------------------------------------------------

int BlockerPassesPlugin::HumansOnline() {
	int count = 0;
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		try {
			if (!pisex::g_pPlayers->IsInGame(slot) || pisex::g_pPlayers->IsFakeClient(slot)) {
				continue;
			}
			if (m_settings.ignoreSpectators && pisex::g_pPlayers->GetTeam(slot) <= 1) {
				continue;
			}
			++count;
		} catch (...) {
		}
	}
	return count;
}

bool BlockerPassesPlugin::ShouldBeOpen() {
	return HumansOnline() >= m_settings.minPlayersToOpen;
}

bool BlockerPassesPlugin::PlayerAlive(int iSlot) {
	return pisex::g_pPlayers->IsAlive(iSlot);
}

bool BlockerPassesPlugin::HasBpAccess(int iSlot) {
	if (iSlot >= 0 && iSlot < kMaxSlots && pisex::g_pPlayers->IsInGame(iSlot)) {
		const uint64_t steamid = pisex::g_pPlayers->GetSteamID64(iSlot);
		if (steamid != 0) {
			for (const uint64_t granted : m_tempAccessSteamIds) {
				if (granted == steamid) {
					return true;
				}
			}
		}
	}
	if (!admin_system::detail::Available()) {
		return false;
	}
	if (!m_settings.accessPermission.empty()) {
		return admin_system::Admin().HasPermission(iSlot, m_settings.accessPermission.c_str());
	}
	if (!m_settings.accessFlag.empty()) {
		return admin_system::Admin().HasFlag(iSlot, m_settings.accessFlag.c_str());
	}
	return false;
}

bool BlockerPassesPlugin::AimPoint(int iSlot, plg::vec3& out) const {
	if (!__s2sdk_TraceLine || !__s2sdk_GetClientEyePosition || !__s2sdk_GetClientForwardVector ||
		!__s2sdk_EntIndexToEntHandle) {
		return false;
	}
	const int pawn = pisex::GetPlayerPawn(iSlot);
	if (pawn == 0) {
		return false;
	}
	try {
		const plg::vec3 start = s2sdk::GetClientEyePosition(iSlot);
		const plg::vec3 forward = s2sdk::GetClientForwardVector(iSlot);
		const plg::vec3 end{start.x + forward.x * 16384.0f, start.y + forward.y * 16384.0f,
							start.z + forward.z * 16384.0f};
		const int ignore = pisex::EntIndexToEntHandle(pawn);
		plg::vec3 outPos{};
		double fraction = 0.0;
		bool hit = false;
		int hitEntity = 0;
		bool startSolid = false;
		s2sdk::TraceLine(start, end, kMaskShot, ignore, outPos, fraction, hit, hitEntity, startSolid);
		out = outPos;
		return true;
	} catch (...) {
		return false;
	}
}

int BlockerPassesPlugin::FindItemByCrosshair(int iSlot, float maxDist) {
	plg::vec3 hit{};
	if (!AimPoint(iSlot, hit)) {
		return -1;
	}

	int best = -1;
	float bestSq = maxDist * maxDist;
	for (const bp::LiveEnt& live : m_live) {
		const int idx = live.index;
		if (idx < 0 || idx >= static_cast<int>(m_items.size())) {
			continue;
		}
		const plg::vec3& p = m_items[static_cast<std::size_t>(idx)].pos;
		const float d = p.x - hit.x, e = p.y - hit.y, f = p.z - hit.z;
		const float dsq = d * d + e * e + f * f;
		if (dsq <= bestSq) {
			bestSq = dsq;
			best = idx;
		}
	}
	return best;
}

// --- entity helpers ---------------------------------------------------------------------------

// True while a stored entity handle still resolves to a live entity. Every s2sdk entity call with
// a stale handle logs "Cannot execute '...' on invalid entity handle: N" and does nothing, and a
// sign test alone is not enough either way: a valid handle can be negative (once the engine's
// serial counter passed 65535) and a handle the engine freed (e.g. the previous map's items
// before the map-end cleanup ran) can still look positive - so the entity calls below are gated on
// this.
bool HandleValid(int handle) {
	return bp::IsValidHandle(handle) && __s2sdk_EntHandleToEntIndex != nullptr &&
		   s2sdk::EntHandleToEntIndex(handle) > 0;
}

int BlockerPassesPlugin::CreateEntity(const char* className) {
	if (!__s2sdk_CreateEntityByName) {
		return -1;
	}
	try {
		return s2sdk::CreateEntityByName(plg::string(className));
	} catch (...) {
		return -1;
	}
}

void BlockerPassesPlugin::DispatchSpawnKv(int handle,
										  const std::vector<std::pair<const char*, plg::any>>& keysValues) {
	if (!__s2sdk_DispatchSpawn2 || !HandleValid(handle)) {
		return;
	}
	try {
		plg::vector<plg::string> keys;
		plg::vector<plg::any> values;
		keys.reserve(keysValues.size());
		values.reserve(keysValues.size());
		for (const auto& [key, value] : keysValues) {
			keys.emplace_back(key);
			values.push_back(value);
		}
		s2sdk::DispatchSpawn2(handle, keys, values);
	} catch (...) {
	}
}

void BlockerPassesPlugin::TeleportEnt(int handle, const plg::vec3& origin, const plg::vec3& angles) {
	if (!__s2sdk_TeleportEntity || !HandleValid(handle)) {
		return;
	}
	try {
		// A zero velocity is the "no velocity" of the original's nullptr (props are static).
		s2sdk::TeleportEntity(handle, origin, angles, plg::vec3{0.0f, 0.0f, 0.0f});
	} catch (...) {
	}
}

void BlockerPassesPlugin::RemoveEnt(int handle) {
	if (!__s2sdk_RemoveEntity || !HandleValid(handle)) {
		return;
	}
	try {
		s2sdk::RemoveEntity(handle);
	} catch (...) {
	}
}

void BlockerPassesPlugin::SetModelOnEnt(int handle, const char* model) {
	if (!__s2sdk_SetEntityModel || !HandleValid(handle) || model == nullptr) {
		return;
	}
	try {
		s2sdk::SetEntityModel(handle, plg::string(model));
	} catch (...) {
	}
}

int BlockerPassesPlugin::EntityIndexOf(int handle) {
	// 0 when the handle is stale or empty (pisex::EntHandleToEntIndex answers -1 for a freed
	// handle, and every caller here tests `== 0` / `!= 0`).
	const int index = pisex::EntHandleToEntIndex(handle);
	return index > 0 ? index : 0;
}

// pisex::IsSchemaFieldNetworked builds the class+field schema-key strings and takes the schema
// table's shared_mutex on every call. The fields below are constants of the plugin, so each answer
// is asked exactly once - ApplyRenderColor alone runs at 10 Hz for every beam of a rainbow wall
// (24 per wall). The schema system is initialised whenever these run (they only touch live map
// entities), so a resolved answer is final.
bool IsFieldNetworkedCached(const char* className, const char* field) {
	struct Entry {
		const char* className;
		const char* field;
		bool networked;
		bool resolved;
	};
	static Entry s_cache[] = {
		{"CBaseEntity", "m_fEffects", false, false},
		{"CBaseModelEntity", "m_clrRender", false, false},
		{"CBeam", "m_vecEndPos", false, false},
		{"CBeam", "m_fWidth", false, false},
		{"CCollisionProperty", "m_vecMins", false, false},
		{"CCollisionProperty", "m_vecMaxs", false, false},
		{"CCollisionProperty", "m_nSolidType", false, false},
		{"CCollisionProperty", "m_nSurroundType", false, false},
		{"CCollisionProperty", "m_CollisionGroup", false, false},
		{"VPhysicsCollisionAttribute_t", "m_nCollisionGroup", false, false},
	};
	for (Entry& entry : s_cache) {
		if (std::strcmp(entry.className, className) == 0 && std::strcmp(entry.field, field) == 0) {
			if (!entry.resolved) {
				entry.resolved = true;
				entry.networked = pisex::IsSchemaFieldNetworked(className, field);
			}
			return entry.networked;
		}
	}
	return pisex::IsSchemaFieldNetworked(className, field);
}

void BlockerPassesPlugin::SetStateChangedField(int handle, const char* className, const char* field) {
	const int index = EntityIndexOf(handle);
	if (index == 0) {
		return;
	}
	// s2sdk warns (and does nothing) for a non-networked field; the original never checked, so
	// the gate keeps a wrong schema flag from spamming the log.
	if (IsFieldNetworkedCached(className, field)) {
		pisex::SetStateChanged(index, className, field);
	}
}

// The CCollisionProperty of these entities is the embedded component CBaseModelEntity::m_Collision
// (func_brush and prop_dynamic are CBaseModelEntity-derived; m_Collision sits at +2192, and a
// generic CBaseEntity carries only the m_pCollision pointer at +1720). The reference wrote the
// fields through me->m_Collision(), i.e. at component + offset; the string helpers of pisexcompat
// apply the CCollisionProperty offsets to the entity base, which lands in the entity's own early
// fields instead (m_vecMins would be written at entity+64, m_nSolidType at entity+91).
void* BlockerPassesPlugin::CollisionPropertyOf(int handle) {
	if (EntityIndexOf(handle) == 0 || !__s2sdk_GetEntSchemaPtr) {
		return nullptr;
	}
	try {
		return s2sdk::GetEntSchemaPtr(handle, "CBaseModelEntity", "m_Collision", 0);
	} catch (...) {
		return nullptr;
	}
}

// Announces a changed leaf field inside the entity's CCollisionProperty. The component sits inline
// in the entity (CBaseModelEntity::m_Collision) and the entity has no chainer, so the change is
// reported at entity + m_Collision + field with chain -1 - the way WeaponSkins reports the leaf
// fields of its inline item view. pisex::SetStateChanged(index, "CCollisionProperty", field) (what
// this used to call, like the original's SetStateChanged) applied the component-relative field
// offset to the entity base and so marked an unrelated early field of the entity instead.
void AnnounceCollisionLeaf(int index, int fieldOffsetInCollision) {
	static int s_collision = -1;
	if (s_collision < 0) {
		s_collision = pisex::SchemaOffset("CBaseModelEntity", "m_Collision");
	}
	if (index == 0 || s_collision < 0 || fieldOffsetInCollision < 0 || !__s2sdk_ChangeEntityState2) {
		return;
	}
	void* entity = pisex::EntIndexToEntPointer(index);
	if (entity == nullptr) {
		return;
	}
	try {
		s2sdk::ChangeEntityState2(entity, s_collision + fieldOffsetInCollision, -1);
	} catch (...) {
	}
}

void BlockerPassesPlugin::SetCollisionInt(int handle, const char* field, int value) {
	void* property = CollisionPropertyOf(handle);
	const int offset = pisex::SchemaOffset("CCollisionProperty", field);
	if (property == nullptr || offset < 0 || !__s2sdk_SetEntData2) {
		return;
	}
	// m_nSurroundType / m_nSolidType / m_CollisionGroup are one byte each.
	try {
		s2sdk::SetEntData2(property, offset, value, /*size=*/1, /*changeState=*/false, /*chainOffset=*/-2);
	} catch (...) {
		return;
	}
	// The original always marked the field changed; only networked fields are announced.
	const int index = EntityIndexOf(handle);
	if (index != 0 && IsFieldNetworkedCached("CCollisionProperty", field)) {
		AnnounceCollisionLeaf(index, offset);
	}
}

void BlockerPassesPlugin::SetCollisionVec(int handle, const char* field, const plg::vec3& value) {
	void* property = CollisionPropertyOf(handle);
	const int offset = pisex::SchemaOffset("CCollisionProperty", field);
	if (property == nullptr || offset < 0 || !__s2sdk_SetEntDataVector3D2) {
		return;
	}
	try {
		s2sdk::SetEntDataVector3D2(property, offset, value, /*changeState=*/false, /*chainOffset=*/-2);
	} catch (...) {
		return;
	}
	const int index = EntityIndexOf(handle);
	if (index != 0 && IsFieldNetworkedCached("CCollisionProperty", field)) {
		AnnounceCollisionLeaf(index, offset);
	}
}

void BlockerPassesPlugin::SetNoDraw(int handle, bool on) {
	if (!HandleValid(handle) || !__s2sdk_GetEntityEffects) {
		return;
	}
	try {
		const int effects = static_cast<int>(s2sdk::GetEntityEffects(handle));
		const int next = on ? (effects | kEfNoDraw) : (effects & ~kEfNoDraw);
		if (next == effects) {
			return;
		}
		if (on) {
			s2sdk::AddEntityEffects(handle, kEfNoDraw);
		} else {
			s2sdk::RemoveEntityEffects(handle, kEfNoDraw);
		}
		SetStateChangedField(handle, "CBaseEntity", "m_fEffects");
	} catch (...) {
	}
}

void BlockerPassesPlugin::ApplyRenderAlpha(int handle, int alpha) {
	if (!HandleValid(handle) || !__s2sdk_GetEntityRenderColor || !__s2sdk_SetEntityRenderColor) {
		return;
	}
	try {
		const plg::vec4 current = s2sdk::GetEntityRenderColor(handle);
		if (static_cast<int>(current.w) == alpha) {
			return;
		}
		s2sdk::SetEntityRenderColor(handle, plg::vec4{current.x, current.y, current.z, static_cast<float>(alpha)});
		SetStateChangedField(handle, "CBaseModelEntity", "m_clrRender");
	} catch (...) {
	}
}

void BlockerPassesPlugin::ApplyRenderColor(int handle, int r, int g, int b) {
	if (!HandleValid(handle) || !__s2sdk_GetEntityRenderColor || !__s2sdk_SetEntityRenderColor) {
		return;
	}
	try {
		const plg::vec4 current = s2sdk::GetEntityRenderColor(handle);
		s2sdk::SetEntityRenderColor(
				handle, plg::vec4{static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), current.w});
		SetStateChangedField(handle, "CBaseModelEntity", "m_clrRender");
	} catch (...) {
	}
}

int BlockerPassesPlugin::CreateBeamLine(const plg::vec3& start, const plg::vec3& end, int r, int g, int b, float width) {
	const int ent = CreateEntity(kBeamClass);
	if (!bp::IsValidHandle(ent)) {
		Dbg("CreateBeamLine: CreateEntityByName failed");
		return -1;
	}

	char color[32];
	std::snprintf(color, sizeof(color), "%d %d %d", r, g, b);
	DispatchSpawnKv(ent, {
								{"BoltWidth", plg::any(width)},
								{"rendercolor", plg::any(plg::string(color))},
								{"renderamt", plg::any(static_cast<int32_t>(255))},
								{"life", plg::any(0.0f)},
						});
	TeleportEnt(ent, start, plg::vec3{0.0f, 0.0f, 0.0f});

	const int index = EntityIndexOf(ent);
	if (index != 0) {
		pisex::SetEntVector(index, "CBeam", "m_vecEndPos", end, false);
		SetStateChangedField(ent, "CBeam", "m_vecEndPos");
		pisex::SetEntFloat(index, "CBeam", "m_fWidth", width, false);
		SetStateChangedField(ent, "CBeam", "m_fWidth");
	}
	Dbg("CreateBeamLine: (%.0f %.0f %.0f) -> (%.0f %.0f %.0f)", start.x, start.y, start.z, end.x, end.y, end.z);
	return ent;
}

std::vector<int> BlockerPassesPlugin::DrawWireframe(const bp::BPItem& item) {
	std::vector<int> beams;
	beams.reserve(24);
	for (const auto& [from, to] : bp::WallBeamSegments(item.pos, item.pos2, item.wallYaw)) {
		int r = item.beamR, g = item.beamG, b = item.beamB;
		if (item.beamRainbow) {
			bp::HueToRGB(m_rainbowHue, r, g, b);
		}
		const int beam = CreateBeamLine(from, to, r, g, b, 1.0f);
		if (bp::IsValidHandle(beam)) {
			beams.push_back(beam);
		}
	}
	return beams;
}

void BlockerPassesPlugin::StartRainbowTimer() {
	if (m_rainbowTimerActive) {
		return;
	}
	m_rainbowTimerActive = true;
	pisex::g_pUtils->CreateTimer(0.1f, []() -> float {
		BlockerPassesPlugin& plugin = g_blockerPassesPlugin;
		plugin.m_rainbowHue += 10.0f;
		if (plugin.m_rainbowHue >= 360.0f) {
			plugin.m_rainbowHue -= 360.0f;
		}

		bool anyRainbow = false;
		for (bp::LiveEnt& live : plugin.m_live) {
			if (live.index < 0 || live.index >= static_cast<int>(plugin.m_items.size())) {
				continue;
			}
			if (!plugin.m_items[static_cast<std::size_t>(live.index)].isWall ||
				!plugin.m_items[static_cast<std::size_t>(live.index)].beamRainbow) {
				continue;
			}
			anyRainbow = true;
			int r = 0, g = 0, b = 0;
			bp::HueToRGB(plugin.m_rainbowHue, r, g, b);
			for (const int beam : live.beams) {
				plugin.ApplyRenderColor(beam, r, g, b);
			}
		}

		if (!anyRainbow) {
			plugin.m_rainbowTimerActive = false;
			return -1.0f;
		}
		return 0.1f;
	});
}

int BlockerPassesPlugin::SpawnOneCollisionBox(const plg::vec3& center, const plg::vec3& mins, const plg::vec3& maxs,
											  float yaw) {
	const int ent = CreateEntity(kCollisionBoxClass);
	if (!bp::IsValidHandle(ent)) {
		Dbg("SpawnOneCollisionBox: CreateEntityByName %s failed", kCollisionBoxClass);
		return -1;
	}

	if (__s2sdk_SetEntityRenderMode) {
		try {
			s2sdk::SetEntityRenderMode(ent, s2sdk::RenderMode::None);
		} catch (...) {
		}
	}

	// QAngle(0, yaw, 0) of the original (0 for the axis-aligned form).
	TeleportEnt(ent, center, plg::vec3{0.0f, yaw, 0.0f});
	// DispatchSpawn(ent, nullptr) of the original: no key values.
	if (__s2sdk_DispatchSpawn) {
		try {
			s2sdk::DispatchSpawn(ent);
		} catch (...) {
		}
	}

	plg::vec3 surroundMins{};
	plg::vec3 surroundMaxs{};
	bp::SurroundBounds(mins, maxs, yaw, surroundMins, surroundMaxs);

	const int index = EntityIndexOf(ent);
	if (index != 0) {
		SetCollisionInt(ent, "m_nSurroundType", kSurroundSpecified);
		SetCollisionVec(ent, "m_vecSpecifiedSurroundingMaxs", surroundMaxs);
		SetCollisionVec(ent, "m_vecSpecifiedSurroundingMins", surroundMins);
		SetCollisionVec(ent, "m_vecMins", mins);
		SetCollisionVec(ent, "m_vecMaxs", maxs);

		// m_collisionAttribute().m_nCollisionGroup() = 0 of the original: the attribute struct is
		// embedded at CCollisionProperty + m_collisionAttribute and carries m_nCollisionGroup at its
		// own offset 46 (one byte) - inside the collision component, not on the entity base.
		void* collision = CollisionPropertyOf(ent);
		const int attributeOffset = pisex::SchemaOffset("CCollisionProperty", "m_collisionAttribute");
		const int groupOffset = pisex::SchemaOffset("VPhysicsCollisionAttribute_t", "m_nCollisionGroup");
		if (__s2sdk_SetEntData2 && collision != nullptr && attributeOffset >= 0 && groupOffset >= 0) {
			try {
				s2sdk::SetEntData2(collision, attributeOffset + groupOffset, 0, 1, false, -2);
			} catch (...) {
			}
			// The original marked the whole attribute (an inline struct) changed; only its networked
			// leaf that was written is announced - announcing a container makes the engine dump the
			// send table.
			if (IsFieldNetworkedCached("VPhysicsCollisionAttribute_t", "m_nCollisionGroup")) {
				AnnounceCollisionLeaf(index, attributeOffset + groupOffset);
			}
		}

		SetCollisionInt(ent, "m_CollisionGroup", 0);
		SetCollisionInt(ent, "m_nSolidType", kSolidObb);

		pisex::SetEntColor(index, "CBaseModelEntity", "m_clrRender", plg::vec4{0.0f, 0.0f, 0.0f, 0.0f}, false);
		if (pisex::IsSchemaFieldNetworked("CBaseModelEntity", "m_clrRender")) {
			pisex::SetStateChanged(index, "CBaseModelEntity", "m_clrRender");
		}
	}

	// The deferred SetCollisionBounds + model of the original (0.0 s timer -> next frame).
	const int capturedHandle = ent;
	const plg::vec3 capturedMins = mins;
	const plg::vec3 capturedMaxs = maxs;
	pisex::g_pUtils->CreateTimer(0.0f, [capturedHandle, capturedMins, capturedMaxs]() -> float {
		BlockerPassesPlugin& plugin = g_blockerPassesPlugin;
		if (plugin.EntityIndexOf(capturedHandle) == 0) {
			return -1.0f;
		}
		plugin.SetModelOnEnt(capturedHandle,
							 "models/props/de_dust/hr_dust/dust_soccerball/dust_soccer_ball001.vmdl");
		if (plugin.m_setCollisionBounds != nullptr && __s2sdk_EntHandleToEntPointer) {
			void* entity = nullptr;
			try {
				entity = s2sdk::EntHandleToEntPointer(capturedHandle);
			} catch (...) {
				entity = nullptr;
			}
			if (entity != nullptr) {
				plugin.m_setCollisionBounds(entity, &capturedMins, &capturedMaxs);
				plugin.Dbg("SpawnOneCollisionBox: deferred SetCollisionBounds applied (OBB)");
			}
		}
		return -1.0f;
	});

	return ent;
}

std::vector<int> BlockerPassesPlugin::SpawnWallCollisions(const bp::BPItem& item) {
	std::vector<int> result;
	if (m_setCollisionBounds == nullptr) {
		Dbg("SpawnWallCollisions: SetCollisionBounds not found");
		return result;
	}

	const bp::WallBox box = bp::ComputeWallBox(item.pos, item.pos2, item.wallYaw);
	// The axis-aligned form passes no yaw, the OBB form the (raw) wall yaw - exactly like the
	// original's two SpawnOneCollisionBox() calls.
	const int ent = SpawnOneCollisionBox(box.center, box.mins, box.maxs, box.yaw);
	if (bp::IsValidHandle(ent)) {
		result.push_back(ent);
	}
	if (box.yaw == 0.0f) {
		Dbg("SpawnWallCollisions: axis-aligned yaw=%.1f box at (%.1f %.1f %.1f)", item.wallYaw, box.center.x,
			box.center.y, box.center.z);
	} else {
		Dbg("SpawnWallCollisions: OBB yaw=%.1f center(%.1f %.1f %.1f)", item.wallYaw, box.center.x, box.center.y,
			box.center.z);
	}
	return result;
}

void BlockerPassesPlugin::KillWallCollision(int handle) {
	if (!bp::IsValidHandle(handle)) {
		return;
	}
	const int index = EntityIndexOf(handle);
	if (index != 0) {
		SetCollisionInt(handle, "m_nSolidType", kSolidNone);
		SetCollisionVec(handle, "m_vecMins", plg::vec3{0.0f, 0.0f, 0.0f});
		SetCollisionVec(handle, "m_vecMaxs", plg::vec3{0.0f, 0.0f, 0.0f});
		SetCollisionVec(handle, "m_vecSpecifiedSurroundingMins", plg::vec3{0.0f, 0.0f, 0.0f});
		SetCollisionVec(handle, "m_vecSpecifiedSurroundingMaxs", plg::vec3{0.0f, 0.0f, 0.0f});
	}

	// The original called the signature with zero bounds before removing the entity.
	if (m_setCollisionBounds != nullptr && __s2sdk_EntHandleToEntPointer) {
		try {
			void* entity = s2sdk::EntHandleToEntPointer(handle);
			if (entity != nullptr) {
				const plg::vec3 zero{0.0f, 0.0f, 0.0f};
				m_setCollisionBounds(entity, &zero, &zero);
			}
		} catch (...) {
		}
	}

	TeleportEnt(handle, plg::vec3{0.0f, 0.0f, -15000.0f}, plg::vec3{0.0f, 0.0f, 0.0f});
	RemoveEnt(handle);
}

void BlockerPassesPlugin::SpawnLiveEntry(int index, bp::LiveEnt& live) {
	const bp::BPItem& item = m_items[static_cast<std::size_t>(index)];
	if (!item.isWall) {
		return;
	}
	for (const int ent : SpawnWallCollisions(item)) {
		live.wallColls.push_back(ent);
	}
	if (!live.wallColls.empty()) {
		live.ent = live.wallColls.front();
	}
	live.beams = DrawWireframe(item);
	if (item.beamRainbow) {
		StartRainbowTimer();
	}
}

int BlockerPassesPlugin::SpawnOne(int index) {
	if (index < 0 || index >= static_cast<int>(m_items.size())) {
		return -1;
	}
	const bp::BPItem& item = m_items[static_cast<std::size_t>(index)];
	if (item.path.empty()) {
		Dbg("SpawnOne: empty path");
		return -1;
	}
	if (item.path.find(".vmdl") == std::string::npos) {
		Dbg("SpawnOne: invalid model '%s'", item.path.c_str());
		return -1;
	}

	for (const char* className : kPropClasses) {
		const int ent = CreateEntity(className);
		if (!bp::IsValidHandle(ent)) {
			Dbg("SpawnOne: CreateEntityByName failed for %s", className);
			continue;
		}

		const float safeScale = bp::ClampScale(item.scale);
		DispatchSpawnKv(ent, {
									{"model", plg::any(plg::string(item.path))},
									{"solid", plg::any(static_cast<int32_t>(6))},
									{"DisableBoneFollowers", plg::any(static_cast<int32_t>(1))},
									{"uniformscale", plg::any(safeScale)},
							});
		TeleportEnt(ent, item.pos, item.ang);

		if (item.invisible) {
			ApplyRenderAlpha(ent, 0);
			SetNoDraw(ent, true);
		}
		if (item.itemR != 255 || item.itemG != 255 || item.itemB != 255) {
			ApplyRenderColor(ent, item.itemR, item.itemG, item.itemB);
		}

		Dbg("SpawnOne: %s '%s' at (%.1f %.1f %.1f) ang(%.1f %.1f %.1f) scale=%.3f invis=%d", className,
			item.path.c_str(), item.pos.x, item.pos.y, item.pos.z, item.ang.x, item.ang.y, item.ang.z, safeScale,
			static_cast<int>(item.invisible));

		// The original returned the first successful class.
		bp::LiveEnt live;
		live.index = index;
		live.ent = ent;
		m_live.push_back(std::move(live));
		return ent;
	}
	Dbg("SpawnOne: failed model '%s'", item.path.c_str());
	return -1;
}

void BlockerPassesPlugin::EnsureClosed() {
	// Every item that has no live entry yet is spawned (the original's EnsureClosed).
	std::vector<int> aliveIdx;
	aliveIdx.reserve(m_live.size());
	for (const bp::LiveEnt& live : m_live) {
		aliveIdx.push_back(live.index);
	}

	for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
		if (std::find(aliveIdx.begin(), aliveIdx.end(), i) != aliveIdx.end()) {
			continue;
		}
		if (m_items[static_cast<std::size_t>(i)].isWall) {
			bp::LiveEnt live;
			live.index = i;
			SpawnLiveEntry(i, live);
			m_live.push_back(std::move(live));
		} else if (!bp::IsValidHandle(SpawnOne(i))) {
			Dbg("EnsureClosed: spawn failed for %d", i);
		}
	}
}

void BlockerPassesPlugin::RemoveLiveBeams(bp::LiveEnt& live) {
	for (const int beam : live.beams) {
		RemoveEnt(beam);
	}
	live.beams.clear();
}

void BlockerPassesPlugin::ClearLive(bool removeEntities) {
	if (removeEntities) {
		for (bp::LiveEnt& live : m_live) {
			const bool isWall = (live.index >= 0 && live.index < static_cast<int>(m_items.size()) &&
								 m_items[static_cast<std::size_t>(live.index)].isWall);
			if (isWall) {
				for (const int collision : live.wallColls) {
					KillWallCollision(collision);
				}
				live.wallColls.clear();
			} else if (bp::IsValidHandle(live.ent)) {
				RemoveEnt(live.ent);
			}
			for (const int beam : live.beams) {
				RemoveEnt(beam);
			}
		}
	}
	m_live.clear();
	m_rainbowTimerActive = false;
}

void BlockerPassesPlugin::ApplyState() {
	if (ShouldBeOpen()) {
		ClearLive(true); // the pass is open: nothing may block it
	} else {
		EnsureClosed();
	}
}

void BlockerPassesPlugin::RespawnLive(int index) {
	const bool isWall = (index >= 0 && index < static_cast<int>(m_items.size()) &&
						 m_items[static_cast<std::size_t>(index)].isWall);
	for (auto it = m_live.begin(); it != m_live.end(); ++it) {
		if (it->index != index) {
			continue;
		}
		if (isWall) {
			for (const int collision : it->wallColls) {
				KillWallCollision(collision);
			}
			it->wallColls.clear();
		} else if (bp::IsValidHandle(it->ent)) {
			RemoveEnt(it->ent);
		}
		RemoveLiveBeams(*it);
		m_live.erase(it);
		break;
	}

	if (!ShouldBeOpen()) {
		if (isWall) {
			bp::LiveEnt live;
			live.index = index;
			SpawnLiveEntry(index, live);
			m_live.push_back(std::move(live));
		} else if (!bp::IsValidHandle(SpawnOne(index))) {
			Dbg("RespawnLive: spawn failed for %d", index);
		}
	}
}

void BlockerPassesPlugin::RespawnWallBeams(int index) {
	for (bp::LiveEnt& live : m_live) {
		if (live.index != index) {
			continue;
		}
		RemoveLiveBeams(live);
		live.beams = DrawWireframe(m_items[static_cast<std::size_t>(index)]);
		if (m_items[static_cast<std::size_t>(index)].beamRainbow) {
			StartRainbowTimer();
		}
		return;
	}
}

void BlockerPassesPlugin::TeleportLive(int index) {
	if (index < 0 || index >= static_cast<int>(m_items.size()) ||
		m_items[static_cast<std::size_t>(index)].isWall) {
		return;
	}
	for (bp::LiveEnt& live : m_live) {
		if (live.index == index && bp::IsValidHandle(live.ent)) {
			TeleportEnt(live.ent, m_items[static_cast<std::size_t>(index)].pos,
						m_items[static_cast<std::size_t>(index)].ang);
			return;
		}
	}
}

void BlockerPassesPlugin::MakeLiveIfMissing(int index) {
	if (index < 0 || index >= static_cast<int>(m_items.size())) {
		return;
	}
	for (const bp::LiveEnt& live : m_live) {
		if (live.index == index) {
			return;
		}
	}
	if (ShouldBeOpen()) {
		return;
	}
	if (m_items[static_cast<std::size_t>(index)].isWall) {
		bp::LiveEnt live;
		live.index = index;
		SpawnLiveEntry(index, live);
		m_live.push_back(std::move(live));
	} else if (!bp::IsValidHandle(SpawnOne(index))) {
		Dbg("MakeLiveIfMissing: failed for %d", index);
	}
}

void BlockerPassesPlugin::ApplyVisualScaleToLive(int index) {
	for (bp::LiveEnt& live : m_live) {
		if (live.index != index || !HandleValid(live.ent)) {
			continue;
		}
		if (!__s2sdk_SetEntityAbsScale) {
			return;
		}
		try {
			const float scale = bp::ClampScale(m_items[static_cast<std::size_t>(index)].scale);
			// m_CBodyComponent->m_pSceneNode->m_flScale of the original; SetAbsScale is the
			// engine's own setter, so it networks the new scene-node scale itself (the original
			// needed its SetStateChanged only because it wrote the field directly).
			s2sdk::SetEntityAbsScale(live.ent, scale);
			Dbg("ApplyVisualScaleToLive: idx=%d sceneNode scale=%.3f", index, scale);
			return;
		} catch (...) {
			RespawnLive(index);
			return;
		}
	}
}

void BlockerPassesPlugin::ApplyInvisibilityToLive(int index) {
	for (bp::LiveEnt& live : m_live) {
		if (live.index != index || !bp::IsValidHandle(live.ent)) {
			continue;
		}
		const bool invisible = m_items[static_cast<std::size_t>(index)].invisible;
		ApplyRenderAlpha(live.ent, invisible ? 0 : 255);
		SetNoDraw(live.ent, invisible);
		if (!invisible) {
			ApplyRenderColor(live.ent, m_items[static_cast<std::size_t>(index)].itemR,
							 m_items[static_cast<std::size_t>(index)].itemG,
							 m_items[static_cast<std::size_t>(index)].itemB);
		}
		Dbg("ApplyInvisibilityToLive: idx=%d invisible=%d", index, static_cast<int>(invisible));
		return;
	}
}

void BlockerPassesPlugin::ApplyItemColorToLive(int index) {
	for (bp::LiveEnt& live : m_live) {
		if (live.index != index || !bp::IsValidHandle(live.ent)) {
			continue;
		}
		ApplyRenderColor(live.ent, m_items[static_cast<std::size_t>(index)].itemR,
						 m_items[static_cast<std::size_t>(index)].itemG,
						 m_items[static_cast<std::size_t>(index)].itemB);
		return;
	}
}

// --- the data file ------------------------------------------------------------------------------

void BlockerPassesPlugin::SaveData() {
	const std::string path = pisex::ResolveDataPath(kDataFile);
	if (path.empty()) {
		return;
	}
	std::error_code error;
	const std::filesystem::path filePath(path);
	if (filePath.has_parent_path()) {
		std::filesystem::create_directories(filePath.parent_path(), error);
	}

	// The original loaded the whole file, removed the current map's section and saved everything
	// back; WriteMapSection copies the other maps over, so they survive.
	pisex::KeyValues root("BPData");
	root.LoadFromFile(kDataFile);
	bp::WriteMapSection(root, m_currentMap, m_items);
	const std::string text = bp::SerializeKeyValues(root);

	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file) {
		LogError(std::string(kLogTag) + " cannot write " + path);
		return;
	}
	file << text;
	file.close();
	Dbg("Saved %d items for map %s", static_cast<int>(m_items.size()), m_currentMap.c_str());
}

void BlockerPassesPlugin::LoadDataForMap(const char* map) {
	if (map != nullptr && *map != '\0') {
		m_currentMap = bp::NormalizeMapName(map);
	}

	m_items.clear();
	ClearLive(true);

	pisex::KeyValues root("BPData");
	if (!root.LoadFromFile(kDataFile)) {
		Dbg("No data file yet for map %s", m_currentMap.c_str());
		return;
	}
	m_items = bp::ReadMapSection(root, m_currentMap);
	Dbg("Loaded %d items for map %s", static_cast<int>(m_items.size()), m_currentMap.c_str());
}

void BlockerPassesPlugin::EnsureCorrectMapLoaded() {
	const std::string_view raw = pisex::CurrentMap();
	if (raw.empty() || !bp::IsValidMapName(std::string(raw).c_str())) {
		Dbg("EnsureCorrectMapLoaded: invalid/corrupted map name, keeping '%s'", m_currentMap.c_str());
		return;
	}
	const std::string realMap = bp::NormalizeMapName(std::string(raw).c_str());
	if (realMap.empty()) {
		return;
	}
	if (m_currentMap != realMap) {
		Dbg("EnsureCorrectMapLoaded: map changed: '%s' -> '%s'", m_currentMap.c_str(), realMap.c_str());
		m_tempAccessSteamIds.clear();
		LoadSettings();
		LoadPhrases();
		LoadDataForMap(realMap.c_str());
	}
}

// --- map / round / ping events --------------------------------------------------------------------

void BlockerPassesPlugin::OnMapStart(const char* map) {
	m_tempAccessSteamIds.clear();
	LoadSettings();
	LoadPhrases();

	const std::string_view current = pisex::CurrentMap();
	std::string realMap;
	if (!current.empty() && bp::IsValidMapName(std::string(current).c_str())) {
		realMap = std::string(current);
	} else if (map != nullptr && bp::IsValidMapName(map)) {
		realMap = map;
	}
	if (realMap.empty()) {
		Dbg("OnMapStart: no valid map name from either source, keeping '%s'", m_currentMap.c_str());
		return;
	}
	Dbg("OnMapStart: raw='%s' globals='%s'", map != nullptr ? map : "(null)", realMap.c_str());
	LoadDataForMap(realMap.c_str());
}

void BlockerPassesPlugin::OnMapEnd() {
	ClearLive(true);
}

void BlockerPassesPlugin::OnRoundStart() {
	ClearLive(true);
	EnsureCorrectMapLoaded();
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		m_pingMode[static_cast<std::size_t>(slot)] = PING_NONE;
		m_pingTargetIndex[static_cast<std::size_t>(slot)] = -1;
	}
	pisex::g_pUtils->CreateTimer(0.10f, []() -> float {
		g_blockerPassesPlugin.ApplyState();
		return -1.0f;
	});
	if (!ShouldBeOpen()) {
		PrintChatAllKey("Chat_ClosedMsg", "{RED}[BP]{DEFAULT} Проход закрыт. Откроется при {RED}%d{DEFAULT} игроках.",
						m_settings.minPlayersToOpen);
	}
}

void BlockerPassesPlugin::OnPlayerPing(const pisex::IGameEvent& event) {
	const int slot = event.GetPlayerSlot("userid");
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	if (m_pingMode[static_cast<std::size_t>(slot)] == PING_NONE) {
		return;
	}

	const plg::vec3 pingPos{event.GetFloat("x"), event.GetFloat("y"), event.GetFloat("z")};

	if (m_pingMode[static_cast<std::size_t>(slot)] == PING_TELEPORT) {
		const int index = m_pingTargetIndex[static_cast<std::size_t>(slot)];
		if (index < 0 || index >= static_cast<int>(m_items.size())) {
			m_pingMode[static_cast<std::size_t>(slot)] = PING_NONE;
			return;
		}
		if (m_items[static_cast<std::size_t>(index)].isWall) {
			bp::BPItem& item = m_items[static_cast<std::size_t>(index)];
			const plg::vec3 center{(item.pos.x + item.pos2.x) * 0.5f, (item.pos.y + item.pos2.y) * 0.5f,
								   (item.pos.z + item.pos2.z) * 0.5f};
			const plg::vec3 offset{pingPos.x - center.x, pingPos.y - center.y, pingPos.z - center.z};
			item.pos = plg::vec3{item.pos.x + offset.x, item.pos.y + offset.y, item.pos.z + offset.z};
			item.pos2 = plg::vec3{item.pos2.x + offset.x, item.pos2.y + offset.y, item.pos2.z + offset.z};
			RespawnLive(index);
		} else {
			m_items[static_cast<std::size_t>(index)].pos = pingPos;
			TeleportLive(index);
			MakeLiveIfMissing(index);
		}
		SaveData();
		m_pingMode[static_cast<std::size_t>(slot)] = PING_NONE;
		OpenItemMenu(slot, index);
		return;
	}

	if (m_pingMode[static_cast<std::size_t>(slot)] == PING_WALL_POS1) {
		m_wallTempPos[static_cast<std::size_t>(slot)] = pingPos;
		m_pingMode[static_cast<std::size_t>(slot)] = PING_WALL_POS2;
		PrintChatKey(slot, "Chat_WallPos2", "Теперь поставьте вторую точку пингом (колёсико мышки)");
		return;
	}

	if (m_pingMode[static_cast<std::size_t>(slot)] == PING_WALL_POS2) {
		m_pingMode[static_cast<std::size_t>(slot)] = PING_NONE;

		bp::BPItem item;
		item.label = "Стена";
		item.path = "";
		item.isWall = true;
		item.pos = m_wallTempPos[static_cast<std::size_t>(slot)];
		item.pos2 = pingPos;
		item.scale = 1.0f;
		item.invisible = false;

		const int newIndex = static_cast<int>(m_items.size());
		m_items.push_back(item);
		SaveData();

		bp::LiveEnt live;
		live.index = newIndex;
		SpawnLiveEntry(newIndex, live);
		m_live.push_back(std::move(live));

		PrintChatKey(slot, "Chat_WallCreated", "Стена создана!");
		OpenItemMenu(slot, newIndex);
		return;
	}
}

// --- menus --------------------------------------------------------------------------------------

void BlockerPassesPlugin::OpenMainMenu(int iSlot) {
	pisex::Menu menu;
	menu.clear();
	pisex::SetTitleMenu(menu, Phrase("Menu_Title", "BlockerPasses"));
	pisex::AddItemMenu(menu, "place", Phrase("Menu_Place", "Поставить предмет"), pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "wall", Phrase("Menu_Wall", "Создать стену (beam)"), pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "edit", Phrase("Menu_Edit", "Редактировать предметы"), pisex::ITEM_DEFAULT);
	pisex::SetExitMenu(menu, true);
	pisex::SetCallback(menu, [](const char* back, const char*, int, int iSlot) {
		if (back == nullptr) {
			return;
		}
		BlockerPassesPlugin& plugin = g_blockerPassesPlugin;
		if (std::strcmp(back, "place") == 0) {
			plugin.OpenModelMenu(iSlot);
		} else if (std::strcmp(back, "wall") == 0) {
			plugin.m_pingMode[static_cast<std::size_t>(iSlot)] = PING_WALL_POS1;
			plugin.PrintChatKey(iSlot, "Chat_WallPos1", "Поставьте первую точку пингом (колёсико мышки)");
			pisex::ClosePlayerMenu(iSlot);
		} else if (std::strcmp(back, "edit") == 0) {
			const int idx = plugin.FindItemByCrosshair(iSlot, 128.0f);
			if (idx >= 0) {
				plugin.MakeLiveIfMissing(idx);
				plugin.OpenItemMenu(iSlot, idx);
			} else {
				plugin.OpenEditListMenu(iSlot);
			}
		}
	});
	pisex::DisplayPlayerMenu(menu, iSlot, true, true);
}

void BlockerPassesPlugin::OpenModelMenu(int iSlot) {
	pisex::Menu menu;
	menu.clear();
	pisex::SetTitleMenu(menu, Phrase("Menu_ModelTitle", "Выбор модели"));
	if (m_settings.models.empty()) {
		pisex::AddItemMenu(menu, "none", Phrase("Menu_NoModels", "Нет моделей"), pisex::ITEM_DISABLED);
	} else {
		for (std::size_t i = 0; i < m_settings.models.size(); ++i) {
			char key[64];
			std::snprintf(key, sizeof(key), "m:%zu", i);
			pisex::AddItemMenu(menu, key, m_settings.models[i].label.c_str(), pisex::ITEM_DEFAULT);
		}
	}
	pisex::SetBackMenu(menu, true);
	pisex::SetExitMenu(menu, true);
	pisex::SetCallback(menu, [](const char* back, const char*, int, int iSlot) {
		if (back == nullptr) {
			return;
		}
		BlockerPassesPlugin& plugin = g_blockerPassesPlugin;
		if (std::strcmp(back, "back") == 0) {
			plugin.OpenMainMenu(iSlot);
			return;
		}
		if (std::strncmp(back, "m:", 2) != 0) {
			return;
		}
		const int idx = std::atoi(back + 2);
		if (idx < 0 || idx >= static_cast<int>(plugin.m_settings.models.size())) {
			return;
		}

		plg::vec3 aim{};
		if (!plugin.AimPoint(iSlot, aim)) {
			return;
		}

		bp::BPItem item;
		item.label = plugin.m_settings.models[static_cast<std::size_t>(idx)].label;
		item.path = plugin.m_settings.models[static_cast<std::size_t>(idx)].path;
		item.pos = aim;
		item.ang = plg::vec3{0.0f, 0.0f, 0.0f};
		item.scale = 1.0f;
		item.invisible = false;

		const int newIndex = static_cast<int>(plugin.m_items.size());
		plugin.m_items.push_back(item);
		plugin.SaveData();

		plugin.MakeLiveIfMissing(newIndex);
		plugin.OpenItemMenu(iSlot, newIndex);
	});
	pisex::DisplayPlayerMenu(menu, iSlot, true, true);
}

void BlockerPassesPlugin::OpenEditListMenu(int iSlot) {
	pisex::Menu menu;
	menu.clear();
	pisex::SetTitleMenu(menu, Phrase("Menu_EditPick", "Выбери предмет"));
	if (m_items.empty()) {
		pisex::AddItemMenu(menu, "none", Phrase("Menu_NoItems", "Нет предметов"), pisex::ITEM_DISABLED);
	} else {
		for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
			char key[64];
			std::snprintf(key, sizeof(key), "e:%d", i);
			std::string title =
					m_items[static_cast<std::size_t>(i)].label.empty() ? m_items[static_cast<std::size_t>(i)].path
																	   : m_items[static_cast<std::size_t>(i)].label;
			if (m_items[static_cast<std::size_t>(i)].isWall) {
				title += " [wall]";
			}
			pisex::AddItemMenu(menu, key, title.c_str(), pisex::ITEM_DEFAULT);
		}
	}
	pisex::SetBackMenu(menu, true);
	pisex::SetExitMenu(menu, true);
	pisex::SetCallback(menu, [](const char* back, const char*, int, int iSlot) {
		if (back == nullptr) {
			return;
		}
		BlockerPassesPlugin& plugin = g_blockerPassesPlugin;
		if (std::strcmp(back, "back") == 0) {
			plugin.OpenMainMenu(iSlot);
			return;
		}
		if (std::strncmp(back, "e:", 2) != 0) {
			return;
		}
		const int idx = std::atoi(back + 2);
		if (idx < 0 || idx >= static_cast<int>(plugin.m_items.size())) {
			return;
		}
		plugin.MakeLiveIfMissing(idx);
		plugin.OpenItemMenu(iSlot, idx);
	});
	pisex::DisplayPlayerMenu(menu, iSlot, true, true);
}

void BlockerPassesPlugin::OpenItemMenu(int iSlot, int index) {
	if (index < 0 || index >= static_cast<int>(m_items.size())) {
		return;
	}
	pisex::Menu menu;
	menu.clear();

	const bool isWall = m_items[static_cast<std::size_t>(index)].isWall;

	char title[256];
	std::snprintf(title, sizeof(title), "%s%s", m_items[static_cast<std::size_t>(index)].label.c_str(),
				  isWall ? " [wall]" : "");
	pisex::SetTitleMenu(menu, title);

	if (isWall) {
		pisex::AddItemMenu(menu, "wallmove", Phrase("Menu_Move", "Двигать"), pisex::ITEM_DEFAULT);
		pisex::AddItemMenu(menu, "wallscale", Phrase("Menu_WallScale", "Размер стены"), pisex::ITEM_DEFAULT);
		pisex::AddItemMenu(menu, "wallrotate", Phrase("Menu_WallRotate", "Поворот стены"), pisex::ITEM_DEFAULT);
		pisex::AddItemMenu(menu, "beamcolor", Phrase("Menu_BeamColor", "Цвет лазера"), pisex::ITEM_DEFAULT);
		pisex::AddItemMenu(menu, "wall:trace", Phrase("Menu_MoveTrace", "Перенести в точку прицела"),
						   pisex::ITEM_DEFAULT);
		pisex::AddItemMenu(menu, "wallping", Phrase("Menu_PingMove", "Телепортировать пингом"), pisex::ITEM_DEFAULT);
	} else {
		pisex::AddItemMenu(menu, "move", Phrase("Menu_Move", "Двигать"), pisex::ITEM_DEFAULT);
		pisex::AddItemMenu(menu, "rotate", Phrase("Menu_Rotate", "Поворачивать"), pisex::ITEM_DEFAULT);
		pisex::AddItemMenu(menu, "scale", Phrase("Menu_Scale", "Масштаб"), pisex::ITEM_DEFAULT);
	}

	pisex::AddItemMenu(menu, "teleport", Phrase("Menu_Teleport", "Телепортироваться"), pisex::ITEM_DEFAULT);

	if (!isWall) {
		pisex::AddItemMenu(menu, "itemcolor", Phrase("Menu_ItemColor", "Цвет предмета"), pisex::ITEM_DEFAULT);
		pisex::AddItemMenu(menu, "ping", Phrase("Menu_PingMove", "Телепортировать пингом"), pisex::ITEM_DEFAULT);
		pisex::AddItemMenu(menu, "move:trace", Phrase("Menu_MoveTrace", "Перенести в точку прицела"),
						   pisex::ITEM_DEFAULT);

		if (m_items[static_cast<std::size_t>(index)].invisible) {
			pisex::AddItemMenu(menu, "invis:off", Phrase("Menu_InvisOff", "Показать"), pisex::ITEM_DEFAULT);
		} else {
			pisex::AddItemMenu(menu, "invis:on", Phrase("Menu_InvisOn", "Скрыть"), pisex::ITEM_DEFAULT);
		}
	}

	pisex::AddItemMenu(menu, "delete", Phrase("Menu_Delete", "Удалить"), pisex::ITEM_DEFAULT);

	pisex::SetBackMenu(menu, true);
	pisex::SetExitMenu(menu, true);
	pisex::SetCallback(menu, [index](const char* back, const char*, int, int iSlot) {
		if (back == nullptr) {
			return;
		}
		BlockerPassesPlugin& plugin = g_blockerPassesPlugin;
		if (std::strcmp(back, "back") == 0) {
			plugin.OpenEditListMenu(iSlot);
			return;
		}
		// The captured index can go stale while this menu is open: another admin can delete
		// items, and a map change reloads (clears) the list. Every branch below indexes
		// m_items[index], so a stale index must end the callback instead of the vector access.
		if (index < 0 || index >= static_cast<int>(plugin.m_items.size())) {
			return;
		}
		if (std::strcmp(back, "move") == 0) {
			plugin.OpenMoveMenu(iSlot, index);
			return;
		}
		if (std::strcmp(back, "rotate") == 0) {
			plugin.OpenRotateMenu(iSlot, index);
			return;
		}
		if (std::strcmp(back, "scale") == 0) {
			plugin.OpenScaleMenu(iSlot, index);
			return;
		}
		if (std::strcmp(back, "wallmove") == 0) {
			plugin.OpenWallMoveMenu(iSlot, index);
			return;
		}
		if (std::strcmp(back, "wallscale") == 0) {
			plugin.OpenWallScaleMenu(iSlot, index);
			return;
		}
		if (std::strcmp(back, "beamcolor") == 0) {
			plugin.OpenBeamColorMenu(iSlot, index);
			return;
		}
		if (std::strcmp(back, "wallrotate") == 0) {
			plugin.OpenWallRotateMenu(iSlot, index);
			return;
		}
		if (std::strcmp(back, "wall:trace") == 0) {
			plg::vec3 aim{};
			if (!plugin.AimPoint(iSlot, aim)) {
				return;
			}
			bp::BPItem& item = plugin.m_items[static_cast<std::size_t>(index)];
			const plg::vec3 center{(item.pos.x + item.pos2.x) * 0.5f, (item.pos.y + item.pos2.y) * 0.5f,
								   (item.pos.z + item.pos2.z) * 0.5f};
			const plg::vec3 offset{aim.x - center.x, aim.y - center.y, aim.z - center.z};
			item.pos = plg::vec3{item.pos.x + offset.x, item.pos.y + offset.y, item.pos.z + offset.z};
			item.pos2 = plg::vec3{item.pos2.x + offset.x, item.pos2.y + offset.y, item.pos2.z + offset.z};
			plugin.RespawnLive(index);
			plugin.SaveData();
			plugin.OpenItemMenu(iSlot, index);
			return;
		}
		if (std::strcmp(back, "wallping") == 0 || std::strcmp(back, "ping") == 0) {
			plugin.m_pingMode[static_cast<std::size_t>(iSlot)] = PING_TELEPORT;
			plugin.m_pingTargetIndex[static_cast<std::size_t>(iSlot)] = index;
			plugin.PrintChatKey(iSlot, "Chat_UsePing", "Выберите место с помощью пинга (колёсико мышки)");
			pisex::ClosePlayerMenu(iSlot);
			return;
		}
		if (std::strcmp(back, "itemcolor") == 0) {
			plugin.OpenItemColorMenu(iSlot, index);
			return;
		}
		if (std::strcmp(back, "teleport") == 0) {
			if (!plugin.PlayerAlive(iSlot)) {
				plugin.PrintChatKey(iSlot, "Chat_MustBeAlive", "Для этого вы должны быть живы");
				return;
			}
			const bp::BPItem& item = plugin.m_items[static_cast<std::size_t>(index)];
			pisex::Teleport(iSlot, &item.pos, &item.ang, nullptr);
			plugin.OpenItemMenu(iSlot, index);
			return;
		}
		if (std::strcmp(back, "move:trace") == 0) {
			plg::vec3 aim{};
			if (!plugin.AimPoint(iSlot, aim)) {
				return;
			}
			plugin.m_items[static_cast<std::size_t>(index)].pos = aim;
			plugin.TeleportLive(index);
			plugin.MakeLiveIfMissing(index);
			plugin.SaveData();
			plugin.OpenItemMenu(iSlot, index);
			return;
		}
		if (std::strcmp(back, "invis:on") == 0) {
			plugin.m_items[static_cast<std::size_t>(index)].invisible = true;
			plugin.ApplyInvisibilityToLive(index);
			plugin.SaveData();
			plugin.OpenItemMenu(iSlot, index);
			return;
		}
		if (std::strcmp(back, "invis:off") == 0) {
			plugin.m_items[static_cast<std::size_t>(index)].invisible = false;
			plugin.ApplyInvisibilityToLive(index);
			plugin.SaveData();
			plugin.OpenItemMenu(iSlot, index);
			return;
		}
		if (std::strcmp(back, "delete") == 0) {
			const bool isWallDel = (index >= 0 && index < static_cast<int>(plugin.m_items.size()) &&
									plugin.m_items[static_cast<std::size_t>(index)].isWall);
			for (auto it = plugin.m_live.begin(); it != plugin.m_live.end();) {
				if (it->index == index) {
					if (isWallDel) {
						for (const int collision : it->wallColls) {
							plugin.KillWallCollision(collision);
						}
						it->wallColls.clear();
					} else if (bp::IsValidHandle(it->ent)) {
						plugin.RemoveEnt(it->ent);
					}
					plugin.RemoveLiveBeams(*it);
					it = plugin.m_live.erase(it);
				} else {
					++it;
				}
			}
			plugin.m_items.erase(plugin.m_items.begin() + index);
			for (bp::LiveEnt& live : plugin.m_live) {
				if (live.index > index) {
					live.index--;
				}
			}
			plugin.SaveData();
			plugin.OpenEditListMenu(iSlot);
			return;
		}
	});
	pisex::DisplayPlayerMenu(menu, iSlot, true, true);
}

void BlockerPassesPlugin::OpenMoveMenu(int iSlot, int index) {
	if (index < 0 || index >= static_cast<int>(m_items.size())) {
		return;
	}
	pisex::Menu menu;
	menu.clear();
	pisex::SetTitleMenu(menu, Phrase("Menu_MoveTitle", "Движение"));
	pisex::AddItemMenu(menu, "x;10", "По оси X +10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "x;-10", "По оси X -10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "y;10", "По оси Y +10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "y;-10", "По оси Y -10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "z;10", "По оси Z +10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "z;-10", "По оси Z -10", pisex::ITEM_DEFAULT);
	pisex::SetBackMenu(menu, true);
	pisex::SetExitMenu(menu, true);
	pisex::SetCallback(menu, [index](const char* back, const char*, int, int iSlot) {
		if (back == nullptr) {
			return;
		}
		BlockerPassesPlugin& plugin = g_blockerPassesPlugin;
		if (std::strcmp(back, "back") == 0) {
			plugin.OpenItemMenu(iSlot, index);
			return;
		}
		if (index < 0 || index >= static_cast<int>(plugin.m_items.size())) {
			return;
		}
		plg::vec3& pos = plugin.m_items[static_cast<std::size_t>(index)].pos;
		if (std::strcmp(back, "x;10") == 0) {
			pos.x += 10;
		} else if (std::strcmp(back, "x;-10") == 0) {
			pos.x -= 10;
		} else if (std::strcmp(back, "y;10") == 0) {
			pos.y += 10;
		} else if (std::strcmp(back, "y;-10") == 0) {
			pos.y -= 10;
		} else if (std::strcmp(back, "z;10") == 0) {
			pos.z += 10;
		} else if (std::strcmp(back, "z;-10") == 0) {
			pos.z -= 10;
		} else {
			return;
		}
		plugin.TeleportLive(index);
		plugin.MakeLiveIfMissing(index);
		plugin.SaveData();
	});
	pisex::DisplayPlayerMenu(menu, iSlot, true, true);
}

void BlockerPassesPlugin::OpenRotateMenu(int iSlot, int index) {
	if (index < 0 || index >= static_cast<int>(m_items.size())) {
		return;
	}
	pisex::Menu menu;
	menu.clear();
	pisex::SetTitleMenu(menu, Phrase("Menu_RotateTitle", "Поворот"));
	pisex::AddItemMenu(menu, "x;10", "По оси X +10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "x;-10", "По оси X -10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "y;10", "По оси Y +10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "y;-10", "По оси Y -10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "z;10", "По оси Z +10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "z;-10", "По оси Z -10", pisex::ITEM_DEFAULT);
	pisex::SetBackMenu(menu, true);
	pisex::SetExitMenu(menu, true);
	pisex::SetCallback(menu, [index](const char* back, const char*, int, int iSlot) {
		if (back == nullptr) {
			return;
		}
		BlockerPassesPlugin& plugin = g_blockerPassesPlugin;
		if (std::strcmp(back, "back") == 0) {
			plugin.OpenItemMenu(iSlot, index);
			return;
		}
		if (index < 0 || index >= static_cast<int>(plugin.m_items.size())) {
			return;
		}
		plg::vec3& ang = plugin.m_items[static_cast<std::size_t>(index)].ang;
		if (std::strcmp(back, "x;10") == 0) {
			ang.x += 10;
		} else if (std::strcmp(back, "x;-10") == 0) {
			ang.x -= 10;
		} else if (std::strcmp(back, "y;10") == 0) {
			ang.y += 10;
		} else if (std::strcmp(back, "y;-10") == 0) {
			ang.y -= 10;
		} else if (std::strcmp(back, "z;10") == 0) {
			ang.z += 10;
		} else if (std::strcmp(back, "z;-10") == 0) {
			ang.z -= 10;
		} else {
			return;
		}
		plugin.TeleportLive(index);
		plugin.MakeLiveIfMissing(index);
		plugin.SaveData();
	});
	pisex::DisplayPlayerMenu(menu, iSlot, true, true);
}

void BlockerPassesPlugin::OpenScaleMenu(int iSlot, int index) {
	if (index < 0 || index >= static_cast<int>(m_items.size())) {
		return;
	}
	pisex::Menu menu;
	menu.clear();

	char title[128];
	std::snprintf(title, sizeof(title), "%s {%.2fx}", Phrase("Menu_ScaleTitle", "Масштаб"),
				  m_items[static_cast<std::size_t>(index)].scale);
	pisex::SetTitleMenu(menu, title);

	pisex::AddItemMenu(menu, "0.1", "Увеличить +0.1", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "-0.1", "Уменьшить -0.1", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "0.5", "Увеличить +0.5", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "-0.5", "Уменьшить -0.5", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "1.0", "Увеличить +1.0", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "-1.0", "Уменьшить -1.0", pisex::ITEM_DEFAULT);
	pisex::SetBackMenu(menu, true);
	pisex::SetExitMenu(menu, true);
	pisex::SetCallback(menu, [index](const char* back, const char*, int, int iSlot) {
		if (back == nullptr) {
			return;
		}
		BlockerPassesPlugin& plugin = g_blockerPassesPlugin;
		if (std::strcmp(back, "back") == 0) {
			plugin.OpenItemMenu(iSlot, index);
			return;
		}
		if (index < 0 || index >= static_cast<int>(plugin.m_items.size())) {
			return;
		}
		const float delta = static_cast<float>(std::atof(back));
		plugin.m_items[static_cast<std::size_t>(index)].scale =
				bp::ClampScale(plugin.m_items[static_cast<std::size_t>(index)].scale + delta);
		plugin.ApplyVisualScaleToLive(index);
		plugin.SaveData();
		plugin.OpenScaleMenu(iSlot, index);
	});
	pisex::DisplayPlayerMenu(menu, iSlot, true, true);
}

void BlockerPassesPlugin::OpenItemColorMenu(int iSlot, int index) {
	if (index < 0 || index >= static_cast<int>(m_items.size())) {
		return;
	}
	pisex::Menu menu;
	menu.clear();
	pisex::SetTitleMenu(menu, Phrase("Menu_ItemColorTitle", "Цвет предмета"));
	pisex::AddItemMenu(menu, "c:255:255:255", "Белый (по умолчанию)", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "c:255:0:0", "Красный", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "c:0:255:0", "Зелёный", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "c:0:128:255", "Голубой", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "c:255:255:0", "Жёлтый", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "c:255:0:255", "Розовый", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "c:0:0:0", "Чёрный", pisex::ITEM_DEFAULT);
	pisex::SetBackMenu(menu, true);
	pisex::SetExitMenu(menu, true);
	pisex::SetCallback(menu, [index](const char* back, const char*, int, int iSlot) {
		if (back == nullptr) {
			return;
		}
		BlockerPassesPlugin& plugin = g_blockerPassesPlugin;
		if (std::strcmp(back, "back") == 0) {
			plugin.OpenItemMenu(iSlot, index);
			return;
		}
		if (index < 0 || index >= static_cast<int>(plugin.m_items.size())) {
			return;
		}
		if (std::strncmp(back, "c:", 2) == 0) {
			int r = 255, g = 255, b = 255;
			std::sscanf(back, "c:%d:%d:%d", &r, &g, &b);
			bp::BPItem& item = plugin.m_items[static_cast<std::size_t>(index)];
			item.itemR = r;
			item.itemG = g;
			item.itemB = b;
			plugin.ApplyItemColorToLive(index);
			plugin.SaveData();
		}
		plugin.OpenItemMenu(iSlot, index);
	});
	pisex::DisplayPlayerMenu(menu, iSlot, true, true);
}

void BlockerPassesPlugin::OpenBeamColorMenu(int iSlot, int index) {
	if (index < 0 || index >= static_cast<int>(m_items.size())) {
		return;
	}
	pisex::Menu menu;
	menu.clear();
	pisex::SetTitleMenu(menu, Phrase("Menu_BeamColorTitle", "Цвет лазера"));
	pisex::AddItemMenu(menu, "c:255:0:0", "Красный", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "c:0:255:0", "Зелёный", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "c:0:128:255", "Голубой", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "c:255:255:0", "Жёлтый", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "c:255:0:255", "Розовый", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "c:255:255:255", "Белый", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "rainbow", "Разноцветный", pisex::ITEM_DEFAULT);
	pisex::SetBackMenu(menu, true);
	pisex::SetExitMenu(menu, true);
	pisex::SetCallback(menu, [index](const char* back, const char*, int, int iSlot) {
		if (back == nullptr) {
			return;
		}
		BlockerPassesPlugin& plugin = g_blockerPassesPlugin;
		if (std::strcmp(back, "back") == 0) {
			plugin.OpenItemMenu(iSlot, index);
			return;
		}
		if (index < 0 || index >= static_cast<int>(plugin.m_items.size())) {
			return;
		}
		bp::BPItem& item = plugin.m_items[static_cast<std::size_t>(index)];
		if (std::strcmp(back, "rainbow") == 0) {
			item.beamRainbow = true;
		} else if (std::strncmp(back, "c:", 2) == 0) {
			int r = 0, g = 128, b = 255;
			std::sscanf(back, "c:%d:%d:%d", &r, &g, &b);
			item.beamR = r;
			item.beamG = g;
			item.beamB = b;
			item.beamRainbow = false;
		} else {
			return;
		}
		plugin.RespawnWallBeams(index);
		plugin.SaveData();
		plugin.OpenItemMenu(iSlot, index);
	});
	pisex::DisplayPlayerMenu(menu, iSlot, true, true);
}

void BlockerPassesPlugin::OpenWallMoveMenu(int iSlot, int index) {
	if (index < 0 || index >= static_cast<int>(m_items.size())) {
		return;
	}
	pisex::Menu menu;
	menu.clear();
	pisex::SetTitleMenu(menu, Phrase("Menu_MoveTitle", "Движение"));
	pisex::AddItemMenu(menu, "x;10", "По оси X +10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "x;-10", "По оси X -10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "y;10", "По оси Y +10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "y;-10", "По оси Y -10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "z;10", "По оси Z +10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "z;-10", "По оси Z -10", pisex::ITEM_DEFAULT);
	pisex::SetBackMenu(menu, true);
	pisex::SetExitMenu(menu, true);
	pisex::SetCallback(menu, [index](const char* back, const char*, int, int iSlot) {
		if (back == nullptr) {
			return;
		}
		BlockerPassesPlugin& plugin = g_blockerPassesPlugin;
		if (std::strcmp(back, "back") == 0) {
			plugin.OpenItemMenu(iSlot, index);
			return;
		}
		if (index < 0 || index >= static_cast<int>(plugin.m_items.size())) {
			return;
		}
		float dx = 0, dy = 0, dz = 0;
		if (std::strcmp(back, "x;10") == 0) dx = 10;
		else if (std::strcmp(back, "x;-10") == 0) dx = -10;
		else if (std::strcmp(back, "y;10") == 0) dy = 10;
		else if (std::strcmp(back, "y;-10") == 0) dy = -10;
		else if (std::strcmp(back, "z;10") == 0) dz = 10;
		else if (std::strcmp(back, "z;-10") == 0) dz = -10;
		else return;

		bp::BPItem& item = plugin.m_items[static_cast<std::size_t>(index)];
		item.pos = plg::vec3{item.pos.x + dx, item.pos.y + dy, item.pos.z + dz};
		item.pos2 = plg::vec3{item.pos2.x + dx, item.pos2.y + dy, item.pos2.z + dz};

		plugin.RespawnLive(index);
		plugin.SaveData();
	});
	pisex::DisplayPlayerMenu(menu, iSlot, true, true);
}

void BlockerPassesPlugin::OpenWallScaleMenu(int iSlot, int index) {
	if (index < 0 || index >= static_cast<int>(m_items.size())) {
		return;
	}
	pisex::Menu menu;
	menu.clear();

	const bp::BPItem& item = m_items[static_cast<std::size_t>(index)];
	const float sizeX = std::fabs(item.pos2.x - item.pos.x);
	const float sizeY = std::fabs(item.pos2.y - item.pos.y);
	const float sizeZ = std::fabs(item.pos2.z - item.pos.z);

	char title[128];
	std::snprintf(title, sizeof(title), "%s {%.0fx%.0fx%.0f}", Phrase("Menu_WallScaleTitle", "Размер стены"), sizeX,
				  sizeY, sizeZ);
	pisex::SetTitleMenu(menu, title);

	pisex::AddItemMenu(menu, "s;10", "Увеличить +10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "s;-10", "Уменьшить -10", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "s;50", "Увеличить +50", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "s;-50", "Уменьшить -50", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "s;100", "Увеличить +100", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "s;-100", "Уменьшить -100", pisex::ITEM_DEFAULT);
	pisex::SetBackMenu(menu, true);
	pisex::SetExitMenu(menu, true);
	pisex::SetCallback(menu, [index](const char* back, const char*, int, int iSlot) {
		if (back == nullptr) {
			return;
		}
		BlockerPassesPlugin& plugin = g_blockerPassesPlugin;
		if (std::strcmp(back, "back") == 0) {
			plugin.OpenItemMenu(iSlot, index);
			return;
		}
		if (index < 0 || index >= static_cast<int>(plugin.m_items.size())) {
			return;
		}
		float delta = 0.0f;
		if (std::sscanf(back, "s;%f", &delta) != 1) {
			return;
		}
		bp::ScaleWall(plugin.m_items[static_cast<std::size_t>(index)], delta);
		plugin.RespawnLive(index);
		plugin.SaveData();
		plugin.OpenWallScaleMenu(iSlot, index);
	});
	pisex::DisplayPlayerMenu(menu, iSlot, true, true);
}

void BlockerPassesPlugin::OpenWallRotateMenu(int iSlot, int index) {
	if (index < 0 || index >= static_cast<int>(m_items.size())) {
		return;
	}
	pisex::Menu menu;
	menu.clear();

	char title[128];
	std::snprintf(title, sizeof(title), "%s {%.0f°}", Phrase("Menu_WallRotateTitle", "Поворот стены"),
				  m_items[static_cast<std::size_t>(index)].wallYaw);
	pisex::SetTitleMenu(menu, title);

	pisex::AddItemMenu(menu, "r;45", "+45°", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "r;-45", "-45°", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "r;15", "+15°", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "r;-15", "-15°", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "r;5", "+5°", pisex::ITEM_DEFAULT);
	pisex::AddItemMenu(menu, "r;-5", "-5°", pisex::ITEM_DEFAULT);
	pisex::SetBackMenu(menu, true);
	pisex::SetExitMenu(menu, true);
	pisex::SetCallback(menu, [index](const char* back, const char*, int, int iSlot) {
		if (back == nullptr) {
			return;
		}
		BlockerPassesPlugin& plugin = g_blockerPassesPlugin;
		if (std::strcmp(back, "back") == 0) {
			plugin.OpenItemMenu(iSlot, index);
			return;
		}
		if (index < 0 || index >= static_cast<int>(plugin.m_items.size())) {
			return;
		}
		float delta = 0.0f;
		if (std::sscanf(back, "r;%f", &delta) == 1) {
			bp::BPItem& item = plugin.m_items[static_cast<std::size_t>(index)];
			item.wallYaw += delta;
			if (item.wallYaw >= 360.0f) {
				item.wallYaw -= 360.0f;
			}
			if (item.wallYaw < 0.0f) {
				item.wallYaw += 360.0f;
			}
			plugin.RespawnLive(index);
			plugin.SaveData();
			plugin.OpenWallRotateMenu(iSlot, index);
		}
	});
	pisex::DisplayPlayerMenu(menu, iSlot, true, true);
}

// --- commands / gamedata / lifecycle -------------------------------------------------------------

bool BlockerPassesPlugin::OnBpCommand(int iSlot) {
	if (!HasBpAccess(iSlot)) {
		PrintChatKey(iSlot, "Chat_NoAccess", "{RED}Нет доступа к {DEFAULT}!bp{RED}.");
		return true;
	}
	OpenMainMenu(iSlot);
	return true;
}

bool BlockerPassesPlugin::OnAccessCommand(int iSlot, const char* args) {
	if (iSlot >= 0) {
		return true;
	}
	if (args == nullptr || *args == '\0') {
		LogInfo(std::string(kLogTag) + " usage: mm_bp_access <steamid64>");
		return true;
	}
	// The original tokenized and took the *second* token (the first is the command name).
	char buffer[128];
	std::snprintf(buffer, sizeof(buffer), "%s", args);
	char* token = std::strtok(buffer, " ");
	if (token != nullptr) {
		token = std::strtok(nullptr, " ");
	}
	if (token == nullptr || *token == '\0') {
		LogInfo(std::string(kLogTag) + " usage: mm_bp_access <steamid64>");
		return true;
	}
	const uint64_t steamid = std::strtoull(token, nullptr, 10);
	if (steamid == 0) {
		LogError(std::string(kLogTag) + " invalid SteamID64");
		return true;
	}
	m_tempAccessSteamIds.push_back(steamid);
	LogInfo(std::string(kLogTag) + " access granted to " + std::to_string(steamid) + " (until map change)");
	return true;
}

bool BlockerPassesPlugin::EnsureCollisionBoundsSignature() {
	if (m_setCollisionBounds != nullptr) {
		return true;
	}
	if (!__s2sdk_LoadGameConfigFile || !__s2sdk_GetGameConfigSignature) {
		return false;
	}
	if (m_gamedata == 0 && !m_gamedataTried) {
		m_gamedataTried = true;
		std::vector<std::filesystem::path> candidates;
		const std::string location = ToString(GetLocation());
		if (!location.empty()) {
			const std::filesystem::path loc(location);
			candidates.push_back(loc / "gamedata.jsonc");
			candidates.push_back(loc.parent_path() / "gamedata.jsonc");
			candidates.push_back(loc.parent_path().parent_path() / "gamedata.jsonc");
		}
		plg::vector<plg::string> paths;
		std::string checked;
		for (const std::filesystem::path& candidate : candidates) {
			std::error_code existsError;
			if (std::filesystem::exists(candidate, existsError)) {
				paths.push_back(plg::string(candidate.string()));
			}
			if (!checked.empty()) {
				checked += ", ";
			}
			checked += candidate.string();
		}
		if (paths.empty()) {
			LogError(std::string(kLogTag) + " gamedata.jsonc not found (looked for: " + checked + ")");
			return false;
		}
		try {
			m_gamedata = s2sdk::LoadGameConfigFile(paths);
			if (m_gamedata == static_cast<s2sdk::ConfigId>(-1)) {
				LogError(std::string(kLogTag) + " s2sdk could not load the gamedata from " +
												 std::string(paths.front()));
				return false;
			}
		} catch (...) {
			return false;
		}
	}
	if (m_gamedata == 0) {
		return false;
	}
	try {
		void* signature = s2sdk::GetGameConfigSignature(m_gamedata, plg::string("SetCollisionBounds"));
		if (signature == nullptr) {
			return false;
		}
		m_setCollisionBounds = reinterpret_cast<SetCollisionBoundsFn>(signature);
		LogInfo(std::string(kLogTag) + " SetCollisionBounds resolved from gamedata");
		return true;
	} catch (...) {
		return false;
	}
}

plg::PluginResult BlockerPassesPlugin::OnPluginStart() {
	try {
		pisex::Init("BlockerPasses");
		LoadSettings();
		LoadPhrases();
		EnsureCollisionBoundsSignature(); // retried from the startup callback when the map is up

		pisex::g_pUtils->RegCommand({m_settings.conCmdBp}, {m_settings.chatCommand}, [](int iSlot, const char*) {
			return g_blockerPassesPlugin.OnBpCommand(iSlot);
		});
		pisex::g_pUtils->RegCommand({m_settings.conCmdAccess}, {},
									[](int iSlot, const char* args) { return g_blockerPassesPlugin.OnAccessCommand(iSlot, args); });

		pisex::g_pUtils->StartupServer([]() {
			// LoadSettings/EnsureCollisionBounds* were possibly incomplete before the map existed.
			g_blockerPassesPlugin.LoadSettings();
			g_blockerPassesPlugin.EnsureCollisionBoundsSignature();
		});
		pisex::g_pUtils->MapStartHook([](const char* map) { g_blockerPassesPlugin.OnMapStart(map); });
		pisex::g_pUtils->MapEndHook([]() { g_blockerPassesPlugin.OnMapEnd(); });
		pisex::g_pUtils->HookEvent("round_start", [](const char*, pisex::IGameEvent*, bool) {
			g_blockerPassesPlugin.OnRoundStart();
		});
		pisex::g_pUtils->HookEvent("player_ping", [](const char*, pisex::IGameEvent* event, bool) {
			if (event != nullptr) {
				g_blockerPassesPlugin.OnPlayerPing(*event);
			}
		});
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnPluginStart failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed: that stops every plugify plugin
}

plg::PluginResult BlockerPassesPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	(void)dt;
	try {
		pisex::Poll();
		if (m_setCollisionBounds == nullptr) {
			EnsureCollisionBoundsSignature(); // gamedata needs a running map on some builds
		}
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " update failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " update failed with an unknown exception");
	}
	return {};
}

plg::PluginResult BlockerPassesPlugin::OnPluginEnd() {
	try {
		ClearLive(true);
		if (__s2sdk_CloseGameConfigFile && m_gamedata != 0 && m_gamedata != static_cast<s2sdk::ConfigId>(-1)) {
			try {
				s2sdk::CloseGameConfigFile(m_gamedata);
			} catch (...) {
			}
			m_gamedata = 0;
		}
		pisex::Shutdown();
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnPluginEnd failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnPluginEnd failed with an unknown exception");
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_blockerPassesPlugin)
