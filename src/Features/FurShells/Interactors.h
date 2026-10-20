#pragma once

#include <array>
#include <unordered_map>
#include <vector>

namespace FurInteractors
{
	/** @brief Capsules a pass can hand to the vertex shader. */
	inline constexpr size_t MaxCapsulesPerPass = 8;

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

		/**
		 * @brief Fits each finger and palm capsule radius to the skinned vertices of the player's hand and armor meshes.
		 * @details Reads the CPU copies of the vertices and skin weights. Each vertex goes to its dominant bone, and a bone's radius
		 *          is a high percentile of its vertices' distances to the bone segment, taking the outermost mesh over the hand. A fingertip is
		 *          measured to the open ray past its last joint, so vertices beyond the joint set its length instead of widening its radius.
		 */
		void FitRadii(RE::Actor* a_player);

		std::vector<Capsule> capsules;
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
