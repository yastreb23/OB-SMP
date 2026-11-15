#pragma once
// Oblivion 1.2.0.416 addresses and calls the plugin uses (Steam, GOG and retail 1.2.0.416 share the code layout), found in
// Oblivion.exe and checked against xOBSE's headers where xOBSE has them. Every hook site's original bytes are checked
// before it is patched (see main.cpp): a different executable leaves the plugin off rather than patching blind.
#include "NetImmerseUtils.h"

namespace Game
{
	template <class T>
	inline T& At(const void* p, size_t offset) { return *(T*)((char*)p + offset); }

	// --- Engine functions -------------------------------------------------------------------------------------------
	constexpr uint32_t kFormHeapAllocate = 0x00401F00;   // MemoryHeap allocate (cdecl size), as xOBSE's FormHeap_Allocate
	constexpr uint32_t kFormHeapFree = 0x00401F20;       // and free (cdecl pointer)
	constexpr uint32_t kNiNodeCreate = 0x0070BA70;       // NiNode::CreateObject: allocates 0xDC, NiNode::NiNode(0) (0x70B780)
	constexpr uint32_t kSetName = 0x006FF420;            // NiObjectNET::SetName(const char*): frees the old name, copies the new
	// NiFile::GetFile(path, mode, bufferSize) through Gamebryo's file-creation hook (0xB27E4C), which Oblivion points at
	// its FileFinder (0x431440 -> 0x430EF0): loose files first, then the BSA archives. Returns a BSFile (or ArchiveFile):
	// good flag +0x24, read proc +0x04 (cdecl (file, buffer, bytes) -> bytes read), destructor vtable slot 0.
	constexpr uint32_t kNiFileGetFile = 0x00748100;
	constexpr uint32_t kIsMenuMode = 0x00578F60;         // bool IsMenuMode() (xOBSE)
	constexpr uint32_t kGetActiveMenu = 0x00578FE0;      // the active menu's type (InterfaceManager, kMenuType_*), 0 for none

	// --- The frame ----------------------------------------------------------------------------------------------------
	// Main loop frame (0x40D800): process manager updates (0x40DE71 ProcessManager 0x67ACA0 with the frame time: the
	// actors' animation), the world update, then 0x40D4D0, which renders the world through OSGlobals::Render 0x40C830
	// (the call at 0x40D658). The plugin's frame runs at that call: every actor animated and updated, nothing drawn yet.
	constexpr uint32_t kRenderCall = 0x0040D658;
	constexpr uint32_t kRender = 0x0040C830;

	// --- Skinned armor without some of its bones in the skeleton -----------------------------------------------------
	// BipedAnim::AttachSkinnedObject (0x47B5B0) gives each skinned armor piece a fresh node under the skeleton's
	// "SkinAttachment" node (or under the skeleton root's parent), and 0x47B090 moves the piece's skinned shapes onto it.
	// It drops the NIF's other children (its bone nodes), then binds each skin slot to the skeleton bone of the same
	// name (0x6F94A0). A slot whose bone the skeleton lacks is left on the NIF's own node, which was just dropped (and
	// freed): the branch at 0x47B465 only prints "Bone '%s' not found for part". The plugin hooks that branch: for a shape
	// with an "SMP Bones" list, the slot goes to the skeleton root instead, as New Vegas does natively, and the plugin
	// makes the item's own bones from the list (SkyrimSystemCreator::createExtraBoneNode). Other shapes keep the game's way.
	// At the branch: ebp the skin instance, edi the slot, esi the shape, ebx the slot array, [esp+0x14] the BipedAnim
	// (its skeleton root at +0). The game's branch starts "mov edx,[ebx+edi*4]; mov ebp,[edx+8]" and joins at 0x47B50B.
	constexpr uint32_t kBoneNotFound = 0x0047B465;
	constexpr uint32_t kBoneNotFoundNext = 0x0047B46B;   // after the two instructions the hook replaces
	constexpr uint32_t kBoneNotFoundDone = 0x0047B50B;   // the loop's next slot
	constexpr uint8_t kBoneNotFoundBytes[] = { 0x8B, 0x14, 0xBB, 0x8B, 0x6A, 0x08 };

