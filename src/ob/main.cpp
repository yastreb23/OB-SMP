// OB SMP Physics: FSMP (HDT-SMP for Skyrim) physics for The Elder Scrolls IV: Oblivion, as an xOBSE plugin.
// A port of NV SMP Physics (Fallout: New Vegas), which keeps FSMP's physics core and Bullet unchanged.
//
// Once a frame, right after the game finishes animating and updating every actor and before it draws (the main loop's
// call to OSGlobals::Render, see Game::kRenderCall), the actor manager finds the items with SMP physics files, and the
// physics world reads the animated pose, steps Bullet and writes the simulated bones back.
#include <share.h>
#include "ActorManager.h"
#include "Game.h"
#include "Settings.h"
#include "hdtSkyrimBone.h"
#include "hdtSkyrimPhysicsWorld.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <intrin.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

#ifndef OBSMP_VERSION
#	define OBSMP_VERSION "0.1.0-dev"
#endif

namespace obsmp
{
	Settings gSettings;

	static FILE* gLog = nullptr;
	static std::mutex gLogLock;
	static char gIniPath[MAX_PATH];
	static bool gFailed = false;
	static bool gLoading = false;          // between xOBSE's pre-load and post-load messages
	static bool gRenderHooked = false;
	static uint32_t gRenderTarget = Game::kRender;   // what the hooked call called (another plugin's hook is chained)
	static volatile long gBonesRedirected = 0;       // skin slots the bone hook moved to the skeleton root (any thread)
	static long gBonesRedirectedLogged = 0;

	bool LogEnabled(LogLevel level) { return gLog && (int)level >= gSettings.logLevel; }

	void LogLine(LogLevel level, const std::string& text)
	{
		if (!gLog)
			return;
		static const char* kNames[] = { "trace", "debug", "info", "warn", "error" };
		std::lock_guard<std::mutex> guard(gLogLock);
		fprintf(gLog, "[%s] %s\n", kNames[(int)level], text.c_str());
		fflush(gLog);
	}

	void Log(const char* fmt, ...)
	{
		if (!gLog)
			return;
		char buf[1024];
		va_list a;
		va_start(a, fmt);
		vsnprintf(buf, sizeof buf, fmt, a);
		va_end(a);
		LogLine(LogLevel::Info, buf);
	}

	// --- settings ---------------------------------------------------------------------------------

	struct Key
	{
		const char* name;
		float* f;
		int* i;
		bool* b;
	};

	static std::vector<Key> SettingKeys()
	{
		Settings& s = gSettings;
		return {
			{ "bEnabled", 0, 0, &s.enabled }, { "fCollisionDistance", &s.collisionDistance }, { "iMaxActors", 0, &s.maxActors },
			{ "iMinFps", 0, &s.minFps }, { "iMaxSubSteps", 0, &s.maxSubSteps }, { "iSolverIterations", 0, &s.solverIterations },
			{ "bCollisionThreads", 0, 0, &s.collisionThreadsOn }, { "iCollisionThreads", 0, &s.collisionThreads }, { "bAsyncStep", 0, 0, &s.asyncStep },
			{ "fGravityScale", &s.gravityScale }, { "bUnclampedResets", 0, 0, &s.unclampedResets }, { "fResetAngle", &s.resetAngle },
			{ "bWind", 0, 0, &s.wind }, { "fWindStrength", &s.windStrength }, { "bWindInInteriors", 0, 0, &s.windInInteriors },
			{ "fWindDirection", &s.windDirection },
			{ "bCollisions", 0, 0, &s.collisions }, { "bCollideOncePerFrame", 0, 0, &s.collideOncePerFrame },
			{ "bGround", 0, 0, &s.ground }, { "bJitterFilter", 0, 0, &s.jitterFilter }, { "fJitterSmoothing", &s.jitterSmoothing },
			{ "iReloadKey", 0, &s.reloadKey }, { "iDumpKey", 0, &s.dumpKey }, { "iLogLevel", 0, &s.logLevel },
		};
	}

