#include "bp_core.hpp"

#include <pisex/paths.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

namespace bp {
namespace {

// The two models the original ships as the fall-back list (LoadSettings when settings.ini is not
// readable).
constexpr const char* kDefaultModelLabel1 = "Желзеные двери";
constexpr const char* kDefaultModelPath1 =
		"models/props/de_dust/hr_dust/dust_windows/dust_rollupdoor_96x128_surface_lod.vmdl";
constexpr const char* kDefaultModelLabel2 = "Желзеный забор";
constexpr const char* kDefaultModelPath2 =
		"models/props/de_nuke/hr_nuke/chainlink_fence_001/chainlink_fence_001_256_capped.vmdl";

float AxisAngle(float degrees) {
	return (degrees / 360.0f - std::floor(degrees / 360.0f)) * 360.0f; // fmod, but never negative
}

// The quote/escape form the KV1 parser (and Valve's own writer) reads back.
void WriteQuoted(std::string& out, std::string_view text) {
	out.push_back('"');
	for (const char c : text) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\t': out += "\\t"; break;
		case '\r': out += "\\r"; break;
		default: out.push_back(c); break;
		}
	}
	out.push_back('"');
}

void WriteNode(std::string& out, pisex::KeyValues& node, int depth) {
	const std::string indent(static_cast<std::size_t>(depth), '\t');
	out += indent;
	WriteQuoted(out, node.GetName());
	if (node.HasValue()) {
		out.push_back(' ');
		WriteQuoted(out, node.GetStringValue(nullptr));
	}
	if (node.HasChildren()) {
		out += "\n";
		out += indent;
		out += "{\n";
		for (pisex::KeyValues* child : node.GetSubKeyList()) {
			if (child != nullptr) {
				WriteNode(out, *child, depth + 1);
			}
		}
		out += indent;
		out += "}\n";
		return;
	}
	out += "\n";
}

} // namespace

const int kWireframeEdges[12][2] = {
		{0, 1}, {1, 2}, {2, 3}, {3, 0},
		{4, 5}, {5, 6}, {6, 7}, {7, 4},
		{0, 4}, {1, 5}, {2, 6}, {3, 7},
};

const int kWireframeCrossEdges[12][2] = {
		{0, 2}, {1, 3},
		{4, 6}, {5, 7},
		{0, 5}, {1, 4},
		{3, 6}, {2, 7},
		{0, 7}, {3, 4},
		{1, 6}, {2, 5},
};

Settings ParseSettings(pisex::KeyValues* kv, bool fileLoaded) {
	Settings settings;
	if (kv == nullptr || !fileLoaded) {
		// "Settings not found, using defaults" of the original: the built-in defaults include the
		// two models.
		settings.models.push_back(ModelDef{kDefaultModelLabel1, kDefaultModelPath1});
		settings.models.push_back(ModelDef{kDefaultModelLabel2, kDefaultModelPath2});
		return settings;
	}

	settings.minPlayersToOpen = kv->GetInt("min_players_to_open", 10);
	settings.accessPermission = kv->GetString("access_permission", "@admin/bp");
	settings.accessFlag = kv->GetString("access_flag", "");
	settings.debugLog = kv->GetInt("debug_log", 1) != 0;
	settings.ignoreSpectators = kv->GetInt("ignore_spectators", 1) != 0;
	settings.chatCommand = kv->GetString("chat_command", "!bp");
	settings.conCmdBp = kv->GetString("console_cmd_bp", "mm_bp");
	settings.conCmdAccess = kv->GetString("console_cmd_access", "mm_bp_access");

	settings.models.clear();
	if (pisex::KeyValues* models = kv->FindKey("models", false)) {
		FOR_EACH_SUBKEY(models, model) {
			const char* label = model->GetString("label", "");
			const char* path = model->GetString("path", "");
			if (path != nullptr && *path != '\0') {
				ModelDef def;
				def.path = path;
				def.label = (label != nullptr && *label != '\0') ? label : path;
				settings.models.push_back(std::move(def));
			}
		}
	}
	return settings;
}

bool IsValidMapName(const char* name) {
	if (name == nullptr || *name == '\0') {
		return false;
	}
	for (const char* p = name; *p != '\0'; ++p) {
		const unsigned char c = static_cast<unsigned char>(*p);
		if (c < 0x20 || c > 0x7E) {
			return false;
		}
	}
	return true;
}

