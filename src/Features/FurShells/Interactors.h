#pragma once

#include <array>
#include <unordered_map>
#include <vector>

namespace FurInteractors
{
	/** @brief Live capsules a pass can hand to the vertex shader. */
	inline constexpr size_t MaxLiveCapsulesPerPass = 8;
	/** @brief Remembered dents a mesh keeps, handed to the vertex shader as fading capsules. */
	inline constexpr size_t MaxDentsPerMesh = 4;
	/** @brief Capsule slots a pass can hand to the vertex shader. */
	inline constexpr size_t MaxCapsulesPerPass = MaxLiveCapsulesPerPass + MaxDentsPerMesh;

	/** @brief Finger and palm capsule radii used until a fit to the player's hand meshes succeeds. */
	inline constexpr float DefaultFingerRadius = 1.2f;
	inline constexpr float DefaultPalmRadius = 1.6f;

	/** @brief A body part that can touch fur, in absolute world space. */
	struct Capsule
	{
		float3 pointA;
		float3 pointB;
		float radius = 0.0f;
		float3 move;             ///< How far the capsule centre moved since the last frame.
		RE::FormID owner = 0;    ///< The actor whose body the capsule belongs to.
		bool appendage = false;  ///< A hand or finger, which also touches the owner's own gear.
		int side = -1;           ///< For a hand or finger capsule, 0 for the left hand and 1 for the right.
		float strength = 1.0f;   ///< How strongly the capsule moves fur; below 1 for a remembered dent that is recovering.
	};

	/**
	 * @brief Collects the collision capsules of nearby actors and the player's VR hands and fingers each frame.
	 * @details Actor capsules come from the same Havok collision objects Grass Collision reads. Finger capsules come from
	 *          the player skeleton, so they work without HIGGS.
	 */
	class Tracker
	{
	public:
		/** @brief Rebuilds the capsule list for the actors within a_maxDistance of the camera and the player's VR hands. */
		void Update(float a_maxDistance);
		/** @brief Drops every capsule and the movement history. */
		void Clear();
		/** @brief Drops the capsules for a frame with no fur in view, keeping the dents and letting them recover. */
		void Rest();

		/**
		 * @brief Picks the capsules nearest a mesh, skipping its wearer's own body capsules but keeping the wearer's hands and fingers.
		 * @param a_center World bound centre of the mesh.
		 * @param a_radius World bound radius of the mesh.
		 * @param a_reach How far past the mesh a capsule still matters.
		 * @param a_wearer Form ID of the actor wearing the mesh, or 0 when unknown.
		 * @param a_out Receives the chosen capsules.
		 * @param a_nearestGap Receives the smallest distance from the mesh bound to any eligible capsule, which is negative when one touches.
		 * @return How many entries of a_out were filled.
		 */
		[[nodiscard]] size_t Select(const RE::NiPoint3& a_center, float a_radius, float a_reach, RE::FormID a_wearer,
			std::array<Capsule, MaxCapsulesPerPass>& a_out, float& a_nearestGap) const;

		[[nodiscard]] size_t GetCount() const { return capsules.size(); }

		/** @brief How many dents all meshes are remembering. */
		[[nodiscard]] size_t GetDentCount() const;

		/**
		 * @brief Remembers how deeply the live capsules press into a mesh's fur, and appends its recovering dents as fading capsules.
		 * @details A press is how far a capsule reaches into the fur, from the skinned mesh vertices. A dent stays where it was pressed, on the bone
		 *          it was pressed against, and stands back up over a time that grows with how deep the press was.
		 * @param a_geometry The fur mesh.
		 * @param a_furLength The fur's current length.
		 * @param a_count How many live capsules a_capsules holds.
		 * @param a_capsules The live capsules, which receive the dents after them.
		 * @return The new total, live capsules plus dents.
		 */
		size_t Remember(const RE::BSGeometry* a_geometry, float a_furLength, size_t a_count, std::array<Capsule, MaxCapsulesPerPass>& a_capsules);

		/** @brief How many of the player's hand and finger skeleton nodes were found, out of 2 * HandNodeCount. */
		[[nodiscard]] size_t GetHandNodeCount() const { return handNodesFound; }

		/** @brief World position of a hand node and of its middle fingertip joint, for checking that injected controller poses move the hands; false while the nodes are not found. */
		[[nodiscard]] bool GetHandPositions(bool a_left, float3& a_hand, float3& a_fingerJoint) const;

		/** @brief How many skinned meshes the capsule radii were fitted from, 0 while the defaults are in use. */
		[[nodiscard]] size_t GetFitMeshCount() const { return fitMeshes; }

		/** @brief Mean fitted finger radius over both hands. */
		[[nodiscard]] float GetAverageFingerRadius() const;

		/** @brief Mean fitted palm radius over both hands. */
		[[nodiscard]] float GetAveragePalmRadius() const;

	private:
		static constexpr size_t FingerCount = 5;
		static constexpr size_t JointsPerFinger = 3;
		static constexpr size_t HandNodeCount = 1 + FingerCount * JointsPerFinger;

		struct HandNodes
		{
			std::array<RE::NiPointer<RE::NiAVObject>, HandNodeCount> nodes;
		};

		void AddActorCapsules(RE::Actor* a_actor, const RE::NiPoint3& a_camera);
		void AddHandCapsules(RE::Actor* a_player, bool a_left);
		void Push(Capsule a_capsule, size_t a_index);
		void RefreshHandNodes(RE::Actor* a_player);
		void DecayDents();

		/**
		 * @brief Fits each finger and palm capsule radius to the skinned vertices of the player's hand and armor meshes.
		 * @details Reads the CPU copies of the vertices and skin weights. Each vertex goes to its dominant bone, and a bone's radius
		 *          is a high percentile of its vertices' distances to the bone segment, taking the outermost mesh over the hand.
		 */
		void FitRadii(RE::Actor* a_player);

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
			RE::NiPoint3 local;  ///< Position in that bone's space.
		};

		struct MeshMemory
		{
			const void* skin = nullptr;
			std::vector<RE::NiPointer<RE::NiAVObject>> bones;
			std::vector<PressSample> samples;
			std::vector<Dent> dents;
			uint32_t handSides = 0;  ///< Bit per hand whose skeleton nodes the mesh is skinned to, such as a gauntlet.
			uint32_t measuredFrame = UINT32_MAX;
			uint32_t usedFrame = 0;
		};

		void BuildPressSamples(const RE::BSGeometry* a_geometry, MeshMemory& a_memory) const;

		std::vector<Capsule> capsules;
		std::unordered_map<const RE::BSGeometry*, MeshMemory> memory;
		float frameSeconds = 0.0f;
		uint32_t frame = 0;
		std::unordered_map<uint64_t, float3> previousCenters;
		std::unordered_map<uint64_t, float3> currentCenters;
		std::array<HandNodes, 2> handNodes;
		const RE::NiAVObject* handNodeRoot = nullptr;
		size_t handNodesFound = 0;

		struct HandRadii
		{
			std::array<float, FingerCount * JointsPerFinger> finger;
			std::array<float, FingerCount> tipExtent{};  ///< Mesh distance from each fingertip joint to the finger's end, 0 until fitted.
			float palm = DefaultPalmRadius;

			HandRadii() { finger.fill(DefaultFingerRadius); }
		};
		std::array<HandRadii, 2> handRadii;
		size_t fitMeshes = 0;
		uint64_t fitSignature = 0;
		uint32_t updatesSinceFitCheck = 0;
	};
}
