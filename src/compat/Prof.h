#pragma once
// Section timers for the offline test (built with OBSMP_PROFILE): where a physics step's time goes.
// In the plugin they compile to nothing.
#ifdef OBSMP_PROFILE
#	include <windows.h>
namespace obsmp::prof
{
	enum Slot { ReadTransform, BonesAndBounds, Broadphase, Narrowphase, Solve, Integrate, WriteTransform, SkinColliders, PairTests, Count };
	inline const char* kNames[Count] = { "read transforms", "bones + bounds", "broadphase", "narrowphase (collision)", "constraint solve", "predict + integrate", "write transforms", "  of which: skin colliders", "  of which: pair tests" };
	inline double gTotal[Count] = {};
	struct Scope
	{
		Slot slot;
		LARGE_INTEGER t0;
		explicit Scope(Slot s) : slot(s) { QueryPerformanceCounter(&t0); }
		~Scope()
		{
			LARGE_INTEGER t1, f;
			QueryPerformanceCounter(&t1);
			QueryPerformanceFrequency(&f);
			gTotal[slot] += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
		}
	};
}
#	define OBSMP_PROF(slot) obsmp::prof::Scope obsmpProfScope##__LINE__(obsmp::prof::slot)
#else
#	define OBSMP_PROF(slot)
#endif
