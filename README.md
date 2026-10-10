# BlockerPasses (Plugify)

Per-map blocker items and beams, ported from the Metamod plugin BlockerPasses (ABKAM 2.0.1). 'mm_bp' / '!bp' opens the admin menu (access_permission '@admin/bp' from addons/configs/BlockerPasses/settings.ini, or a SteamID64 granted this map via 'mm_bp_access'): place a model at the crosshair, build a wall (hidden func_brush collision box with OBB bounds through the engine's SetCollisionBounds signature, plus a 24-beam env_beam wireframe), move/rotate/scale/recolour/hide/delete items, teleport to them or move them to a ping. Below min_players_to_open (humans, ignore_spectators) the passages are open and the blockades disappear; round_start re-applies the state. Items persist per map in addons/data/bp_data.ini; phrases come from addons/translations/blockerpasses.phrases.txt. The SetCollisionBounds signature lives in the plugin's gamedata.jsonc. Requires the admin_system core. No exported methods.

Plugify port (fork) of **BlockerPasses** by ABKAM (Metamod (C++)).
Original: https://github.com/ABKAM2023/cs2-blockerpasses
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, admin_system, hudmenu (optional)

## Installation
Download `BlockerPasses-2.0.2.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/BlockerPasses`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