std::string NormalizeMapName(const char* in) {
	if (in == nullptr || *in == '\0') {
		return {};
	}
	std::string s(in);

	const std::size_t pipePos = s.find(" | ");
	if (pipePos != std::string::npos) {
		s = s.substr(0, pipePos);
	}

	std::transform(s.begin(), s.end(), s.begin(),
				   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	std::size_t pos = s.find_last_of('/');
	if (pos != std::string::npos) {
		s = s.substr(pos + 1);
	}
	pos = s.find_last_of('.');
	if (pos != std::string::npos) {
		s = s.substr(0, pos);
	}
	return s;
}

float ClampScale(float v) {
	if (v < 0.05f) {
		v = 0.05f;
	}
	if (v > 20.0f) {
		v = 20.0f;
	}
	return v;
}

void HueToRGB(float hue, int& r, int& g, int& b) {
	const float h = std::fmod(hue, 360.0f) / 60.0f;
	const int i = static_cast<int>(h);
	const float f = h - static_cast<float>(i);
	const int v = 255;
	const int q = static_cast<int>(255 * (1.0f - f));
	const int t = static_cast<int>(255 * f);
	switch (i % 6) {
	case 0: r = v; g = t; b = 0; break;
	case 1: r = q; g = v; b = 0; break;
	case 2: r = 0; g = v; b = t; break;
	case 3: r = 0; g = q; b = v; break;
	case 4: r = t; g = 0; b = v; break;
	default: r = v; g = 0; b = q; break;
	}
}

std::array<plg::vec3, 8> WireframeCorners(const plg::vec3& p1, const plg::vec3& p2, float yaw) {
	const float minX = std::fmin(p1.x, p2.x), maxX = std::fmax(p1.x, p2.x);
	const float minY = std::fmin(p1.y, p2.y), maxY = std::fmax(p1.y, p2.y);
	const float minZ = std::fmin(p1.z, p2.z), maxZ = std::fmax(p1.z, p2.z);

	std::array<plg::vec3, 8> corners = {
			plg::vec3{minX, minY, minZ}, plg::vec3{maxX, minY, minZ}, plg::vec3{maxX, maxY, minZ},
			plg::vec3{minX, maxY, minZ}, plg::vec3{minX, minY, maxZ}, plg::vec3{maxX, minY, maxZ},
			plg::vec3{maxX, maxY, maxZ}, plg::vec3{minX, maxY, maxZ},
	};

	if (yaw != 0.0f) {
		const float cx = (minX + maxX) * 0.5f;
		const float cy = (minY + maxY) * 0.5f;
		const float rad = yaw * static_cast<float>(M_PI) / 180.0f;
		const float cosA = std::cos(rad);
		const float sinA = std::sin(rad);
		for (plg::vec3& corner : corners) {
			const float dx = corner.x - cx;
			const float dy = corner.y - cy;
			corner.x = cx + dx * cosA - dy * sinA;
			corner.y = cy + dx * sinA + dy * cosA;
		}
	}
	return corners;
}

std::array<std::pair<plg::vec3, plg::vec3>, 24> WallBeamSegments(const plg::vec3& p1, const plg::vec3& p2, float yaw) {
	const std::array<plg::vec3, 8> corners = WireframeCorners(p1, p2, yaw);
	std::array<std::pair<plg::vec3, plg::vec3>, 24> segments{};
	std::size_t i = 0;
	for (const auto& edge : kWireframeEdges) {
		segments[i++] = {corners[edge[0]], corners[edge[1]]};
	}
	for (const auto& edge : kWireframeCrossEdges) {
		segments[i++] = {corners[edge[0]], corners[edge[1]]};
	}
	return segments;
}

WallBox ComputeWallBox(const plg::vec3& p1, const plg::vec3& p2, float yaw) {
	WallBox box;
	float halfExt[3];
	const float* a = &p1.x;
	const float* b = &p2.x;
	float* center = &box.center.x;
	for (int axis = 0; axis < 3; ++axis) {
		const float lo = std::fmin(a[axis], b[axis]);
		const float hi = std::fmax(a[axis], b[axis]);
		center[axis] = (lo + hi) * 0.5f;
		halfExt[axis] = std::fmax((hi - lo) * 0.5f, 1.0f);
	}

	const float normalYaw = AxisAngle(yaw);
	const bool isAxisAligned = (normalYaw < 1.0f || std::fabs(normalYaw - 90.0f) < 1.0f ||
								std::fabs(normalYaw - 180.0f) < 1.0f || std::fabs(normalYaw - 270.0f) < 1.0f ||
								normalYaw > 359.0f);

	if (isAxisAligned) {
		float hx = halfExt[0];
		float hy = halfExt[1];
		if (std::fabs(normalYaw - 90.0f) < 1.0f || std::fabs(normalYaw - 270.0f) < 1.0f) {
			std::swap(hx, hy);
		}
		box.mins = plg::vec3{-hx, -hy, -halfExt[2]};
		box.maxs = plg::vec3{hx, hy, halfExt[2]};
		box.yaw = 0.0f;
	} else {
		box.mins = plg::vec3{-halfExt[0], -halfExt[1], -halfExt[2]};
		box.maxs = plg::vec3{halfExt[0], halfExt[1], halfExt[2]};
		box.yaw = yaw; // the original passed the raw yaw through to the entity
	}
	return box;
}

void SurroundBounds(const plg::vec3& mins, const plg::vec3& maxs, float yaw, plg::vec3& surroundMins,
					plg::vec3& surroundMaxs) {
	const float cosAbs = std::fabs(std::cos(yaw * static_cast<float>(M_PI) / 180.0f));
	const float sinAbs = std::fabs(std::sin(yaw * static_cast<float>(M_PI) / 180.0f));
	const float surroundHX = std::fabs(maxs.x) * cosAbs + std::fabs(maxs.y) * sinAbs;
	const float surroundHY = std::fabs(maxs.x) * sinAbs + std::fabs(maxs.y) * cosAbs;
	surroundMins = plg::vec3{-surroundHX, -surroundHY, mins.z};
	surroundMaxs = plg::vec3{surroundHX, surroundHY, maxs.z};
}

void ScaleWall(BPItem& item, float delta) {
	const plg::vec3 center{(item.pos.x + item.pos2.x) * 0.5f, (item.pos.y + item.pos2.y) * 0.5f,
						   (item.pos.z + item.pos2.z) * 0.5f};
	plg::vec3 half{(item.pos2.x - item.pos.x) * 0.5f, (item.pos2.y - item.pos.y) * 0.5f,
				   (item.pos2.z - item.pos.z) * 0.5f};
	float* h = &half.x;
	for (int axis = 0; axis < 3; ++axis) {
		if (h[axis] > 0.0f) {
			h[axis] = std::fmax(h[axis] + delta * 0.5f, 1.0f);
		} else if (h[axis] < 0.0f) {
			h[axis] = std::fmin(h[axis] - delta * 0.5f, -1.0f);
		} else {
			h[axis] = delta * 0.5f;
		}
	}
	item.pos = plg::vec3{center.x - half.x, center.y - half.y, center.z - half.z};
	item.pos2 = plg::vec3{center.x + half.x, center.y + half.y, center.z + half.z};
}

void DeepCopy(pisex::KeyValues& from, pisex::KeyValues& to) {
	pisex::KeyValues* child = to.AddSubKey(from.GetName());
	if (from.HasValue()) {
		child->SetString({}, from.GetStringValue(nullptr));
	}
	for (pisex::KeyValues* sub : from.GetSubKeyList()) {
		if (sub != nullptr) {
			DeepCopy(*sub, *child);
		}
	}
}

std::string SerializeKeyValues(pisex::KeyValues& node) {
	std::string out;
	WriteNode(out, node, 0);
	return out;
}

std::vector<BPItem> ReadMapSection(pisex::KeyValues& root, const std::string& map) {
	std::vector<BPItem> items;
	pisex::KeyValues* mapKV = root.FindKey(map.c_str(), false);
	if (mapKV == nullptr) {
		return items;
	}
	FOR_EACH_SUBKEY(mapKV, k) {
		BPItem it;
		it.label = k->GetString("label", "");
		it.path = k->GetString("path", "");
		it.pos.x = k->GetFloat("px", 0.0f);
		it.pos.y = k->GetFloat("py", 0.0f);
		it.pos.z = k->GetFloat("pz", 0.0f);
		it.ang.x = k->GetFloat("ax", 0.0f);
		it.ang.y = k->GetFloat("ay", 0.0f);
		it.ang.z = k->GetFloat("az", 0.0f);
		it.scale = k->GetFloat("sc", 1.0f);
		it.invisible = k->GetInt("iv", 0) != 0;
		it.isWall = k->GetInt("wall", 0) != 0;
		if (it.isWall) {
			it.pos2.x = k->GetFloat("p2x", 0.0f);
			it.pos2.y = k->GetFloat("p2y", 0.0f);
			it.pos2.z = k->GetFloat("p2z", 0.0f);
			it.beamR = k->GetInt("br", 0);
			it.beamG = k->GetInt("bg", 128);
			it.beamB = k->GetInt("bb", 255);
			it.beamRainbow = k->GetInt("brb", 0) != 0;
			it.wallYaw = k->GetFloat("wy", 0.0f);
		} else {
			it.itemR = k->GetInt("ir", 255);
			it.itemG = k->GetInt("ig", 255);
			it.itemB = k->GetInt("ib", 255);
		}
		if (!it.path.empty() || it.isWall) {
			items.push_back(std::move(it));
		}
	}
	return items;
}

void WriteMapSection(pisex::KeyValues& root, const std::string& map, const std::vector<BPItem>& items) {
	// The original removed the old section and wrote a fresh one, keeping every other map.
	pisex::KeyValues rebuilt(root.GetName());
	if (root.HasValue()) {
		rebuilt.SetString({}, root.GetStringValue(nullptr)); // keeps a root value if there is one
	}
	for (pisex::KeyValues* child : root.GetSubKeyList()) {
		if (child == nullptr) {
			continue;
		}
		// FindKey-like case-insensitive section match (Valve's KeyValues are case-insensitive).
		std::string name(child->GetName());
		std::string wanted(map);
		std::transform(name.begin(), name.end(), name.begin(),
					   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		std::transform(wanted.begin(), wanted.end(), wanted.begin(),
					   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (name == wanted) {
			continue;
		}
		DeepCopy(*child, rebuilt);
	}

	pisex::KeyValues* section = rebuilt.AddSubKey(map);
	for (const BPItem& it : items) {
		pisex::KeyValues* k = section->AddSubKey("item");
		k->SetString("label", it.label);
		k->SetString("path", it.path);
		char buffer[32];
		std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(it.pos.x));
		k->SetString("px", buffer);
		std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(it.pos.y));
		k->SetString("py", buffer);
		std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(it.pos.z));
		k->SetString("pz", buffer);
		std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(it.ang.x));
		k->SetString("ax", buffer);
		std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(it.ang.y));
		k->SetString("ay", buffer);
		std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(it.ang.z));
		k->SetString("az", buffer);
		std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(it.scale));
		k->SetString("sc", buffer);
		k->SetInt("iv", it.invisible ? 1 : 0);
		k->SetInt("wall", it.isWall ? 1 : 0);
		if (it.isWall) {
			std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(it.pos2.x));
			k->SetString("p2x", buffer);
			std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(it.pos2.y));
			k->SetString("p2y", buffer);
			std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(it.pos2.z));
			k->SetString("p2z", buffer);
			k->SetInt("br", it.beamR);
			k->SetInt("bg", it.beamG);
			k->SetInt("bb", it.beamB);
			k->SetInt("brb", it.beamRainbow ? 1 : 0);
			if (it.wallYaw != 0.0f) {
				std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(it.wallYaw));
				k->SetString("wy", buffer);
			}
		} else {
			k->SetInt("ir", it.itemR);
			k->SetInt("ig", it.itemG);
			k->SetInt("ib", it.itemB);
		}
	}

	// Copy the rebuilt sections back into the caller's root (Clear first: the tree is replaced).
	root.Clear();
	if (rebuilt.HasValue()) {
		root.SetString({}, rebuilt.GetStringValue(nullptr));
	}
	for (pisex::KeyValues* child : rebuilt.GetSubKeyList()) {
		if (child != nullptr) {
			DeepCopy(*child, root);
		}
	}
}