	void ApplySettings()
	{
		auto* world = hdt::SkyrimPhysicsWorld::get();
		world->waitForStep();   // the world's settings and bodies are not changed under a running step
		auto& s = gSettings;
		s.minFps = std::clamp(s.minFps, 30, 240);
		s.maxSubSteps = std::clamp(s.maxSubSteps, 1, 16);
		s.solverIterations = std::clamp(s.solverIterations, 1, 64);
		world->m_timeTick = 1.0f / (float)s.minFps;
		world->m_maxSubSteps = s.maxSubSteps;
		world->getSolverInfo().m_numIterations = s.solverIterations;
		s.collisionThreads = std::clamp(s.collisionThreads, -1, 8);
		tbb::SetWorkers(!s.collisionThreadsOn ? 0 : s.collisionThreads < 0 ? tbb::AutoWorkers() : s.collisionThreads);
		world->setGravity(btVector3(0, 0, -9.8f * hdt::scaleSkyrim * s.gravityScale));
		world->m_unclampedResets = s.unclampedResets;
		world->m_unclampedResetAngle = s.resetAngle;
		world->m_enableWind = s.wind;
		world->m_windStrength = s.windStrength;
		world->disabled = !s.enabled;
		world->m_collisionsEnabled = s.collisions;
		world->m_collideOncePerFrame = s.collideOncePerFrame;
		s.jitterSmoothing = std::clamp(s.jitterSmoothing, 0.0f, 1.0f);
		hdt::SkyrimBone::SetJitterFilter(s.jitterFilter, s.jitterSmoothing);
		ActorManager::instance()->applyCollision();
	}

	void LoadSettings()
	{
		for (auto& k : SettingKeys()) {
			char b[64];
			GetPrivateProfileStringA("Main", k.name, "", b, 64, gIniPath);
			if (!b[0])
				continue;
			if (k.f)
				*k.f = (float)atof(b);
			else if (k.i)
				*k.i = (int)strtol(b, nullptr, 0);
			else
				*k.b = atoi(b) != 0;
		}
		ApplySettings();
	}

	// --- the frame -----------------------------------------------------------------------------------

	// Oblivion's weather has a wind speed but no direction (the grass and trees sway on a timer): the wind blows from
	// fWindDirection, or turns slowly when that is negative (a full turn in about six minutes, never quite steady).
	static void UpdateWind(float realDt)
	{
		auto* world = hdt::SkyrimPhysicsWorld::get();
		btVector3 target(0, 0, 0);
		static float angle = 0.7f, clock = 0.0f;
		clock += realDt;
		if (gSettings.windDirection >= 0.0f)
			angle = gSettings.windDirection * (3.14159265f / 180.0f);
		else
			angle += realDt * (6.2831853f / 360.0f) * (0.6f + 0.4f * std::sin(clock * 0.05f));
		if (gSettings.wind && (!Game::InInterior() || gSettings.windInInteriors)) {
			const float magnitude = std::clamp(Game::WindSpeed(), 0.0f, 1.0f);
			target = btVector3(std::sin(angle), std::cos(angle), 0) * (world->m_windStrength * hdt::scaleSkyrim * magnitude);
		}
		world->setWind(target);
	}