	// --- Globals --------------------------------------------------------------------------------------------------------
	constexpr uint32_t kPlayer = 0x00B333C4;           // PlayerCharacter* (xOBSE g_thePlayer)
	constexpr uint32_t kTES = 0x00B333A0;              // TES*: interior cell +0x34, Sky* +0x5C
	constexpr uint32_t kProcessManager = 0x00B3BD00;   // ActorProcessManager: middle-high actors +0x00, high actors +0x5C (tList)
	constexpr uint32_t kFrameTime = 0x00B33E9C;        // the frame's time step in seconds (xOBSE TimeInfo 0xB33E90 +0x0C)

	inline void* Player() { return *(void**)kPlayer; }
	inline float FrameTime() { return *(float*)kFrameTime; }
	inline bool IsMenuMode() { return ((bool(__cdecl*)())kIsMenuMode)(); }
	inline uint32_t ActiveMenu() { return ((uint32_t(__cdecl*)())kGetActiveMenu)(); }

	enum : uint32_t { kMenu_Dialog = 0x3F1, kMenu_RaceSex = 0x40C, kMenu_Loading = 0x3EF };

	inline bool InInterior()
	{
		void* tes = *(void**)kTES;
		return tes && At<void*>(tes, 0x34);
	}

	// The weather's wind, 0..1, blended between weathers (Sky::UpdateWind 0x53FF90 writes Sky +0xC0 from TESWeather +0x48
	// over 255; the GetWindSpeed console function reads it there). Oblivion's weather has no wind direction.
	inline float WindSpeed()
	{
		void* tes = *(void**)kTES;
		void* sky = tes ? At<void*>(tes, 0x5C) : nullptr;
		return sky ? At<float>(sky, 0xC0) : 0.0f;
	}

	// A reference's place (TESObjectREFR +0x2C) and its 3D (+0x3C, what TESObjectREFR::GetNiNode, vtable slot 0x55,
	// returns; for the player in first person the game returns the first-person model instead).
	inline RE::NiPoint3 RefPosition(const void* ref) { return At<RE::NiPoint3>(ref, 0x2C); }
	inline RE::NiNode* Get3D(void* ref)
	{
		auto* obj = ref ? At<RE::NiAVObject*>(ref, 0x3C) : nullptr;
		return obj ? obj->AsNode() : nullptr;
	}

	// The player's first-person model (PlayerCharacter +0x5D0), and whether the camera is in third person (+0x588).
	inline RE::NiNode* PlayerFirstPerson3D(void* player)
	{
		auto* obj = player ? At<RE::NiAVObject*>(player, 0x5D0) : nullptr;
		return obj ? obj->AsNode() : nullptr;
	}
	inline bool PlayerThirdPerson(void* player) { return player && At<uint8_t>(player, 0x588) != 0; }

	// A reference's form type (TESForm +0x04): ACHR (a character) 0x32, ACRE (a creature) 0x33.
	inline bool IsActorRef(const void* ref)
	{
		const uint8_t type = At<uint8_t>(ref, 0x04);
		return type == 0x32 || type == 0x33;
	}

	// Every actor in the high and middle-high process lists (the ones the game animates and draws near the player), the
	// player excluded. The lists are tLists: the first node inline (item, next).
	template <class F>
	inline void ForEachProcessActor(F&& f)
	{
		struct Node { void* item; Node* next; };
		char* pm = (char*)kProcessManager;
		for (size_t head : { (size_t)0x5C, (size_t)0x00 }) {
			int guard = 0;
			for (const Node* n = (const Node*)(pm + head); n && guard < 4096; n = n->next, ++guard)
				if (n->item && n->item != Player() && IsActorRef(n->item))
					f(n->item);
		}
	}

	// The class name of a NetImmerse object (its NiRTTI: vtable slot 1, name at +0), for the log.
	inline const char* RttiName(RE::NiObject* obj)
	{
		typedef const char** (__thiscall * GetRTTI_t)(RE::NiObject*);
		const char** rtti = obj ? ((GetRTTI_t)obj->vtbl[1])(obj) : nullptr;
		return rtti && *rtti ? *rtti : "?";
	}
}
