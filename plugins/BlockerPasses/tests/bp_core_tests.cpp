// BlockerPasses unit tests — the engine-independent parts: the settings.ini parsing (against the
// *production* file), the bp_data.ini round-trip (other maps must survive a
// save), the map-name normalization and the wall geometry of the original.
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "bp_core.hpp"

#include <plg/plugin.hpp>

#include <pisex/keyvalues.hpp>
#include <pisex/paths.hpp>

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// pisexcompat (linked for pisex::KeyValues) reads the plugify dirs through these globals; a real
// plugin gets them from PLUGIFY_PLUGIN, the test leaves them null on purpose (the same state
// tests/pisexcompat_tests.cpp exercises).
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEq(const std::string& actual, const std::string& expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s\n  expected: [%s]\n  actual:   [%s]\n", what.c_str(), expected.c_str(),
					actual.c_str());
	}
}

void CheckNear(float actual, float expected, const std::string& what, float epsilon = 0.001f) {
	++g_checks;
	if (std::fabs(actual - expected) > epsilon) {
		++g_failures;
		std::printf("FAIL: %s (expected %f, got %f)\n", what.c_str(), expected, actual);
	}
}

void TestDefaultSettings() {
	// No file at all: the original fell back to the built-in defaults *with* the two models.
	const bp::Settings settings = bp::ParseSettings(nullptr, false);
	Check(settings.minPlayersToOpen == 10, "default min_players_to_open");
	Check(settings.debugLog && settings.ignoreSpectators, "default debug_log/ignore_spectators");
	CheckEq(settings.accessPermission, "@admin/bp", "default access_permission");
	CheckEq(settings.chatCommand, "!bp", "default chat_command");
	CheckEq(settings.conCmdBp, "mm_bp", "default console_cmd_bp");
	CheckEq(settings.conCmdAccess, "mm_bp_access", "default console_cmd_access");
	Check(settings.models.size() == 2, "the built-in default models are the two of the original");
	Check(settings.models[0].path.find(".vmdl") != std::string::npos, "default model paths are vmdl files");
}

void TestProductionSettings() {
	// The shipped production settings.ini.
	const std::string path = std::string(BP_TEST_DATA_DIR) + "/settings.ini";
	pisex::KeyValues kv("BlockerPasses");
	const bool loaded = kv.LoadFromFile(path);
	Check(loaded, "the production settings.ini parses (" + path + ")");
	if (!loaded) {
		return;
	}
	const bp::Settings settings = bp::ParseSettings(&kv, true);
	Check(settings.minPlayersToOpen == 10, "production min_players_to_open");
	Check(!settings.debugLog, "production debug_log is 0");
	Check(settings.ignoreSpectators, "production ignore_spectators is 1");
	CheckEq(settings.accessPermission, "@admin/bp", "production access_permission");
	Check(settings.models.size() == 2, "production models list");
	CheckEq(settings.models[0].label, "Желзеные двери", "production model 1 label");
	CheckEq(settings.models[1].label, "Желзеный забор", "production model 2 label");

	// The production file has no chat/console command keys: the code defaults apply (parity with
	// the Metamod plugin).
	CheckEq(settings.chatCommand, "!bp", "missing keys keep the code defaults");
	CheckEq(settings.conCmdBp, "mm_bp", "missing console command key keeps the default");
}

void TestMapNames() {
	Check(bp::IsValidMapName("de_dust2"), "a plain map name is valid");
	Check(!bp::IsValidMapName(""), "an empty map name is invalid");
	Check(!bp::IsValidMapName("de_\x01ust2"), "a non-printable byte makes the name invalid");
	CheckEq(bp::NormalizeMapName("workshop/12345/de_dust2 | Something"), "de_dust2",
			"directory, extension and the ' | ' suffix are stripped");
	CheckEq(bp::NormalizeMapName("DE_Dust2.vmf"), "de_dust2", "lower case and extension removed");
	CheckEq(bp::NormalizeMapName(""), "", "an empty name stays empty");
}

