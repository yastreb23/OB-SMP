// Ported from NV SMP Physics (src/nv/ActorManager.cpp), GPL-3.0 with FSMP's modding exception: the item scan, the
// build with its retries, the bind wait and the collision LOD are that plugin's; the New Vegas only parts are left out
// (see ActorManager.h) and the scene walk is Oblivion's.
#include "ActorManager.h"
#include "Game.h"
#include "Settings.h"
#include "hdtSkyrimPhysicsWorld.h"
#include "hdtSkyrimBone.h"

#include <algorithm>
#include <unordered_set>

namespace obsmp
{
	static const char* kPhysicsKey = "HDT Skinned Mesh Physics Object";

	// smpdress --proxy collision stand-ins (shapes named "SMP Proxy:...") are hidden (app-culled) when their physics is
	// built (generateMeshBody), and shown again whenever the item's physics goes (the model may be attached again as it
	// is). The player's first-person model gets no physics: hidden there every frame.
	static void SetProxiesHidden(RE::NiAVObject* obj, bool hidden, int depth = 0)
	{
		if (!obj || depth > 48)
			return;
		if (auto* node = obj->AsNode()) {
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				SetProxiesHidden(node->children[i], hidden, depth + 1);
			return;
		}
		if (!obj->m_name || _strnicmp(obj->m_name, "SMP Proxy:", 10) != 0)
			return;
		if (hidden)
			obj->flags |= RE::NiAVObject::kFlag_AppCulled;
		else
			obj->flags &= ~RE::NiAVObject::kFlag_AppCulled;
	}

	// Skin slots the game left on dropped nodes: until it binds an item's skin, or for good in Oblivion for a slot whose bone
	// the skeleton lacks (the plugin's hook moves those to the skeleton root for shapes with an "SMP Bones" list, see
	// Game::kBoneNotFound), a slot can point at a freed node. Such slots are never written: while the plugin waits for
	// the bind, each slot not on a live skeleton bone gets a node of the plugin's own with the same name, put at its rest
	// pose every frame (NV SMP Physics' PlaceDroppedBones). Never released: the game may still hold them in a skin's slots.
	static std::vector<RE::NiPointer<RE::NiNode>> g_waitNodes;
	static std::unordered_set<const RE::NiAVObject*> g_waitSet;

