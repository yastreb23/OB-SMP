# OB SMP Physics

HDT-SMP physics for The Elder Scrolls IV: Oblivion. It uses [FSMP](https://github.com/DaymareOn/hdtSMP64)'s
(Faster HDT-SMP) physics core and Bullet, built as a 32-bit [xOBSE](https://github.com/llde/xOBSE) plugin, and reads
the same SMP physics XML that Skyrim mods use. Capes, coats, skirts, hair and anything else rigged with physics bones
move with the character, collide with the body under them, and blow in the weather's wind.

This is a port of [NV SMP Physics](https://github.com/Balebandro/NV-SMP-Physics) (Fallout: New Vegas). That plugin
keeps FSMP's core unchanged and adds a glue layer for Gamebryo games. OB SMP Physics keeps that core, NV SMP
Physics' system builder and physics world, and replaces the game layer with Oblivion's.

> **Status: first test builds.** The engine addresses come from reverse-engineering Oblivion.exe 1.2.0.416. The
> plugin has not been run in the game yet. Please send your `OBSMPPhysics.log` (see Testing below).

## Requirements

- Oblivion 1.2.0.416 (GOG, Steam or retail patched to 1.2.0.416).
- [xOBSE](https://github.com/llde/xOBSE) (OBSE 0021 or later).
- Items made for SMP in Oblivion's format (see Making an item). The plugin does nothing on its own.

## Installing

Get `OBSMPPhysics-<n>.zip` from the latest successful run under the repository's **Actions** tab (artifact), or
from Releases once there are any. Install it with your mod manager, or copy its `Data` folder into the game folder.
That gives you `Data\OBSE\Plugins\OBSMPPhysics.dll` and `OBSMPPhysics.ini`. The log is `OBSMPPhysics.log` in the
game folder (next to `Oblivion.exe`).

## How it works

Once a frame the plugin runs before the game draws the world: right after every actor has been animated and
updated, at the main loop's call to `OSGlobals::Render` (`0x40D658`). There, it:

1. finds SMP items on the player and on the actors in the high and middle-high process lists. An SMP item is a
   skinned shape with the `HDT Skinned Mesh Physics Object` string extra data naming its physics XML;
2. builds a physics system for each new item from its XML (FSMP's reader, templates and constraints, unchanged) and
   drops the systems of items that were unequipped or whose actors left range;
3. reads the animated pose, steps Bullet and writes the simulated bones back. By default the step runs on a
   physics thread while the game draws (`bAsyncStep`), so the bones are one frame behind the body.

### What Oblivion needs that Skyrim does not

- **The physics file on the shapes.** When Oblivion equips a skinned armor piece, it keeps only the piece's
  skinned shapes. It moves them onto a fresh node under the skeleton's `SkinAttachment` node (or the skeleton
  root's parent) and drops the NIF's root, with the root's extra data. So the `HDT Skinned Mesh Physics Object`
  string has to be on the skinned shapes (all of them, or at least one). In Skyrim it is on the root.
- **The item's own bones.** Skyrim merges an item's physics bones into the actor's skeleton. Oblivion drops every
  node of the armor NIF that is not a shape. It binds each skin slot to the skeleton bone of the same name, and it
  leaves a slot whose bone the skeleton lacks pointing at the freed node. Each skinned shape therefore lists its
  skin slots in an `SMP Bones` string extra data, in skin-instance order:
  `name|parent;name|parent;...` (long lists go on in `SMP Bones 2`, `SMP Bones 3`...). The same list is used by
  NV SMP Physics. For shapes with that list, the plugin hooks the biped's "bone not found" branch (`0x47B465`) and
  points the slot at the skeleton root instead of the freed node. It then creates the item's own bones, places
  them at their bind pose against the body, and carries them on their parents. Shapes without the list keep the
  game's behavior.
- **Gamebryo skinning.** Vertex positions come from `NiGeometryData`, weights and triangles from the
  `NiSkinPartition` the renderer uses. These are laid out as in New Vegas, apart from the object offsets
  (`src/ob/NetImmerseUtils.h`, pinned with `static_assert`).
- **Wind.** Oblivion's weather has a wind speed (`Sky +0xC0`, which is what `GetWindSpeed` reads) but no
  direction. The wind blows from `fWindDirection`, or turns slowly when that is `-1`.

### Kept from NV SMP Physics

These features carry over unchanged:

- the ground plane at the actor's feet (`bGround`);
- case-insensitive bone and shape names;
- virtual legs (`<nv-virtual-legs>`);
- the jitter filter;
- the collision LOD (`fCollisionDistance`, `iMaxActors`);
- the bind wait for items the game has not bound yet;
- resets on teleports and fast turns.

Left out for now:

- the in-game menu (use the INI and the reload key);
- impact sounds (they are parsed, not played);
- placed objects' cloth, race hair and head parts, projectiles, explosions and the grab.

## Testing

1. Install the plugin and start the game. `OBSMPPhysics.log` should begin with the version line, a `probe:` line
   (`7/7 engine functions as expected` is the hoped-for result) and `render call hooked`. In game,
   `first frame: the render hook runs` follows.
2. Equip an SMP item. Expect `found '...'`, `building '...'`, then `'...': N bones, M collision bodies`. Every two
   seconds or so, `[stats]` lines report how many bones are simulated and how far they move.
3. **F8** writes the player's 3D (and the nearest actors') to the log. Every node and shape is listed with its
   class, extra data, and each skin slot: whether it is bound to a skeleton bone, a plugin node or a dropped node.
   This is what to send when something does not move or looks wrong.
4. **F7** reads the INI again and rebuilds every system.

If the plugin turns itself off after a fault, the log says where (`exception in the physics update ... stack: +XXXX`).
The offsets can be looked up in the `.pdb` that comes with each build.

## Making an item

An SMP item for Oblivion is an ordinary Oblivion skinned armor or clothing NIF (20.0.0.5) with, on each skinned
shape:

- `NiStringExtraData` **HDT Skinned Mesh Physics Object** = the XML's path under `Data`, as in Skyrim
  (e.g. `meshes\armor\mycape\cape.xml`);
- `NiStringExtraData` **SMP Bones** = `name|parent;...` for every skin slot, in order. A skeleton bone's parent
  can be left empty (`Bip01 Spine2|;`). For a physics bone, give the bone it hangs from (`Cape 1|Bip01 Spine2;
  Cape 2|Cape 1;...`).

The skin may use bones the skeleton lacks: those are the physics bones. Skyrim bone names in the XML must match the
names used in the NIF. A Skyrim item converted with sky2obl is retargeted to
Oblivion's skeleton. Automatic SMP support in sky2obl (keeping the physics bones, writing these strings and
converting the XML's bone names) is planned.

## Layout

| Path | What |
|---|---|
| `src/fsmp/hdtSkinnedMesh` | FSMP's physics core, as NV SMP Physics carries it (two 32-bit fixes) |
| `src/fsmp/XmlReader.*`, `XmlInspector` | FSMP's XML reader |
| `src/ob` | The Oblivion layer: `NetImmerseUtils.h` (scene graph layouts), `Game.h` (addresses), the system builder, bone, body and world from NV SMP Physics (comments marked "New Vegas:" are that plugin's additions to FSMP), the actor manager, and `main.cpp` (hooks, settings, the xOBSE entry points) |
| `src/compat` | Stand-ins for the CommonLibSSE types (`BSFixedString`, `BSTSmartPointer`) and the oneTBB calls the core uses |
| `extern/bullet` | Bullet 3.25 with FSMP's patches |

## Building

You need the Visual Studio 2022 C++ build tools (x86). Run `build.bat`, which produces `build\OBSMPPhysics.dll`.
GitHub Actions builds every push (`.github/workflows/build.yml`) and uploads the DLL, the INI and the PDB as an
artifact.

## Licence

GPL-3.0 with FSMP's modding exception (`LICENSE`, `EXCEPTIONS`), as FSMP and NV SMP Physics. Bullet is zlib and
XmlInspector is MIT.

Credits:

- FSMP by DaymareOn and contributors, from HydrogensaysHDT's HDT-SMP.
- NV SMP Physics by Balebandro.
- Bullet by Erwin Coumans.
- Engine layouts cross-checked against xOBSE's headers.