void TestGeometry() {
	CheckNear(bp::ClampScale(0.001f), 0.05f, "ClampScale lower bound");
	CheckNear(bp::ClampScale(50.0f), 20.0f, "ClampScale upper bound");
	CheckNear(bp::ClampScale(2.5f), 2.5f, "ClampScale keeps a sane value");

	int r = 0, g = 0, b = 0;
	bp::HueToRGB(0.0f, r, g, b);
	Check(r == 255 && g == 0 && b == 0, "hue 0 is red");
	bp::HueToRGB(120.0f, r, g, b);
	Check(r == 0 && g == 255 && b == 0, "hue 120 is green");
	bp::HueToRGB(240.0f, r, g, b);
	Check(r == 0 && g == 0 && b == 255, "hue 240 is blue");

	// The wireframe box of DrawWireframe: 8 corners, rotated around the center by the wall yaw.
	const auto corners = bp::WireframeCorners({0.0f, 0.0f, 0.0f}, {100.0f, 50.0f, 20.0f}, 0.0f);
	CheckNear(corners[0].x, 0.0f, "corner 0 min x");
	CheckNear(corners[6].x, 100.0f, "corner 6 max x");
	CheckNear(corners[6].y, 50.0f, "corner 6 max y");
	CheckNear(corners[6].z, 20.0f, "corner 6 max z");
	const auto rotated = bp::WireframeCorners({0.0f, 0.0f, 0.0f}, {100.0f, 50.0f, 20.0f}, 90.0f);
	// A 90 degree yaw rotates the corners around the box center (50, 25): corner 0 (0,0) -> (75,-25).
	CheckNear(rotated[0].x, 75.0f, "rotated corner 0 x");
	CheckNear(rotated[0].y, -25.0f, "rotated corner 0 y");
	CheckNear(rotated[0].z, 0.0f, "the yaw rotation leaves Z alone");
	CheckNear(rotated[6].z, 20.0f, "the top corner keeps its Z");

	// The wall collision box: axis-aligned forms swap the extents for a 90/270 degree yaw.
	const bp::WallBox wall = bp::ComputeWallBox({0.0f, 0.0f, 0.0f}, {100.0f, 40.0f, 30.0f}, 0.0f);
	CheckNear(wall.center.x, 50.0f, "wall center x");
	CheckNear(wall.mins.x, -50.0f, "wall mins x");
	CheckNear(wall.maxs.y, 20.0f, "wall maxs y");
	CheckNear(wall.yaw, 0.0f, "an axis-aligned wall gets yaw 0");

	const bp::WallBox turned = bp::ComputeWallBox({0.0f, 0.0f, 0.0f}, {100.0f, 40.0f, 30.0f}, 90.0f);
	CheckNear(turned.mins.x, -20.0f, "90 degrees swaps the X extent");
	CheckNear(turned.maxs.y, 50.0f, "90 degrees swaps the Y extent");
	CheckNear(turned.yaw, 0.0f, "the swapped form stays axis-aligned");

	const bp::WallBox obb = bp::ComputeWallBox({0.0f, 0.0f, 0.0f}, {100.0f, 40.0f, 30.0f}, 45.0f);
	CheckNear(obb.mins.x, -50.0f, "an OBB keeps the full extents");
	CheckNear(obb.yaw, 45.0f, "an OBB carries the yaw to the entity");

	// Degenerate walls keep a 1-unit minimum half extent (the original's fmaxf(.., 1.0f)).
	const bp::WallBox thin = bp::ComputeWallBox({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.0f);
	CheckNear(thin.mins.x, -1.0f, "a degenerate wall becomes a 1-unit box");

	const auto segments = bp::WallBeamSegments({0.0f, 0.0f, 0.0f}, {100.0f, 50.0f, 20.0f}, 0.0f);
	Check(segments.size() == 24, "24 beams per wall (12 edges + 12 cross edges)");
	CheckNear(segments[0].first.x, 0.0f, "the first segment starts at corner 0");
	CheckNear(segments[0].second.x, 100.0f, "the first segment ends at corner 1");

	// The size menu math keeps the center and enforces the 1-unit minimum.
	bp::BPItem item;
	item.pos = plg::vec3{-50.0f, -25.0f, 0.0f};
	item.pos2 = plg::vec3{50.0f, 25.0f, 20.0f};
	bp::ScaleWall(item, 10.0f);
	CheckNear(item.pos.x, -55.0f, "growing moves the lower corner out");
	CheckNear(item.pos2.x, 55.0f, "growing moves the upper corner out");
	CheckNear(item.pos.z, -5.0f, "growing applies to Z as well");
	bp::ScaleWall(item, -400.0f);
	CheckNear(item.pos.x, -1.0f, "shrinking stops at the 1-unit half extent");
	CheckNear(item.pos2.x, 1.0f, "both corners respect the minimum");
}

void TestSurroundBounds() {
	plg::vec3 mins{};
	plg::vec3 maxs{};
	bp::SurroundBounds(plg::vec3{-10.0f, -20.0f, -5.0f}, plg::vec3{10.0f, 20.0f, 5.0f}, 0.0f, mins, maxs);
	CheckNear(mins.x, -10.0f, "axis-aligned surround mins x");
	CheckNear(maxs.y, 20.0f, "axis-aligned surround maxs y");
	CheckNear(mins.z, -5.0f, "the Z extents are taken from the box");

	// A 90 degree yaw swaps the X/Y hull (|cos|=0, |sin|=1).
	bp::SurroundBounds(plg::vec3{-10.0f, -20.0f, -5.0f}, plg::vec3{10.0f, 20.0f, 5.0f}, 90.0f, mins, maxs);
	CheckNear(mins.x, -20.0f, "rotated surround mins x");
	CheckNear(maxs.y, 10.0f, "rotated surround maxs y");
}

void TestDataFileRoundTrip() {
	// The original wrote the whole bp_data.ini back, so the sections of other maps must survive
	// a save of the current map (the port rebuilds the tree for that).
	pisex::KeyValues root("BPData");
	const std::string initial =
			"\"BPData\"\n"
			"{\n"
			"\t\"de_dust2\"\n"
			"\t{\n"
			"\t\t\"item\"\n"
			"\t\t{\n"
			"\t\t\t\"label\" \"Door\"\n"
			"\t\t\t\"px\" \"1\"\n"
			"\t\t}\n"
			"\t}\n"
			"\t\"de_nuke\"\n"
			"\t{\n"
			"\t\t\"item\"\n"
			"\t\t{\n"
			"\t\t\t\"wall\" \"1\"\n"
			"\t\t\t\"p2x\" \"7\"\n"
			"\t\t}\n"
			"\t}\n"
			"}\n";
	Check(root.LoadFromBuffer(initial), "the fixture parses");

	const std::vector<bp::BPItem> nuke = bp::ReadMapSection(root, "de_nuke");
	Check(nuke.size() == 1 && nuke[0].isWall, "a wall section is read back");
	CheckNear(nuke[0].pos2.x, 7.0f, "the wall's pos2 survives the parse");
	Check(nuke[0].beamG == 128 && nuke[0].beamB == 255, "the beam colour defaults are the original's");

	bp::BPItem missile;
	missile.label = "Missile";
	missile.path = "models/x.vmdl";
	missile.pos = plg::vec3{10.5f, -20.25f, 30.0f};
	missile.ang = plg::vec3{0.0f, 90.0f, 0.0f};
	missile.scale = 2.0f;
	missile.invisible = true;
	missile.itemR = 10;
	missile.itemG = 20;
	missile.itemB = 30;
	const std::vector<bp::BPItem> items = {missile};

	// Writing de_dust2 replaces only that section; de_nuke must stay.
	bp::WriteMapSection(root, "de_dust2", items);
	const std::string text = bp::SerializeKeyValues(root);
	Check(text.find("\"de_nuke\"") != std::string::npos, "the other map's section survives a save");
	Check(text.find("Missile") != std::string::npos, "the new item is in the saved text");

	pisex::KeyValues reloaded("BPData");
	Check(reloaded.LoadFromBuffer(text), "the written text parses back (" + text + ")");
	const std::vector<bp::BPItem> readBack = bp::ReadMapSection(reloaded, "de_dust2");
	Check(readBack.size() == 1, "the new item is read back");
	if (readBack.size() == 1) {
		CheckEq(readBack[0].label, "Missile", "label round-trip");
		CheckEq(readBack[0].path, "models/x.vmdl", "path round-trip");
		CheckNear(readBack[0].pos.x, 10.5f, "pos.x round-trip");
		CheckNear(readBack[0].pos.y, -20.25f, "pos.y round-trip");
		CheckNear(readBack[0].ang.y, 90.0f, "angle round-trip");
		CheckNear(readBack[0].scale, 2.0f, "scale round-trip");
		Check(readBack[0].invisible, "invisible round-trip");
		Check(readBack[0].itemR == 10 && readBack[0].itemG == 20 && readBack[0].itemB == 30,
			  "the item colour round-trips");
	}
	const std::vector<bp::BPItem> nukeAfter = bp::ReadMapSection(reloaded, "de_nuke");
	Check(nukeAfter.size() == 1 && nukeAfter[0].isWall, "the other map's wall is still intact");
	CheckNear(nukeAfter[0].pos2.x, 7.0f, "the other map's value is unchanged");
}

void TestLabelQuoting() {
	// The writer must quote/escape so that the parser reads the same text back (labels are free
	// text, e.g. Russian or names with quotes).
	pisex::KeyValues root("BPData");
	bp::BPItem item;
	item.label = "Дверь \"A\" \\ test";
	item.path = "models/a.vmdl";
	bp::WriteMapSection(root, "de_test", {item});
	const std::string text = bp::SerializeKeyValues(root);
	pisex::KeyValues reloaded("BPData");
	Check(reloaded.LoadFromBuffer(text), "escaped text parses back");
	const std::vector<bp::BPItem> readBack = bp::ReadMapSection(reloaded, "de_test");
	Check(readBack.size() == 1 && readBack[0].label == item.label, "quotes/backslashes round-trip");
}

void TestHandleValidity() {
	// A real handle: index 93, serial 102949 -> 0xC912805D = -921534371, a valid handle the
	// game hands out once its serial counter has passed 65535 (bit 31 set).
	Check(bp::IsValidHandle(-921534371), "-921534371 (0xC912805D) is a valid handle");
	Check(bp::IsValidHandle(0x7FFFFFFF), "0x7FFFFFFF is a valid handle");
	Check(bp::IsValidHandle(1), "handle 1 is valid");
	Check(!bp::IsValidHandle(0), "0 (the world / \"none\") is not a valid handle");
	Check(!bp::IsValidHandle(-1), "-1 (INVALID_EHANDLE_INDEX) is not a valid handle");
}

void TestSaveDataFile() {
	// The whole SaveData() file path (load -> replace one section -> write back): an existing file
	// that does not parse must be left alone (a rewrite would drop the other maps) and the write
	// itself goes through a temporary file.
	const std::filesystem::path dir =
			std::filesystem::temp_directory_path() / ("bp_core_save_test_" + std::to_string(::getpid()));
	std::error_code ioError;
	std::filesystem::remove_all(dir, ioError);
	std::filesystem::create_directories(dir, ioError);
	const std::string path = (dir / "bp_data.ini").string();

	bp::BPItem item;
	item.label = "Missile";
	item.path = "models/x.vmdl";

	// First save: no file yet, the section is created.
	std::string saveError;
	Check(bp::SaveDataFile(path, path, "de_dust2", {item}, saveError),
		  "the first save writes the file (" + saveError + ")");
	Check(!std::filesystem::exists(path + ".tmp"), "no temporary file survives the write");
	{
		pisex::KeyValues kv("BPData");
		Check(kv.LoadFromFile(path), "the saved file parses back");
		Check(bp::ReadMapSection(kv, "de_dust2").size() == 1, "the saved section is read back");
	}

	// A save of another map keeps the first one.
	bp::BPItem wall;
	wall.isWall = true;
	wall.pos2 = plg::vec3{1.0f, 2.0f, 3.0f};
	Check(bp::SaveDataFile(path, path, "de_nuke", {wall}, saveError), "the second save succeeds");
	{
		pisex::KeyValues kv("BPData");
		Check(kv.LoadFromFile(path), "the file still parses");
		Check(bp::ReadMapSection(kv, "de_dust2").size() == 1, "the first map's section survives");
		Check(bp::ReadMapSection(kv, "de_nuke").size() == 1, "the second map's section is there");
	}

	// An existing file that cannot be parsed (e.g. truncated by a crash) must not be overwritten.
	const std::string broken = "\"BPData\"\n{\n\t\"de_nuke\"\n\t{\n\t\t\"item\"\n";
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << broken;
	}
	Check(!bp::SaveDataFile(path, path, "de_dust2", {item}, saveError),
		  "a save over an unparsable file is refused");
	Check(!saveError.empty(), "the refusal carries a message");
	CheckEq(pisex::ReadFile(path), broken, "the unparsable file is left untouched");

	// A write that cannot succeed reports why (and leaves the target alone).
	Check(!bp::WriteFileAtomic((dir / "missing_dir" / "x.ini").string(), "text", saveError),
		  "writing into a missing directory fails");
	Check(!saveError.empty(), "the failed write carries a message");

	std::filesystem::remove_all(dir, ioError);
}

} // namespace

int main() {
	std::printf("BlockerPasses tests (data dir: %s)\n", BP_TEST_DATA_DIR);
	TestDefaultSettings();
	TestProductionSettings();
	TestMapNames();
	TestGeometry();
	TestSurroundBounds();
	TestDataFileRoundTrip();
	TestLabelQuoting();
	TestHandleValidity();
	TestSaveDataFile();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
