#pragma once

#include "Utils/BodyCapsules.h"

#include <array>
#include <span>
#include <unordered_map>
#include <vector>

namespace FurInteractors
{
	using Capsule = Util::BodyCapsules::Capsule;

	/** @brief Live capsules a pass can hand to the vertex shader. */
	inline constexpr size_t MaxLiveCapsulesPerPass = 8;
	/** @brief Remembered dents a mesh keeps, handed to the vertex shader as fading capsules. */
	inline constexpr size_t MaxDentsPerMesh = 4;
	/** @brief Capsule slots a pass can hand to the vertex shader. */
	inline constexpr size_t MaxCapsulesPerPass = MaxLiveCapsulesPerPass + MaxDentsPerMesh;

	/**
	 * @brief Remembers how deeply the player's hands and fingers pressed into each fur mesh, so the fur stands back up slowly.
	 * @details A press is how far a capsule reaches into the fur, from the skinned mesh vertices. A dent stays where it was pressed, on the bone
	 *          it was pressed against, and recovers exponentially over a time that grows with how deep the press was.
	 */
	class Memory
	{
	public:
		/** @brief Advances the recovery by one frame; call it once per frame whether or not fur is drawn. */
		void Advance();
		/** @brief Forgets every dent. */
		void Clear();

		/**
		 * @brief Records how deeply the live capsules press into a mesh's fur, and appends its recovering dents as fading capsules.
		 * @param a_geometry The fur mesh.
		 * @param a_furLength The fur's current length.
		 * @param a_count How many live capsules a_capsules holds.
		 * @param a_capsules The live capsules, which receive the dents after them.
		 * @return The new total, live capsules plus dents.
		 */
		size_t Remember(const RE::BSGeometry* a_geometry, float a_furLength, size_t a_count, std::array<Capsule, MaxCapsulesPerPass>& a_capsules);

		/**
		 * @brief Keeps the capsules nearest a fur mesh's surface, so a fingertip is not dropped for a palm capsule that is nearer the mesh centre.
		 * @details Distance is measured to the skinned mesh vertices. A hand and finger capsule ignores the vertices driven by that same hand's bones, such as its own
		 *          gauntlet, which the hand fills and would otherwise take every slot. A mesh without skin data keeps the first capsules in the order given.
		 * @param a_geometry The fur mesh.
		 * @param a_candidates Capsules within reach of the mesh.
		 * @param a_out Receives up to its size of the nearest candidates.
		 * @return How many entries of a_out were filled.
		 */
		size_t Nearest(const RE::BSGeometry* a_geometry, std::span<const Capsule> a_candidates, std::span<Capsule> a_out);

		/** @brief How many dents all meshes are remembering. */
		[[nodiscard]] size_t GetDentCount() const;

	private:
		/** @brief A press that stays where it happened until the fur stands back up. */
		struct Dent
		{
			RE::NiPointer<RE::NiAVObject> bone;
			RE::NiPoint3 local;  ///< Press point in the bone's space.
			float radius = 0.0f;
			float depth = 0.0f;           ///< Press depth, 0 to 1, falling as the fur recovers.
			float recoverSeconds = 0.0f;  ///< Time constant of the exponential recovery, longer after a deeper press.
			uint32_t refreshedFrame = 0;
		};

		/** @brief A mesh vertex reduced to its dominant bone, for measuring how far a capsule reaches into the fur. */
		struct PressSample
		{
			uint16_t bone = 0;
			int side = -1;       ///< 0 or 1 when that bone is a left or right hand or finger bone, which a hand fills, otherwise -1.
			RE::NiPoint3 local;  ///< Position in that bone's space.
		};

		struct MeshMemory
		{
			const void* skin = nullptr;
			std::vector<RE::NiPointer<RE::NiAVObject>> bones;
			std::vector<PressSample> samples;
			std::vector<Dent> dents;
			uint32_t measuredFrame = UINT32_MAX;
			uint32_t usedFrame = 0;
		};

		MeshMemory& Prepare(const RE::BSGeometry* a_geometry, const void* a_skin);
		void DecayDents();
		void BuildPressSamples(const RE::BSGeometry* a_geometry, MeshMemory& a_memory) const;

		std::unordered_map<const RE::BSGeometry*, MeshMemory> memory;
		float frameSeconds = 0.0f;
		uint32_t frame = 0;
	};
}
