#pragma once
// The plugin's settings (OBSMPPhysics.ini next to the DLL, Data\OBSE\Plugins), read at start and again with the reload key.

namespace obsmp
{
	struct Settings
	{
		bool enabled = true;
		float collisionDistance = 1500.0f;   // actors farther than this are simulated without collision (much cheaper)
		int maxActors = 10;            // most actors with collision, nearest first (the others simulated without it)

		// FSMP's world settings (its configs.xml defaults, as NV SMP Physics tuned them).
		int minFps = 180;              // the simulation's step rate at least: a frame is cut in equal steps no longer than this
		int maxSubSteps = 4;           // a frame longer than this many 60 Hz frames (4: 1/15 s) drops the rest
		int solverIterations = 10;     // Bullet's constraint solver iterations
		bool collisionThreadsOn = false;  // off: the collision tests all on the physics thread (experimental)
		int collisionThreads = -1;     // extra threads for the collision tests: -1 auto, 0 none
		bool asyncStep = true;         // the step runs on a physics thread while the game draws (the bones a frame behind it)
		float gravityScale = 1.0f;
		bool unclampedResets = true;   // turning faster than the limit below resets the physics
		float resetAngle = 120.0f;     // per second (FSMP's unclamped-reset-angle)

		bool wind = true;
		float windStrength = 2.0f;     // FSMP's wind strength: at the windiest weather, compared with 9.8 for gravity
		bool windInInteriors = false;
		float windDirection = -1.0f;   // degrees (0 = toward +Y, north); -1: drifts slowly (Oblivion's weather has none)

		bool collisions = true;         // off: the cloth passes through the body (for finding what pushes it)
		bool collideOncePerFrame = false; // the collision tests on a frame's first step only, later steps reuse its contacts
		bool ground = true;             // a ground plane at the actor's feet the cloth rests on (crouching, sitting)

		// The jitter filter (SkyrimBone::s_filter): smooths what is drawn of each simulated bone, not the physics.
		bool jitterFilter = true;
		float jitterSmoothing = 0.5f;   // 0 light .. 1 strong

		int reloadKey = 0x76;          // F7: settings read again from the INI, every system rebuilt
		int dumpKey = 0x77;            // F8: the player's and the nearest actors' 3D written to the log (names, types, skins)
		int logLevel = 2;              // 0 trace .. 4 errors only
	};

	extern Settings gSettings;

	void LoadSettings();
	void ApplySettings();   // pushes the world settings into the physics world
}
