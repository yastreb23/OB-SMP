#pragma once
// Oblivion 1.2.0.416 scene graph (Gamebryo, NIF 20.0.0.5), as FSMP's glue sees Skyrim's: the NetImmerse objects SMP
// reads and writes, laid out as the game has them in memory, and the helpers FSMP's NetImmerseUtils.h provides. The
// object types live in namespace RE, as in FSMP and in NV SMP Physics, so the ported glue reads the same.
//
// Offsets: xOBSE's NiNodes.h, NiObjects.h, NiGeometry.h and NiExtraData.h, checked against Oblivion.exe (NiNode's
// constructor 0x70B780 and CreateObject 0x70BA70, NiObjectNET::SetName 0x6FF420, the vtables found through the game's
// RTTI) and pinned here with static_assert. Where Oblivion differs from New Vegas:
//   - names are plain heap strings (no NiFixedString): NiObjectNET::SetName frees and copies;
//   - NiAVObject is 0xAC (flags 0x18, parent 0x1C, local 0x30, world 0x64), NiNode's children at 0xB0;
//   - NiGeometryData's vertices at 0x1C, NiGeometry's data and skin at 0xB4 / 0xB8;
//   - NiSkinData keeps its bone count at 0x40 and the bones at 0x44, and NiSkinInstance keeps no count of its own
//     (its bone array is as long as the skin data's), so boneCount is a property reading the skin data;
//   - NiRefObject's only virtual is the destructor (slot 0, with a "free" flag); IsNode is slot 2, IsGeometry slot 3.
#include "hdtSkinnedMesh/hdtBulletHelper.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace RE
{
	struct NiPoint3
	{
		float x = 0, y = 0, z = 0;

		NiPoint3() = default;
		NiPoint3(float a, float b, float c) : x(a), y(b), z(c) {}
		NiPoint3 operator+(const NiPoint3& o) const { return { x + o.x, y + o.y, z + o.z }; }
		NiPoint3 operator-(const NiPoint3& o) const { return { x - o.x, y - o.y, z - o.z }; }
		NiPoint3 operator*(float s) const { return { x * s, y * s, z * s }; }
		float Length() const { return std::sqrt(x * x + y * y + z * z); }
	};

	// Row-major, entry[row][column], as Gamebryo stores it.
	struct NiMatrix3
	{
		float entry[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

		NiPoint3 operator*(const NiPoint3& v) const
		{
			return { entry[0][0] * v.x + entry[0][1] * v.y + entry[0][2] * v.z,
				entry[1][0] * v.x + entry[1][1] * v.y + entry[1][2] * v.z,
				entry[2][0] * v.x + entry[2][1] * v.y + entry[2][2] * v.z };
		}
		NiMatrix3 operator*(const NiMatrix3& o) const
		{
			NiMatrix3 r;
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					r.entry[i][j] = entry[i][0] * o.entry[0][j] + entry[i][1] * o.entry[1][j] + entry[i][2] * o.entry[2][j];
			return r;
		}
		NiMatrix3 Transpose() const
		{
			NiMatrix3 r;
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					r.entry[i][j] = entry[j][i];
			return r;
		}
	};

	struct NiTransform
	{
		NiMatrix3 rotate;
		NiPoint3 translate;
		float scale = 1.0f;

		// this * rhs: rhs's frame expressed in this one's parent (world = parent.world * local).
		NiTransform operator*(const NiTransform& rhs) const
		{
			NiTransform r;
			r.rotate = rotate * rhs.rotate;
			r.translate = translate + rotate * rhs.translate * scale;
			r.scale = scale * rhs.scale;
			return r;
		}
		NiPoint3 operator*(const NiPoint3& p) const { return translate + rotate * p * scale; }
		NiTransform Invert() const
		{
			NiTransform r;
			r.rotate = rotate.Transpose();
			r.scale = scale != 0.0f ? 1.0f / scale : 1.0f;
			r.translate = r.rotate * translate * -r.scale;
			return r;
		}
	};
	static_assert(sizeof(NiTransform) == 0x34);

	struct NiBound
	{
		NiPoint3 center;
		float radius;
	};

	// NiRefObject / NiObject: vtable, reference count.
	class NiObject
	{
	public:
		void** vtbl;              // 00
		volatile long refCount;   // 04

		void IncRefCount() { InterlockedIncrement(&refCount); }
		void DecRefCount()
		{
			if (InterlockedDecrement(&refCount) == 0) {
				typedef void*(__thiscall * Destructor_t)(NiObject*, bool);
				((Destructor_t)vtbl[0])(this, true);   // NiRefObject's virtual destructor, freeing
			}
		}
		// NiObject::IsNode (vtable slot 2) and IsGeometry (slot 3): the object cast to that type, or null. (Oblivion's
		// NiNode vtable 0xA7E38C has `return this` in slot 2; NiTriShape's 0xA7ED5C and NiTriStrips' 0xA7F27C in slot 3.)
		void* IsNode() { typedef void*(__thiscall * F)(NiObject*); return ((F)vtbl[2])(this); }
		void* IsGeometry() { typedef void*(__thiscall * F)(NiObject*); return ((F)vtbl[3])(this); }
	};

	class NiExtraData : public NiObject
	{
	public:
		const char* name;   // 08
	};
	static_assert(offsetof(NiExtraData, name) == 0x08);

	class NiStringExtraData : public NiExtraData
	{
	public:
		const char* value;   // 0C
	};
	static_assert(offsetof(NiStringExtraData, value) == 0x0C);

	class NiObjectNET : public NiObject
	{
	public:
		const char* m_name;          // 08 a heap string
		void* controller;            // 0C
		NiExtraData** extraData;     // 10 sorted by name (the game binary-searches it)
		uint16_t extraDataCount;     // 14
		uint16_t extraDataCapacity;  // 16

		const char* GetName() const { return m_name ? m_name : ""; }
		const char* FindStringExtra(const char* key) const
		{
			if (!extraData)
				return nullptr;
			for (uint16_t i = 0; i < extraDataCount; ++i) {
				auto* e = extraData[i];
				if (e && e->name && _stricmp(e->name, key) == 0)
					return static_cast<NiStringExtraData*>(e)->value;
			}
			return nullptr;
		}
		// "SMP Bones" may go on in "SMP Bones 2", "SMP Bones 3"... (Outfit Studio cannot read a NIF string of 2048
		// characters or more, so long lists are split): the whole list, or empty when there is none.
		std::string SmpBonesList() const
		{
			std::string list;
			if (const char* first = FindStringExtra("SMP Bones")) list = first;
			for (int i = 2; !list.empty(); ++i) {
				const char* more = FindStringExtra(("SMP Bones " + std::to_string(i)).c_str());
				if (!more) break;
				list += more;
			}
			return list;
		}
	};
	static_assert(sizeof(NiObjectNET) == 0x18);

	class NiNode;

	class NiAVObject : public NiObjectNET
	{
	public:
		enum : uint16_t
		{
			kFlag_AppCulled = 1 << 0,
			kFlag_SelUpdate = 1 << 1,
			kFlag_SelUpdateTransforms = 1 << 2,
		};

		uint16_t flags;               // 18
		uint16_t pad1A;
		NiNode* parent;               // 1C
		NiBound worldBound;           // 20
		NiTransform local;            // 30
		NiTransform world;            // 64
		uint8_t pad98[0xAC - 0x98];   // properties (98), collision object (A8)

		NiNode* AsNode() { return static_cast<NiNode*>(IsNode()); }
	};
	static_assert(offsetof(NiAVObject, flags) == 0x18);
	static_assert(offsetof(NiAVObject, parent) == 0x1C);
	static_assert(offsetof(NiAVObject, local) == 0x30);
	static_assert(offsetof(NiAVObject, world) == 0x64);
	static_assert(sizeof(NiAVObject) == 0xAC);

	class NiNode : public NiAVObject
	{
	public:
		void* childrenVtbl;        // AC NiTArray<NiAVObjectPtr>
		NiAVObject** children;     // B0
		uint16_t childCapacity;    // B4
		uint16_t childFirstFree;   // B6 one past the last used slot
		uint16_t childCount;       // B8
		uint16_t childGrowBy;      // BA
		uint8_t padBC[0xDC - 0xBC];   // effects list (BC), combined bound (CC)

		uint16_t ChildEnd() const { return children ? childFirstFree : 0; }

		// NiNode::AttachChild (vtable slot 0x21): reparents the child (detaching it from its old parent) and holds a reference.
		void AttachChild(NiAVObject* child, bool firstAvailable = true)
		{
			typedef void(__thiscall * F)(NiNode*, NiAVObject*, bool);
			((F)vtbl[0x21])(this, child, firstAvailable);
		}
	};
	static_assert(offsetof(NiNode, children) == 0xB0);
	static_assert(offsetof(NiNode, childFirstFree) == 0xB6);
	static_assert(sizeof(NiNode) == 0xDC);

	// Skinning.
	class NiSkinData : public NiObject
	{
	public:
		struct BoneVertData
		{
			uint16_t vertex;   // 00
			float weight;      // 04
		};
		struct BoneData
		{
			NiTransform skinToBone;   // 00
			NiBound bound;            // 34
			BoneVertData* vertData;   // 44
			uint32_t vertCount;       // 48
		};
		static_assert(sizeof(BoneData) == 0x4C);

		void* skinPartition;          // 08
		NiTransform rootParentToSkin; // 0C
		uint32_t boneCount;           // 40
		BoneData* boneData;           // 44
	};
	static_assert(offsetof(NiSkinData, boneCount) == 0x40);
	static_assert(offsetof(NiSkinData, boneData) == 0x44);

	class NiSkinInstance : public NiObject
	{
	public:
		NiSkinData* skinData;      // 08
		void* skinPartition;       // 0C
		NiAVObject* rootParent;    // 10
		NiAVObject** bones;        // 14 as many as skinData's bones
		uint32_t frameID;          // 18
		uint32_t unk1C[4];         // 1C

		// New Vegas keeps a count here; Oblivion's array is as long as the skin data's.
		uint32_t GetBoneCount() const { return skinData ? skinData->boneCount : 0; }
		__declspec(property(get = GetBoneCount)) uint32_t boneCount;
	};
	static_assert(offsetof(NiSkinInstance, bones) == 0x14);
	static_assert(sizeof(NiSkinInstance) == 0x2C);

	class NiGeometryData : public NiObject
	{
	public:
		uint16_t vertexCount;   // 08
		uint16_t id;            // 0A
		NiBound bound;          // 0C
		NiPoint3* vertices;     // 1C
		NiPoint3* normals;      // 20
		void* colors;           // 24
		void* uvs;              // 28
		uint16_t format;        // 2C
		uint16_t dirtyFlags;    // 2E
		uint8_t keepFlags;      // 30
		uint8_t compressFlags;  // 31
		uint8_t pad32[2];
		void* additional;       // 34
		uint32_t unk38;         // 38
		uint8_t unk3C[4];       // 3C
	};
	static_assert(offsetof(NiGeometryData, vertices) == 0x1C);
	static_assert(sizeof(NiGeometryData) == 0x40);

	// NiTriBasedGeomData (triangle count) and NiTriShapeData (index list).
	class NiTriShapeData : public NiGeometryData
	{
	public:
		uint16_t triangleCount;        // 40
		uint16_t pad42;
		uint32_t triListLength;        // 44 index count
		uint16_t* triList;             // 48
	};
	static_assert(offsetof(NiTriShapeData, triList) == 0x48);

	class NiGeometry : public NiAVObject
	{
	public:
		void* propertyState;              // AC
		void* effectState;                // B0
		NiGeometryData* data;             // B4
		NiSkinInstance* skinInstance;     // B8
		void* shader;                     // BC
	};
	static_assert(offsetof(NiGeometry, data) == 0xB4);
	static_assert(offsetof(NiGeometry, skinInstance) == 0xB8);
	static_assert(sizeof(NiGeometry) == 0xC0);

	// Intrusive pointer on the game's reference count.
	template <class T>
	class NiPointer
	{
	public:
		NiPointer() = default;
		NiPointer(T* p) : m_ptr(p) { if (m_ptr) m_ptr->IncRefCount(); }
		NiPointer(const NiPointer& o) : NiPointer(o.m_ptr) {}
		NiPointer(NiPointer&& o) noexcept : m_ptr(std::exchange(o.m_ptr, nullptr)) {}
		~NiPointer() { if (m_ptr) m_ptr->DecRefCount(); }
		NiPointer& operator=(const NiPointer& o) { reset(o.m_ptr); return *this; }
		NiPointer& operator=(NiPointer&& o) noexcept { if (this != &o) { if (m_ptr) m_ptr->DecRefCount(); m_ptr = std::exchange(o.m_ptr, nullptr); } return *this; }
		NiPointer& operator=(T* p) { reset(p); return *this; }

		void reset(T* p = nullptr)
		{
			if (p) p->IncRefCount();
			if (m_ptr) m_ptr->DecRefCount();
			m_ptr = p;
		}
		T* get() const { return m_ptr; }
		T* operator->() const { return m_ptr; }
		T& operator*() const { return *m_ptr; }
		explicit operator bool() const { return m_ptr != nullptr; }
		bool operator==(const T* p) const { return m_ptr == p; }
		bool operator!=(const T* p) const { return m_ptr != p; }
		bool operator==(const void* p) const { return m_ptr == p; }
		bool operator!=(const void* p) const { return m_ptr != p; }
		bool operator==(const NiPointer& o) const { return m_ptr == o.m_ptr; }
		bool operator!=(const NiPointer& o) const { return m_ptr != o.m_ptr; }

	private:
		T* m_ptr = nullptr;
	};
}

namespace hdt
{
	template <class T>
	inline RE::NiPointer<T> make_nismart(T* p) { return RE::NiPointer<T>(p); }

	inline RE::NiNode* castNiNode(RE::NiAVObject* obj) { return obj ? obj->AsNode() : nullptr; }
	inline RE::NiGeometry* castNiGeometry(RE::NiAVObject* obj) { return obj ? static_cast<RE::NiGeometry*>(obj->IsGeometry()) : nullptr; }

	// The first object named `name` under `obj`, obj included (case-insensitive, as the game's own lookup).
	inline RE::NiAVObject* findObject(RE::NiAVObject* obj, const char* name, int depth = 0)
	{
		if (!obj || depth > 64)
			return nullptr;
		if (obj->m_name && _stricmp(obj->m_name, name) == 0)
			return obj;
		if (auto* node = castNiNode(obj)) {
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				if (auto* found = findObject(node->children[i], name, depth + 1))
					return found;
		}
		return nullptr;
	}

	inline RE::NiNode* findNode(RE::NiNode* obj, const char* name)
	{
		return castNiNode(findObject(obj, name));
	}

	// World transforms of obj and everything under it from their local ones, as NiAVObject::UpdateWorldData.
	inline void updateTransformUpDown(RE::NiAVObject* obj, bool = false, int depth = 0)
	{
		if (!obj || depth > 64)
			return;
		if (obj->parent)
			obj->world = obj->parent->world * obj->local;
		if (auto* node = castNiNode(obj)) {
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				updateTransformUpDown(node->children[i], false, depth + 1);
		}
	}

	// A new, unparented NiNode named `name`, with one reference held by the caller (NiPointer).
	RE::NiNode* createNode(const char* name);
	// Renames an object (NiObjectNET::SetName: the old name freed, the new one copied).
	void setName(RE::NiAVObject* obj, const char* name);

	// A file from the game's Data folder: loose (through Mod Organizer's virtual file system) or in a BSA.
	std::string readAllFile(const char* path);
}