bool WriteFileAtomic(const std::string& path, const std::string& text, std::string& error) {
	const std::filesystem::path target(path);
	const std::filesystem::path temp = target.parent_path() / (target.filename().string() + ".tmp");
	std::error_code ioError;
	{
		std::ofstream file(temp, std::ios::binary | std::ios::trunc);
		if (!file) {
			error = "cannot write " + temp.string();
			return false;
		}
		file << text;
		file.close();
		if (!file) {
			error = "cannot write " + temp.string();
			std::filesystem::remove(temp, ioError);
			return false;
		}
	}
	std::filesystem::rename(temp, target, ioError);
	if (ioError) {
		error = "cannot replace " + path + ": " + ioError.message();
		std::filesystem::remove(temp, ioError);
		return false;
	}
	return true;
}

bool SaveDataFile(std::string_view readPath, const std::string& writePath, const std::string& map,
				  const std::vector<BPItem>& items, std::string& error) {
	pisex::KeyValues root("BPData");
	if (!root.LoadFromFile(readPath)) {
		// A missing file is the first save; an unreadable or malformed one must not be replaced:
		// the rebuild only knows the loaded sections, so the other maps would be dropped.
		const std::string existing = pisex::ResolvePath(readPath);
		if (pisex::FileExists(existing)) {
			error = "the existing data file " + existing + " cannot be parsed - not overwriting it";
			return false;
		}
	}
	WriteMapSection(root, map, items);
	const std::string text = SerializeKeyValues(root);
	return WriteFileAtomic(writePath, text, error);
}

} // namespace bp