	// A skin slot's node, under SEH (a slot left from an earlier attach may point at a freed object): null if unreadable.
	static RE::NiAVObject* ReadSlot(RE::NiAVObject** bones, uint32_t i)
	{
		__try {
			return bones[i];
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}

	// The node's name into `out`, under SEH: false if unreadable or unnamed.
	static bool CopyName(RE::NiAVObject* node, char* out, size_t size)
	{
		__try {
			const char* n = node->m_name;
			if (!n) return false;
			size_t i = 0;
			for (; i + 1 < size && n[i]; ++i) out[i] = n[i];
			out[i] = 0;
			return i > 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Whether the node is a live skeleton bone: under `actor`, outside `model` (the item's subtree). Under SEH, and at most
	// 256 parents up: a stale slot can point at reused memory whose parent chain loops.
	static bool IsLiveBone(RE::NiAVObject* node, RE::NiAVObject* actor, RE::NiAVObject* model)
	{
		__try {
			int depth = 0;
			for (RE::NiAVObject* p = node; p && depth < 256; p = p->parent, ++depth) {
				if (p == model) return false;
				if (p == actor) return true;
			}
			return false;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// A node by name under `obj`, outside `skip` (the item's subtree).
	static RE::NiAVObject* FindOutside(RE::NiAVObject* obj, RE::NiAVObject* skip, const char* name, int depth = 0)
	{
		if (!obj || obj == skip || depth > 64)
			return nullptr;
		if (obj->m_name && !_stricmp(obj->m_name, name))
			return obj;
		if (auto* node = obj->AsNode())
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				if (auto* found = FindOutside(node->children[i], skip, name, depth + 1))
					return found;
		return nullptr;
	}

	static void PlaceDroppedBones(RE::NiAVObject* obj, RE::NiNode* actor, RE::NiAVObject* model, int depth = 0)
	{
		if (!obj || depth > 64)
			return;
		if (auto* g = hdt::castNiGeometry(obj)) {
			auto* skin = g->skinInstance;
			if (!g->FindStringExtra("SMP Bones") || !skin || !skin->bones || !skin->skinData || !skin->skinData->boneData)
				return;
			const uint32_t n = skin->skinData->boneCount;
			std::vector<RE::NiAVObject*> nodes(n, nullptr), skeleton(n, nullptr);
			int reference = -1;
			bool liveReference = false;
			for (uint32_t i = 0; i < n; ++i) {
				RE::NiAVObject* slot = ReadSlot(skin->bones, i);
				if (!slot) continue;
				char name[128];
				const bool named = CopyName(slot, name, sizeof(name));
				if (!g_waitSet.count(slot)) {
					if (IsLiveBone(slot, actor, nullptr)) {
						skeleton[i] = slot;   // the best reference, left as it is
						if (!liveReference) { reference = (int)i; liveReference = true; }
						continue;
					}
					RE::NiPointer<RE::NiNode> own(hdt::createNode(named ? name : "SMP Wait"));
					if (!own) continue;
					g_waitNodes.push_back(own);
					g_waitSet.insert(own.get());
					skin->bones[i] = own.get();
					slot = own.get();
				}
				nodes[i] = slot;
				if (named)
					if (RE::NiAVObject* bone = FindOutside(actor, model, name)) {
						skeleton[i] = bone;
						if (reference < 0) reference = (int)i;
					}
			}
			if (reference < 0)
				return;   // no skeleton bone to place them from: they stay at the origin (finite, so no broken bound)
			const auto* data = skin->skinData->boneData;
			const RE::NiTransform skinToWorld = skeleton[reference]->world * data[reference].skinToBone;
			for (uint32_t i = 0; i < n; ++i)
				if (nodes[i])
					nodes[i]->world = skeleton[i] ? skeleton[i]->world : skinToWorld * data[i].skinToBone.Invert();
			return;
		}
		if (auto* node = obj->AsNode())
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				PlaceDroppedBones(node->children[i], actor, model, depth + 1);
	}

	// A safety net for the player: while the root's world bound radius is not finite (one broken bone in a skinned item
	// makes it so, and the game then culls the whole player), the last good radius is written back so the view test
	// passes. Oblivion keeps the bound in the object (NiAVObject +0x20); the game recomputes it each frame.
	static bool KeepRadius(RE::NiAVObject* obj, float& lastGood)
	{
		__try {
			RE::NiBound& b = obj->worldBound;
			if (!std::isfinite(b.center.x) || !std::isfinite(b.center.y) || !std::isfinite(b.center.z))
				return false;
			if (std::isfinite(b.radius) && b.radius > 0)
				lastGood = b.radius;
			else if (lastGood > 0)
				b.radius = lastGood;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	ActorManager* ActorManager::instance()
	{
		static ActorManager manager;
		return &manager;
	}

	void ActorManager::drop(Item& item, bool detached)
	{
		if (item.system) {
			item.system->m_itemDetached = detached;
			hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(item.system.get());
			item.system = nullptr;
		}
		SetProxiesHidden(item.model.get(), false);
	}

	void ActorManager::found(RE::NiNode* root, RE::NiAVObject* model, const char* file)
	{
		auto key = std::make_pair((void*)root, (void*)model);
		auto it = m_items.find(key);
		if (it != m_items.end() && it->second.file != file) {
			// The same node now names another file: rebuild.
			drop(it->second, false);
			m_items.erase(it);
			it = m_items.end();
		}
		if (it == m_items.end()) {
			Item item;
			item.skeleton = root;
			item.model = model;
			item.file = file;
			item.buildAt = m_frame + kSettleFrames;
			it = m_items.emplace(key, std::move(item)).first;
			logger::info("[SMP] found '{}' on {:p} under '{}'", file, (void*)model, root->GetName());
		}
		Item& item = it->second;
		item.frame = m_frame;
		// The game re-pointed the item's skin since it was built (detached and attached again while the plugin was not
		// looking, as in a menu): the slots that drew from the cloth bones now draw from the skeleton. Built again.
		if (item.system && rebound(item)) {
			logger::warn("[SMP] {:p}: the game re-pointed its skin (detached and attached again), building again", (void*)model);
			drop(item, true);
			item.builds = 0;
			item.failed = false;
			item.buildAt = m_frame + kSettleFrames;
		}
		// The game has not bound the item's skin (yet, or any more): no system until it has, then a few frames to settle.
		if (!item.failed && !skinsBound(model)) {
			if (item.system) {
				logger::warn("[SMP] {:p}: the game unbound its skin, physics paused until it is bound again", (void*)model);
				drop(item, true);
				item.builds = 0;
			}
			if (!item.waiting)
				logger::info("[SMP] {:p}: waiting for the game to bind its skin", (void*)model);
			PlaceDroppedBones(model, root, model);
			item.waiting = true;
			item.buildAt = m_frame + kSettleFrames;
			return;
		}
		item.waiting = false;
		if (!item.system && !item.failed && m_frame >= item.buildAt)
			build(item);
	}

	bool ActorManager::rebound(const Item& item)
	{
		for (auto& s : item.system->m_boundSlots)
			if (!s.holds())
				return true;
		return false;
	}

	// Whether the game has bound the skin of every shape that lists its own bones: a skin with all its slots pointing at
	// nodes.
	bool ActorManager::skinsBound(RE::NiAVObject* obj, int depth)
	{
		if (!obj || depth > 64)
			return true;
		if (auto* g = hdt::castNiGeometry(obj)) {
			if (!g->FindStringExtra("SMP Bones"))
				return true;
			auto* skin = g->skinInstance;
			if (!skin || !skin->bones || !skin->boneCount)
				return false;
			for (uint32_t i = 0; i < skin->boneCount; ++i)
				if (!skin->bones[i])
					return false;
			return true;
		}
		if (auto* node = hdt::castNiNode(obj))
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				if (!skinsBound(node->children[i], depth + 1))
					return false;
		return true;
	}

	// The game attaches an item over several frames, so an item is built once it has been there kSettleFrames, and built
	// again if the build found the item's own bones missing, up to kMaxBuilds times; the last try is kept as it is.
	void ActorManager::build(Item& item)
	{
		auto* root = item.skeleton.get();
		auto* model = item.model.get();
		const char* file = item.file.c_str();
		++item.builds;
		hdt::DefaultBBP::PhysicsFile_t physicsFile{ item.file, {} };
		hdt::SkyrimSystemCreator creator;
		creator.m_ground = gSettings.ground;
		m_builtGround = gSettings.ground;
		logger::info("[SMP] building '{}' for {:p} on '{}' (try {})", file, (void*)model, root->GetName(), item.builds);
		auto system = creator.createOrUpdateSystem(root, model, &physicsFile, {}, nullptr);
		if (!system) {
			item.failed = true;
			logger::error("[SMP] '{}' produced no physics for {:p}", file, (void*)model);
			return;
		}
		if (!creator.complete() && item.builds < kMaxBuilds) {
			logger::warn("[SMP] {:p}: incomplete, building again in {} frames", (void*)model, kRetryFrames);
			system = nullptr;   // gives its skin slots back
			item.buildAt = m_frame + kRetryFrames;
			return;
		}
		item.system = system;
		hdt::SkyrimPhysicsWorld::get()->addSkinnedMeshSystem(item.system.get());
		logger::info("[SMP] '{}': {} bones, {} collision bodies", file, item.system->getBones().size(), item.system->meshes().size());
	}

	void ActorManager::scan(RE::NiNode* root, RE::NiAVObject* obj, int depth)
	{
		if (!obj || depth > 48)
			return;
		// Not the actor's own root. On a shape (where Oblivion keeps it: a skinned armor's NIF root is not attached), the
		// item is the node holding it, the one the game made for the armor piece.
		if (obj->extraDataCount && obj != root) {
			if (const char* file = obj->FindStringExtra(kPhysicsKey)) {
				RE::NiAVObject* model = obj->AsNode() ? obj : (obj->parent ? static_cast<RE::NiAVObject*>(obj->parent) : obj);
				if (file[0] && model != root)
					found(root, model, file);
				if (obj->AsNode())
					return;   // one system per item: its shapes are part of it
			}
		}
		if (auto* node = obj->AsNode()) {
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				scan(root, node->children[i], depth + 1);
		}
	}

	// How far apart each constraint's two pivots are, in the physics and as drawn, the widest per item until the next report.
	static void SampleJointGaps(hdt::SkyrimSystem* system, float& phys, float& drawn)
	{
		std::vector<hdt::BoneScaleConstraint*> all;
		for (auto& c : system->constraints()) all.push_back(c.get());
		for (auto& group : system->constraintGroups())
			for (auto& c : group->m_constraints) all.push_back(c.get());
		for (auto* c : all) {
			if (!c->getConstraint() || c->getConstraint()->getConstraintType() != D6_SPRING_2_CONSTRAINT_TYPE) continue;
			auto* g = static_cast<btGeneric6DofSpring2Constraint*>(c->getConstraint());
			auto* a = static_cast<hdt::SkyrimBone*>(c->m_boneA);
			auto* b = static_cast<hdt::SkyrimBone*>(c->m_boneB);
			if (&g->getRigidBodyA() != &a->m_rig) std::swap(a, b);
			const btVector3 pa = (g->getRigidBodyA().getWorldTransform() * g->getFrameOffsetA()).getOrigin();
			const btVector3 pb = (g->getRigidBodyB().getWorldTransform() * g->getFrameOffsetB()).getOrigin();
			phys = std::max(phys, (float)(pa - pb).length());
			if (!a->m_node || !b->m_node) continue;
			const btTransform ra = hdt::convertNi(a->m_node->world).asTransform() * a->m_localToRig;
			const btTransform rb = hdt::convertNi(b->m_node->world).asTransform() * b->m_localToRig;
			drawn = std::max(drawn, (float)((ra * g->getFrameOffsetA()).getOrigin() - (rb * g->getFrameOffsetB()).getOrigin()).length());
		}
	}

	void ActorManager::update()
	{
		++m_frame;
		const auto& s = gSettings;
		for (auto& [key, item] : m_items)
			if (item.system)
				SampleJointGaps(item.system.get(), item.jointGapPhys, item.jointGapDrawn);

		// The player (its third-person 3D, animated in first person too), then every actor in the high and middle-high
		// process lists with its 3D loaded, nearest first. None is dropped for distance or count: the far ones run without
		// collision (see the collision LOD below).
		void* player = Game::Player();
		RE::NiNode* playerRoot = player ? Game::Get3D(player) : nullptr;
		m_playerRootName = playerRoot ? playerRoot->GetName() : "(none)";
		if (playerRoot)
			KeepRadius(playerRoot, m_lastRootRadius);
		if (RE::NiNode* firstPerson = player ? Game::PlayerFirstPerson3D(player) : nullptr; firstPerson && firstPerson != playerRoot)
			SetProxiesHidden(firstPerson, true);
		std::vector<std::pair<float, RE::NiNode*>> actors;
		if (playerRoot)
			actors.push_back({ 0.0f, playerRoot });
		const RE::NiPoint3 playerPos = playerRoot ? playerRoot->world.translate : RE::NiPoint3{};
		Game::ForEachProcessActor([&](void* ref) {
			if (auto* root = Game::Get3D(ref))
				if (std::none_of(actors.begin(), actors.end(), [root](auto& a) { return a.second == root; }))
					actors.push_back({ (root->world.translate - playerPos).Length(), root });
		});
		std::sort(actors.begin(), actors.end(), [](auto& a, auto& b) { return a.first < b.first; });
		for (auto& a : actors)
			scan(a.second, a.second, 0);

		// Items not seen this frame: unequipped, unloaded, or their actor left range.
		int failed = 0;
		std::vector<void*> roots;
		for (auto it = m_items.begin(); it != m_items.end();) {
			if (it->second.frame != m_frame) {
				logger::info("[SMP] {:p} gone (unequipped, or its actor's 3D went)", it->first.second);
				drop(it->second, true);
				it = m_items.erase(it);
				continue;
			}
			failed += it->second.failed;
			// Collision LOD: past the collision distance, or beyond the nearest iMaxActors, physics without collision (the
			// player, nearest at distance 0, always collides).
			if (it->second.system) {
				float d = 0.0f;
				int rank = 0;
				for (size_t i = 0; i < actors.size(); ++i)
					if (actors[i].second == it->first.first) { d = actors[i].first; rank = (int)i; break; }
				it->second.system->setCollisionLod(d > s.collisionDistance || rank >= std::max(1, s.maxActors));
			}
			if (std::find(roots.begin(), roots.end(), it->first.first) == roots.end())
				roots.push_back(it->first.first);
			++it;
		}
		// Impact sounds (<impact-sound>): found by the physics, not played in Oblivion yet. Taken so they do not pile up.
		const size_t impacts = hdt::SkyrimPhysicsWorld::get()->takeImpacts().size();
		if (impacts && m_impactsDropped == 0)
			logger::info("[SMP] impact sounds are not played in Oblivion yet (the item has <impact-sound> entries)");
		m_impactsDropped += (unsigned)impacts;
		stats.actors = (int)roots.size();
		stats.systems = (int)hdt::SkyrimPhysicsWorld::get()->systemCount();
		stats.failed = failed;
	}

	// A line in the log every couple of seconds saying whether the physics is doing anything visible: how many of each
	// system's bones are simulated, how far the simulated ones are from where they would hang if rigid (0 = not moving),
	// and whether the item's skin still draws with them.
	void ActorManager::report()
	{
		auto* world = hdt::SkyrimPhysicsWorld::get();
		logger::info("[stats] scan: player 3D '{}', {} physics item(s) seen ({} built); the weather's wind {:.2f}", m_playerRootName, m_items.size(),
			std::count_if(m_items.begin(), m_items.end(), [](auto& kv) { return (bool)kv.second.system; }), Game::WindSpeed());
		for (auto& [key, item] : m_items) {
			if (!item.system)
				continue;
			int simulated = 0;
			float maxOffset = 0, sumOffset = 0;
			std::vector<std::pair<float, std::string>> worst;
			for (auto& b : item.system->getBones()) {
				auto* bone = static_cast<hdt::SkyrimBone*>(b.get());
				if (bone->m_rig.isStaticOrKinematicObject() || !bone->m_anchor || !bone->m_node)
					continue;
				++simulated;
				RE::NiPoint3 rigid = (bone->m_anchor->world * bone->m_anchorToNode).translate;
				float d = (bone->m_node->world.translate - rigid).Length();
				maxOffset = std::max(maxOffset, d);
				sumOffset += d;
				worst.push_back({ d, bone->m_name.c_str() });
			}
			std::sort(worst.rbegin(), worst.rend());
			std::string most;
			for (size_t k = 0; k < worst.size() && k < 5; k++)
				most += std::format("{} {:.1f}; ", worst[k].second, worst[k].first);
			int drawn = 0;
			for (auto& slot : item.system->m_boundSlots)
				drawn += slot.holds();
			logger::info("[stats] {:p} on '{}': {} bones, {} simulated, off rigid avg {:.2f} max {:.2f} units; skin slots drawn from cloth bones {}/{}; "
			             "step {:.2f} ms ({:.2f} on the game thread)",
				key.second, item.skeleton->GetName(), item.system->getBones().size(), simulated, simulated ? sumOffset / simulated : 0.0f, maxOffset,
				drawn, item.system->m_boundSlots.size(), world->m_averageStepMs, world->m_averageMainMs);
			if (!most.empty())
				logger::info("[stats]   furthest from rest: {}", most);
			size_t joints = item.system->constraints().size();
			for (auto& group : item.system->constraintGroups()) joints += group->m_constraints.size();
			float jitter = 0, rawJitter = 0;
			for (auto& b : item.system->getBones()) {
				auto* bone = static_cast<hdt::SkyrimBone*>(b.get());
				jitter = std::max(jitter, bone->m_jitter);
				rawJitter = std::max(rawJitter, bone->m_rawJitter);
			}
			auto* sys = item.system.get();
			logger::info("[stats]   joints apart at most {:.3f} in the physics, {:.3f} as drawn ({} joints); resets: {} turn, {} jump, {} other; "
				"jitter {:.4f} drawn, {:.4f} raw", item.jointGapPhys, item.jointGapDrawn, joints,
				sys->m_resetsTurn, sys->m_resetsJump, sys->m_resetsOther, jitter, rawJitter);
			sys->m_resetsTurn = sys->m_resetsJump = sys->m_resetsOther = 0;
			item.jointGapPhys = item.jointGapDrawn = 0;
		}
	}

	// The ground is built in (see build), so a change rebuilds every system, at the next update.
	void ActorManager::applyCollision()
	{
		if ((int)gSettings.ground == m_builtGround)
			return;
		m_builtGround = gSettings.ground;
		m_rebuildPending = true;
	}

	void ActorManager::rebuildIfPending()
	{
		if (!m_rebuildPending)
			return;
		m_rebuildPending = false;
		if (m_items.empty())
			return;
		logger::info("[SMP] collision settings changed (ground {}): rebuilding", gSettings.ground ? "on" : "off");
		clear();
	}

	void ActorManager::clear()
	{
		for (auto& [key, item] : m_items)
			drop(item, false);
		m_items.clear();
		stats = {};
	}

	// --- the scene dump ---------------------------------------------------------------------------------------------------

	static void DumpObject(RE::NiAVObject* obj, RE::NiNode* actor, int depth, int& lines)
	{
		if (!obj || depth > 40 || lines > 4000)
			return;
		++lines;
		std::string indent(depth * 2, ' ');
		std::string extras;
		for (uint16_t i = 0; obj->extraData && i < obj->extraDataCount && i < 16; ++i) {
			auto* e = obj->extraData[i];
			if (!e) continue;
			const char* cls = Game::RttiName(e);
			extras += std::format(" [{} '{}'", cls, e->name ? e->name : "");
			if (!_stricmp(cls, "NiStringExtraData")) {
				const char* v = static_cast<RE::NiStringExtraData*>(e)->value;
				std::string value = v ? v : "";
				if (value.size() > 120) value = value.substr(0, 120) + "...";
				extras += " = '" + value + "'";
			}
			extras += "]";
		}
		const auto& t = obj->world.translate;
		logger::info("[dump] {}{} '{}' {:p} flags {:04x} at {:.1f} {:.1f} {:.1f}{}", indent, Game::RttiName(obj), obj->GetName(), (void*)obj, obj->flags,
			t.x, t.y, t.z, extras);
		if (auto* g = hdt::castNiGeometry(obj)) {
			auto* skin = g->skinInstance;
			logger::info("[dump] {}  data {:p} ({} vertices), skin {:p}", indent, (void*)g->data, g->data ? g->data->vertexCount : 0, (void*)skin);
			if (skin && skin->bones && skin->skinData) {
				for (uint32_t i = 0; i < skin->boneCount && i < 128; ++i) {
					RE::NiAVObject* slot = ReadSlot(skin->bones, i);
					char name[128] = "?";
					const bool named = slot && CopyName(slot, name, sizeof(name));
					const char* state = !slot ? "empty" : g_waitSet.count(slot) ? "plugin wait node" : IsLiveBone(slot, actor, g->parent) ? "skeleton" : "not in the skeleton (dropped or the plugin's)";
					logger::info("[dump] {}  slot {}: {:p} '{}' {}", indent, i, (void*)slot, named ? name : "?", state);
				}
			}
			return;
		}
		if (auto* node = obj->AsNode())
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				DumpObject(node->children[i], actor, depth + 1, lines);
	}

	static void DumpFault() { logger::error("[dump] (fault while walking this 3D)"); }

	// Under SEH, in a function of its own (no C++ objects to unwind here).
	static void DumpRoot(RE::NiNode* root)
	{
		int lines = 0;
		__try {
			DumpObject(root, root, 0, lines);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			DumpFault();
		}
	}

	void ActorManager::dumpScene(int count)
	{
		void* player = Game::Player();
		std::vector<std::pair<float, RE::NiNode*>> roots;
		RE::NiNode* playerRoot = player ? Game::Get3D(player) : nullptr;
		if (playerRoot) roots.push_back({ 0.0f, playerRoot });
		if (RE::NiNode* fp = player ? Game::PlayerFirstPerson3D(player) : nullptr; fp && fp != playerRoot) roots.push_back({ 0.0f, fp });
		const RE::NiPoint3 at = playerRoot ? playerRoot->world.translate : RE::NiPoint3{};
		std::vector<std::pair<float, RE::NiNode*>> others;
		Game::ForEachProcessActor([&](void* ref) {
			if (auto* r = Game::Get3D(ref)) others.push_back({ (r->world.translate - at).Length(), r });
		});
		std::sort(others.begin(), others.end(), [](auto& a, auto& b) { return a.first < b.first; });
		for (size_t i = 0; i < others.size() && (int)i < count; ++i) roots.push_back(others[i]);
		logger::info("[dump] ---- {} root(s): the player's third-person 3D, its first-person model, then the nearest actors ----", roots.size());
		for (auto& r : roots)
			DumpRoot(r.second);
		logger::info("[dump] ---- end ----");
	}
}
