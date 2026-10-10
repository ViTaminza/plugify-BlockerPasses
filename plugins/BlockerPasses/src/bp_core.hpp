// BlockerPasses — engine-independent core: the settings/data model, the KV1 data-file store and
// the geometry helpers of the original.
//
// Everything here is pure data work, so it is covered by bp_core_tests without an engine: the
// settings.ini parsing (against the production file), the bp_data.ini round-trip (the original
// loaded the whole file, replaced one map section and saved it back - other maps must survive),
// the map-name normalization and the wall geometry (box placement + rotation).

#pragma once

#include <pisex/keyvalues.hpp>

#include <plg/numerics.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bp {

constexpr int kMaxSlots = 64;

// The [BlockerPasses] settings.ini keys of the original's LoadSettings().
struct ModelDef {
	std::string label;
	std::string path;
};

struct Settings {
	int minPlayersToOpen = 10;		// min_players_to_open
	bool debugLog = true;			// debug_log
	bool ignoreSpectators = true;	// ignore_spectators
	std::string accessPermission = "@admin/bp"; // access_permission
	std::string accessFlag;						// access_flag
	std::string chatCommand = "!bp";			// chat_command
	std::string conCmdBp = "mm_bp";				// console_cmd_bp
	std::string conCmdAccess = "mm_bp_access";	// console_cmd_access
	std::vector<ModelDef> models;				// models { modelN { label path } }
};

// LoadSettings() of the original. `kv` is the parsed settings.ini tree; `fileLoaded` is false when
// the file could not be read (the built-in defaults, *with* the two default models, apply then).
// A null `kv` with fileLoaded == false is the "no file" case as well.
Settings ParseSettings(pisex::KeyValues* kv, bool fileLoaded);

// One placeable item / wall (g_Items of the original, including the wall-only fields).
struct BPItem {
	std::string label;
	std::string path;
	plg::vec3 pos{0.0f, 0.0f, 0.0f};
	plg::vec3 ang{0.0f, 0.0f, 0.0f};
	float scale = 1.0f;
	bool invisible = false;
	bool isWall = false;
	plg::vec3 pos2{0.0f, 0.0f, 0.0f};
	int beamR = 0;
	int beamG = 128;
	int beamB = 255;
	bool beamRainbow = false;
	float wallYaw = 0.0f;
	int itemR = 255;
	int itemG = 255;
	int itemB = 255;
};

// Entity HANDLES are int32 views of the engine's uint32 CEntityHandle (index | serial << 15):
// valid <=> h != INVALID_EHANDLE_INDEX (-1) && h != 0 (index 0 is the world; CreateEntityByName
// answers -1 when the creation failed). The sign means nothing - once the engine's serial counter
// passes 65535 bit 31 is set and a valid handle is a negative int; a `h >= 0` test treats those
// entities as failed/absent (they are never removed then).
inline bool IsValidHandle(int32_t h) { return h != -1 && h != 0; }

// One spawned item (g_Live of the original): the entity handles (s2sdk entity *handles*, the value
// CreateEntityByName returns; IsValidHandle above).
struct LiveEnt {
	int index = -1;
	int ent = -1;
	std::vector<int> beams;
	std::vector<int> wallColls;
};

// IsValidMapName() of the original: printable ASCII only (a corrupted engine map name is rejected).
bool IsValidMapName(const char* name);
// NormalizeMapName(): lower case, everything after " | ", any directory prefix and the extension
// removed.
std::string NormalizeMapName(const char* in);

// ClampScale() of the original (0.05 .. 20.0).
float ClampScale(float v);
// HueToRGB() of the original (0..360 degrees).
void HueToRGB(float hue, int& r, int& g, int& b);

// DrawWireframe() of the original: the eight corners of the [p1,p2] box, rotated by yaw degrees
// around the box's X/Y center.
std::array<plg::vec3, 8> WireframeCorners(const plg::vec3& p1, const plg::vec3& p2, float yaw);

// SpawnWallCollisions() of the original: the collision box of a wall - the axis-aligned form swaps
// the half extents for a 90/270 degree yaw, otherwise the yaw goes to the entity and the box is
// axis-aligned around the center with the full extents.
struct WallBox {
	plg::vec3 center{0.0f, 0.0f, 0.0f};
	plg::vec3 mins{0.0f, 0.0f, 0.0f};
	plg::vec3 maxs{0.0f, 0.0f, 0.0f};
	float yaw = 0.0f; // entity yaw; 0 for the axis-aligned form
};
WallBox ComputeWallBox(const plg::vec3& p1, const plg::vec3& p2, float yaw);

// SpawnOneCollisionBox() of the original: the "specified surrounding" bounds of a rotated box are
// the axis-aligned hull of the rotated mins/maxs (|cos|/|sin| of the yaw).
void SurroundBounds(const plg::vec3& mins, const plg::vec3& maxs, float yaw, plg::vec3& surroundMins,
					plg::vec3& surroundMaxs);

// The size/rotate menu math of the original (OpenWallScaleMenu): moves pos/pos2 so that the box
// grows/shrinks by `delta` on every axis while keeping its center. The original's per-axis rules
// (positive half, negative half, degenerate) are kept.
void ScaleWall(BPItem& item, float delta);

// The wireframe edges of DrawWireframe (the 12 box edges plus the 12 cross edges).
extern const int kWireframeEdges[12][2];
extern const int kWireframeCrossEdges[12][2];
// 12 + 12 beam segments for a wall (the original created one env_beam per edge).
std::array<std::pair<plg::vec3, plg::vec3>, 24> WallBeamSegments(const plg::vec3& p1, const plg::vec3& p2, float yaw);

// --- the bp_data.ini store ----------------------------------------------------------------
// A KV1 writer for the loaded tree. The original saved through the engine filesystem (SaveToFile);
// the port rebuilds the whole file text, so the sections of the other maps survive a save.
std::string SerializeKeyValues(pisex::KeyValues& node);
// Deep copy of a KV subtree (pisex::KeyValues is not copyable; SaveData() rebuilds the root).
void DeepCopy(pisex::KeyValues& from, pisex::KeyValues& to);
// Replaces the section of `map` with the given items (creating it when missing) and rebuilds
// `root` with every other section copied over.
void WriteMapSection(pisex::KeyValues& root, const std::string& map, const std::vector<BPItem>& items);
// The items of `map` (empty when the section does not exist).
std::vector<BPItem> ReadMapSection(pisex::KeyValues& root, const std::string& map);

// Writes `text` to a temporary file next to `path` and renames it over `path`: a crash mid-write
// cannot truncate the file, the old content survives until the rename. Returns false and sets
// `error` when the temporary cannot be written or the rename fails (the temporary is removed
// then).
bool WriteFileAtomic(const std::string& path, const std::string& text, std::string& error);
// SaveData() of the original: loads the whole file, replaces the section of `map` and writes the
// result back. A file that exists but cannot be parsed refuses the save (the rebuilt tree is
// empty and would silently drop the sections of every other map). `readPath` resolves like the
// loaders do, `writePath` is used as given; false sets `error`.
bool SaveDataFile(std::string_view readPath, const std::string& writePath, const std::string& map,
                  const std::vector<BPItem>& items, std::string& error);

} // namespace bp
