# Experimental ultrawide support

Wonder 1.0.0 has been tested by the player at 3840x1080 (32:9) and
2560x1080 (approximately 21:9). The implementation changes world rendering,
terrain, in-game HUD, talking flowers, world-map rendering, cards and markers.
Some menu and transition details still require further validation. Other game
versions and aspect ratios have not been validated.

On Windows, select **Aspect ratio** in the launcher Settings page or in the
F1 Video menu. The choices are 16:9 (original), 21:9 and 32:9. Restart the
game for the change to take effect. The selection is saved as `aspect_ratio`
in `data/pc-settings.ini`; the game selects the corresponding generated mod
folders without renaming them. Existing extra mods remain separate.

The terrain adjustment changes a verified Wonder 1.0.0 constant in memory
before constructors run. It uses the selected aspect ratio, following the
approach of the emulator patch by StevensND and KeatonTheBot:
https://github.com/StevensND/switch-port-mods/tree/main/Super%20Mario%20Bros.%20Wonder/%5B010015100B514000%5D/Aspect%20Ratio/32.9

## Generate layout mods from your own game data

The world-map HUD and screen layouts use assets generated from the player's
extracted game data. Those generated files are not distributed with the source.
Game setup generates both aspect variants automatically. When an ultrawide
profile is launched, SuperWonders checks the layouts and regenerates missing
or outdated files using the installed game-build kit. No game assets are
included in the source or release. If this fails, the game reports an error;
check `data/nx.log` and repair the game-build kit or rerun game setup.

For development or troubleshooting, with Python and zstd installed, the
individual layout generators can also be run from the repository root,
substituting paths for the extracted RomFS, the existing `recomp-menu` mod's
RomFS folder, and the installation's mods folder:

```powershell
$rom = 'PATH_TO_DATA\extract\romfs'
$menu = 'PATH_TO_DATA\mods\recomp-menu\romfs'
$mods = 'PATH_TO_DATA\mods'
$rstb = "$menu\System\Resource\ResourceSizeTable.Product.100.rsizetable.zs"

python tools/wonder_uw_map_hud.py --romfs $rom --rstb $rstb --out "$mods\00-uw-map-hud-21-9" --aspect 2.37037037037
python tools/wonder_uw_screen_backgrounds.py --romfs $rom --menu-mod $menu --out "$mods\00-uw-screen-test-21-9" --aspect 2.37037037037
python tools/wonder_uw_map_hud.py --romfs $rom --rstb $rstb --out "$mods\00-uw-map-hud-32-9" --aspect 3.55555555556
python tools/wonder_uw_screen_backgrounds.py --romfs $rom --menu-mod $menu --out "$mods\00-uw-screen-test" --aspect 3.55555555556
```

The generated world-map resource-size table is based on the specified table.
If the menu mod is regenerated or other RomFS mods replace that table, their
resource-size changes must be combined; otherwise mod precedence can discard
entries. The automatic generator uses the active `recomp-menu` resource table.

The Windows aspect selector initializes the runtime switches on startup. The
Linux startup path has not yet been wired to the selector or tested for these
changes; the presence of the shared menu does not establish Linux support.
Diagnostic switches and the map proxy remain investigation tools.
