#pragma once
// Which actors and items have physics. Each frame it walks the player's and the high- and middle-high-process actors' 3D
// for items carrying an SMP physics file (the "HDT Skinned Mesh Physics Object" string extra data on the item's shapes:
// Oblivion keeps a skinned armor's shapes but not its NIF root), builds a system for each new one, and drops the systems
// of items that were unequipped or of actors that left range.
//
// Ported from NV SMP Physics' ActorManager, without what is New Vegas only (race hair and head parts, the race menu's
// stand-ins, placed objects' cloth, projectiles, explosions, the grab, impact sounds).
#include "hdtSkyrimSystem.h"

#include <map>
#include <tuple>

namespace obsmp
{
	struct ActorStats
	{
		int actors = 0;        // actors with physics items in range
		int systems = 0;       // systems in the world
		int failed = 0;        // items whose physics file could not be built (see the log)
	};

	class ActorManager
	{
	public:
		static ActorManager* instance();

		// Once per frame, before the world steps.
		void update();
		// Drops every system (settings reloaded, game load).
		void clear();
		// Writes each system's state to the log.
		void report();
		// After a settings change: marks every system for a rebuild if a built-in collision setting changed.
		void applyCollision();
		// Once per frame, before update: does that rebuild.
		void rebuildIfPending();
		// The player's and the nearest actors' 3D to the log: every node and shape, its class, and each skinned shape's
		// slots (which skeleton bone each is bound to, or that it is not), for finding out what the game made of an item.
		void dumpScene(int actors);

		ActorStats stats;

	private:
		struct Item
		{
			RE::NiPointer<RE::NiNode> skeleton;   // the actor's 3D root
			RE::NiPointer<RE::NiAVObject> model;  // the item's node under it (the one the game made for the armor piece)
			std::string file;
			RE::BSTSmartPointer<hdt::SkyrimSystem> system;
			unsigned frame = 0;                   // last frame it was seen
			unsigned buildAt = 0;                 // frame of the next build
			int builds = 0;
			bool failed = false;
			bool waiting = false;                 // the game has not bound its skin yet
			float jointGapPhys = 0, jointGapDrawn = 0;   // the widest a constraint's two pivots were apart since the last report
		};

		static constexpr unsigned kSettleFrames = 10, kRetryFrames = 30;
		static constexpr int kMaxBuilds = 4;

		void build(Item& item);
		static bool rebound(const Item& item);   // the game re-pointed a skin slot the system had bound
		static bool skinsBound(RE::NiAVObject* obj, int depth = 0);
		void scan(RE::NiNode* root, RE::NiAVObject* obj, int depth);
		void found(RE::NiNode* root, RE::NiAVObject* model, const char* file);
		void drop(Item& item, bool detached);

		std::map<std::pair<void*, void*>, Item> m_items;   // by (skeleton, model)
		unsigned m_frame = 0;
		float m_lastRootRadius = 0;            // the player root's last finite bound radius (see KeepRadius)
		std::string m_playerRootName;          // what the last scan took as the player's 3D (for the log)
		int m_builtGround = -1;                // the ground setting the systems were built with
		bool m_rebuildPending = false;         // a collision setting changed: rebuild at the next update
		unsigned m_impactsDropped = 0;         // impact sounds the physics found (not played in Oblivion yet)
	};
}