	// A key pressed this frame (down now, up last time), only while the game's window has the focus.
	static bool KeyPressed(int vk, bool& wasDown)
	{
		if (vk <= 0 || vk > 0xFE)
			return false;
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000) != 0;
		const bool pressed = down && !wasDown;
		wasDown = down;
		return pressed;
	}

	static void Update()
	{
		static bool reloadDown = false, dumpDown = false;
		const bool reload = KeyPressed(gSettings.reloadKey, reloadDown);
		const bool dump = KeyPressed(gSettings.dumpKey, dumpDown);

		// Real time, for the wind and for the menus where the game pauses but the character is still shown moving.
		static LARGE_INTEGER lastTick{};
		LARGE_INTEGER now, freq;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&freq);
		const float real = lastTick.QuadPart ? (float)((now.QuadPart - lastTick.QuadPart) / (double)freq.QuadPart) : 0.0f;
		lastTick = now;

		if (const long n = gBonesRedirected; n != gBonesRedirectedLogged) {
			logger::info("[SMP] {} skin slot(s) of SMP items so far had no skeleton bone of their name: put on the skeleton root for the plugin's own bones",
				n);
			gBonesRedirectedLogged = n;
		}
		if (gLoading)
			return;

		// The step started last frame on the physics thread (bAsyncStep): its bones are written before anything here
		// changes the world.
		auto* world = hdt::SkyrimPhysicsWorld::get();
		world->finishUpdate();
		if (std::string fault = world->takeStepFailure(); !fault.empty()) {
			LogLine(LogLevel::Error, "exception in the physics step (physics thread); OB SMP Physics is off for the rest of this session: " + fault);
			gFailed = true;
			return;
		}

		if (reload) {
			logger::info("[SMP] reload key: settings read again from {}, every system rebuilt", gIniPath);
			LoadSettings();
			ActorManager::instance()->clear();
		}
		if (dump)
			ActorManager::instance()->dumpScene(4);

		ActorManager::instance()->rebuildIfPending();
		if (!gSettings.enabled) {
			if (world->systemCount())
				ActorManager::instance()->clear();
			return;
		}

		ActorManager::instance()->update();
		UpdateWind(std::min(real, 0.25f));

		// The game's own time step: 0 while a menu pauses the game (the bones keep their last simulated pose). In dialogue
		// and the race menu the game pauses too but keeps showing the character moving: there the physics runs on real time.
		float dt = Game::FrameTime();
		const bool menuMode = Game::IsMenuMode();
		const uint32_t menu = menuMode ? Game::ActiveMenu() : 0;
		static uint32_t lastMenu = 0;
		if (menu != lastMenu) {
			logger::info("[SMP] menu {:#x} {}", menu ? menu : lastMenu, menu ? "open" : "closed");
			lastMenu = menu;
		}
		if (menuMode)
			dt = (menu == Game::kMenu_Dialog || menu == Game::kMenu_RaceSex) ? std::min(real, 0.05f) : 0.0f;

		// Every 120 frames: what the physics is doing. Before the step starts: the report reads the bodies.
		static int sinceReport = 0;
		if (++sinceReport >= 120) {
			sinceReport = 0;
			logger::info("[stats] frame time {:.4f} s, {} system(s) in the world", dt, world->systemCount());
			ActorManager::instance()->report();
		}

		if (gSettings.asyncStep) {
			if (dt > FLT_EPSILON)
				world->beginUpdate(std::min(dt, 0.25f), true);
		} else if (dt > FLT_EPSILON)
			world->doUpdate(std::min(dt, 0.25f));
	}

	// Where the fault was, filled by the exception filter: the code, the faulting address, and the return addresses inside
	// this DLL found on the stack (callers, as offsets from the DLL's base to look up in the pdb).
	static char gFailureWhere[512];

	static void DescribeAddress(char* out, size_t size, uintptr_t address)
	{
		HMODULE module = nullptr;
		char path[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module)) {
			GetModuleFileNameA(module, path, MAX_PATH);
			const char* name = strrchr(path, '\\');
			snprintf(out, size, "%s+%X", name ? name + 1 : path, (unsigned)(address - (uintptr_t)module));
		} else
			snprintf(out, size, "%08X", (unsigned)address);
	}

	static int RecordFailure(EXCEPTION_POINTERS* info)
	{
		char where[128];
		DescribeAddress(where, sizeof(where), (uintptr_t)info->ExceptionRecord->ExceptionAddress);
		int len = snprintf(gFailureWhere, sizeof(gFailureWhere), "code %08X at %s", (unsigned)info->ExceptionRecord->ExceptionCode, where);
		if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
			len += snprintf(gFailureWhere + len, sizeof(gFailureWhere) - len, " (%s %08X)",
				info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", (unsigned)info->ExceptionRecord->ExceptionInformation[1]);

		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&RecordFailure, &self);
		MODULEINFO mi = {};
		if (self && GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof(mi))) {
			uintptr_t lo = (uintptr_t)mi.lpBaseOfDll, hi = lo + mi.SizeOfImage;
			const uint32_t* sp = (const uint32_t*)info->ContextRecord->Esp;
			len += snprintf(gFailureWhere + len, sizeof(gFailureWhere) - len, "; stack:");
			for (int i = 0, found = 0; i < 512 && found < 12 && len < (int)sizeof(gFailureWhere) - 16; i++) {
				if (IsBadReadPtr(sp + i, 4)) break;
				uintptr_t v = sp[i];
				if (v > lo && v < hi) {
					len += snprintf(gFailureWhere + len, sizeof(gFailureWhere) - len, " +%X", (unsigned)(v - lo));
					found++;
				}
			}
		}
		return EXCEPTION_EXECUTE_HANDLER;
	}

	static void ReportFailure()
	{
		LogLine(LogLevel::Error, std::string("exception in the physics update; OB SMP Physics is off for the rest of this session: ") + gFailureWhere);
	}

	static void SafeUpdate()
	{
		__try {
			Update();
		} __except (RecordFailure(GetExceptionInformation())) {
			gFailed = true;
			ReportFailure();
		}
	}

	// Called from the hooked render call with every register saved: the game's SSE state (MXCSR) is kept as it was (the
	// physics sets flush-to-zero for its own math).
	static void __cdecl OnFrame()
	{
		static bool first = true;
		if (first) {
			first = false;
			Log("first frame: the render hook runs");
		}
		if (gFailed)
			return;
		const unsigned int csr = _mm_getcsr();
		SafeUpdate();
		_mm_setcsr(csr);
	}

	__declspec(naked) static void RenderHook()
	{
		__asm {
			pushad
			pushfd
			call OnFrame
			popfd
			popad
			jmp dword ptr [gRenderTarget]
		}
	}

	// The biped's "bone not found" branch (Game::kBoneNotFound): true when the slot was handled here (a shape with an
	// "SMP Bones" list: put on the skeleton root), false for the game's own branch. Any thread: touches only the skin.
	static bool __cdecl OnBoneNotFound(RE::NiSkinInstance* skin, uint32_t slot, void* biped, RE::NiGeometry* shape)
	{
		__try {
			if (!skin || !skin->bones || !biped || !shape || slot >= skin->boneCount)
				return false;
			if (!shape->FindStringExtra("SMP Bones"))
				return false;
			RE::NiAVObject* skeleton = *(RE::NiAVObject**)biped;
			if (!skeleton)
				return false;
			skin->bones[slot] = skeleton;
			InterlockedIncrement(&gBonesRedirected);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	static uint32_t gBoneNotFoundNext = Game::kBoneNotFoundNext;
	static uint32_t gBoneNotFoundDone = Game::kBoneNotFoundDone;

	__declspec(naked) static void BoneNotFoundHook()
	{
		__asm {
			pushad
			mov eax, [esp + 0x34]   // the BipedAnim: [esp+0x14] at the hook, past pushad's 0x20 bytes
			push esi                // the shape
			push eax
			push edi                // the slot
			push ebp                // the skin instance
			call OnBoneNotFound
			add esp, 0x10
			test al, al
			popad                   // leaves the flags alone
			jnz handled
			mov edx, [ebx + edi * 4]   // the game's two instructions the jump replaced
			mov ebp, [edx + 8]
			jmp dword ptr [gBoneNotFoundNext]
		handled:
			jmp dword ptr [gBoneNotFoundDone]
		}
	}

	static void WriteCode(uint32_t at, const uint8_t* bytes, size_t n)
	{
		DWORD old;
		VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old);
		memcpy((void*)at, bytes, n);
		VirtualProtect((void*)at, n, old, &old);
		FlushInstructionCache(GetCurrentProcess(), (void*)at, n);
	}

	static std::string HexBytes(uint32_t at, size_t n)
	{
		std::string s;
		for (size_t i = 0; i < n; ++i) s += std::format("{:02X}", ((const uint8_t*)at)[i]);
		return s;
	}

	// The render call: a call (E8) there is taken over, its target (the game's render, or another plugin's hook of it)
	// called after the plugin's frame.
	static bool HookRenderCall()
	{
		const uint8_t* p = (const uint8_t*)Game::kRenderCall;
		if (p[0] != 0xE8) {
			Log("render call at %08X is not a call (%s): the plugin stays off", Game::kRenderCall, HexBytes(Game::kRenderCall, 5).c_str());
			return false;
		}
		gRenderTarget = Game::kRenderCall + 5 + *(const int32_t*)(p + 1);
		if (gRenderTarget != Game::kRender)
			Log("render call at %08X goes to %08X, not the game's %08X: another plugin hooked it, chained", Game::kRenderCall, gRenderTarget, Game::kRender);
		uint8_t code[5] = { 0xE8 };
		*(int32_t*)(code + 1) = (int32_t)((uint32_t)&RenderHook - (Game::kRenderCall + 5));
		WriteCode(Game::kRenderCall, code, 5);
		return true;
	}

	static bool HookBoneNotFound()
	{
		if (memcmp((const void*)Game::kBoneNotFound, Game::kBoneNotFoundBytes, sizeof(Game::kBoneNotFoundBytes)) != 0) {
			Log("biped bone branch at %08X is not as expected (%s): SMP items' own bones are left as the game leaves them",
				Game::kBoneNotFound, HexBytes(Game::kBoneNotFound, 6).c_str());
			return false;
		}
		uint8_t code[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
		*(int32_t*)(code + 1) = (int32_t)((uint32_t)&BoneNotFoundHook - (Game::kBoneNotFound + 5));
		WriteCode(Game::kBoneNotFound, code, 6);
		return true;
	}

	// The probe: what the plugin expects at each engine function it calls, as found in the GOG and Steam 1.2.0.416
	// executables. A mismatch is logged, not fatal (heap replacers and other plugins patch some of these).
	static void ProbeExecutable()
	{
		struct Expect { uint32_t at; const char* bytes; const char* what; };
		static const Expect kExpect[] = {
			{ 0x0070BA70, "6AFF68DB749C00", "NiNode::CreateObject" },
			{ 0x006FF420, "53578BF98B4708", "NiObjectNET::SetName" },
			{ 0x00578F60, "6A016A00E8F791", "IsMenuMode" },
			{ 0x00578FE0, "6A016A00E87791", "GetActiveMenu" },
			{ 0x00748100, "FF254C7EB200", "NiFile::GetFile" },
			{ 0x0040C830, "6AFF6863A19A00", "OSGlobals::Render" },
			{ 0x00401F00, "8B4424046A0150", "FormHeap_Allocate" },
		};
		int ok = 0;
		for (auto& e : kExpect) {
			const size_t n = strlen(e.bytes) / 2;
			const std::string found = HexBytes(e.at, n);
			if (found == e.bytes) ++ok;
			else Log("probe: %s at %08X is %s, expected %s (patched by another plugin, or another executable)", e.what, e.at, found.c_str(), e.bytes);
		}
		// NiNode's vtable: IsNode (slot 2) returns this, AttachChild in slot 0x21.
		const uint32_t* vt = (const uint32_t*)0x00A7E38C;
		Log("probe: %d/%d engine functions as expected; NiNode vtable IsNode %08X (expected 007616D0), AttachChild %08X", ok, (int)(sizeof(kExpect) / sizeof(kExpect[0])),
			vt[2], vt[0x21]);
	}
}

// --- engine helpers the glue declares (NetImmerseUtils.h) ---------------------------------------------

namespace hdt
{
	RE::NiNode* createNode(const char* name)
	{
		auto* node = ((RE::NiNode*(__cdecl*)())Game::kNiNodeCreate)();
		if (node && name)
			setName(node, name);
		return node;
	}

	void setName(RE::NiAVObject* obj, const char* name)
	{
		typedef void(__thiscall * SetName_t)(void*, const char*);
		((SetName_t)Game::kSetName)(obj, name);
	}

	// The game's file objects, each step under SEH (no C++ objects here).
	static void* OpenGameFile(const char* rel)
	{
		__try {
			void* file = ((void*(__cdecl*)(const char*, uint32_t, uint32_t))Game::kNiFileGetFile)(rel, 0, 0x2800);
			if (file && !*((uint8_t*)file + 0x24)) {   // not good: not found
				typedef void(__thiscall * Destroy_t)(void*, bool);
				((Destroy_t)(*(void***)file)[0])(file, true);
				return nullptr;
			}
			return file;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}

	static uint32_t ReadGameFile(void* file, void* buffer, uint32_t bytes)
	{
		__try {
			typedef uint32_t(__cdecl * Read_t)(void*, void*, uint32_t);
			return (*(Read_t*)((uint8_t*)file + 0x04))(file, buffer, bytes);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	static void CloseGameFile(void* file)
	{
		__try {
			typedef void(__thiscall * Destroy_t)(void*, bool);
			((Destroy_t)(*(void***)file)[0])(file, true);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	// Loose first (Mod Organizer's virtual Data folder answers this), then through the game's FileFinder (loose files
	// again, then the BSA archives), as the game reads its own files.
	std::string readAllFile(const char* path)
	{
		std::string rel = path;
		for (auto& c : rel)
			if (c == '/')
				c = '\\';
		if (_strnicmp(rel.c_str(), "Data\\", 5) == 0)
			rel = rel.substr(5);

		std::ifstream in("Data\\" + rel, std::ios::binary);
		if (in)
			return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

		void* file = OpenGameFile(rel.c_str());
		if (!file)
			return {};
		std::string out;
		char chunk[0x10000];
		for (;;) {
			const uint32_t got = ReadGameFile(file, chunk, sizeof(chunk));
			if (!got || got > sizeof(chunk))
				break;
			out.append(chunk, got);
			if (out.size() > 0x1000000)
				break;
		}
		CloseGameFile(file);
		logger::info("[SMP] '{}' read from the game's archives ({} bytes)", rel, out.size());
		return out;
	}
}

// --- xOBSE plugin interface --------------------------------------------------------------------------

struct PluginInfo
{
	enum { kInfoVersion = 3 };
	uint32_t infoVersion;
	const char* name;
	uint32_t version;
};

struct OBSEInterface
{
	uint32_t obseVersion, oblivionVersion, editorVersion, isEditor;
	void* registerCommand;
	void* setOpcodeBase;
	void* (*QueryInterface)(uint32_t id);
	uint32_t (*GetPluginHandle)();
};

struct OBSEMessage
{
	const char* sender;
	uint32_t type;
	uint32_t dataLen;
	void* data;
};

struct OBSEMessagingInterface
{
	uint32_t version;
	bool (*RegisterListener)(uint32_t listener, const char* sender, void (*handler)(OBSEMessage*));
};

static constexpr uint32_t kInterface_Messaging = 4;
static constexpr uint32_t kOblivion_1_2_416 = 0x010201A0;
enum : uint32_t { kMessage_PostLoad = 0, kMessage_ExitGame = 1, kMessage_ExitToMainMenu = 2, kMessage_LoadGame = 3, kMessage_PreLoadGame = 6, kMessage_PostLoadGame = 8 };

// A load replaces every actor's 3D: the systems go while the old 3D is still whole, and nothing is scanned or built until
// the load is over. Going back to the main menu drops them too.
static void OnOBSEMessage(OBSEMessage* msg)
{
	using namespace obsmp;
	switch (msg->type) {
	case kMessage_PreLoadGame:
		gLoading = true;
		ActorManager::instance()->clear();
		Log("loading a save: physics cleared");
		break;
	case kMessage_ExitToMainMenu:
		ActorManager::instance()->clear();
		Log("main menu: physics cleared");
		break;
	case kMessage_PostLoadGame:
		gLoading = false;
		Log("save loaded: physics resumes");
		break;
	}
}

static HMODULE gSelf = nullptr;

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
		gSelf = h;
	return TRUE;
}

extern "C"
{
	__declspec(dllexport) bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info)
	{
		info->infoVersion = PluginInfo::kInfoVersion;
		info->name = "OB SMP Physics";
		info->version = 1;
		return !obse->isEditor && obse->oblivionVersion == kOblivion_1_2_416;
	}

	__declspec(dllexport) bool OBSEPlugin_Load(const OBSEInterface* obse)
	{
		using namespace obsmp;
		if (obse->isEditor)
			return false;
		GetModuleFileNameA(gSelf, gIniPath, MAX_PATH);
		if (char* dot = strrchr(gIniPath, '.'))
			strcpy_s(dot, gIniPath + MAX_PATH - dot, ".ini");
		gLog = _fsopen("OBSMPPhysics.log", "w", _SH_DENYWR);   // in the game folder, readable while the game runs
		Log("OB SMP Physics %s (FSMP physics core, Bullet %d), xOBSE %u, Oblivion %08X", OBSMP_VERSION, btGetVersion(), obse->obseVersion, obse->oblivionVersion);
		LoadSettings();
		ProbeExecutable();
		if (obse->obseVersion >= 17)
			if (auto messaging = (OBSEMessagingInterface*)obse->QueryInterface(kInterface_Messaging))
				messaging->RegisterListener(obse->GetPluginHandle(), "OBSE", OnOBSEMessage);
		gRenderHooked = HookRenderCall();
		if (!gRenderHooked)
			return true;   // loaded, but off: nothing else is patched
		Log("render call hooked at %08X", Game::kRenderCall);
		if (HookBoneNotFound())
			Log("biped bone branch hooked at %08X (SMP items' own bones)", Game::kBoneNotFound);
		return true;
	}
}
