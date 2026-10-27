#pragma once

namespace Util::Higgs
{
	/**
	 * @brief The part of HIGGS's IHiggsInterface001 this plugin reads, laid out as the HIGGS header declares it.
	 * @details Entries this plugin never calls are placeholders that keep the virtual table in order, so reorder or remove none of them.
	 *          Source: adamhynek/higgs include/higgsinterface001.h.
	 */
	struct Interface001
	{
		virtual unsigned int GetBuildNumber() = 0;
		virtual void AddPulledCallback(void*) = 0;
		virtual void AddGrabbedCallback(void*) = 0;
		virtual void AddDroppedCallback(void*) = 0;
		virtual void AddStashedCallback(void*) = 0;
		virtual void AddConsumedCallback(void*) = 0;
		virtual void AddCollisionCallback(void*) = 0;
		virtual void GrabObject(RE::TESObjectREFR*, bool) = 0;
		virtual RE::TESObjectREFR* GetGrabbedObject(bool) = 0;
		virtual bool IsHandInGrabbableState(bool) = 0;
		virtual void DisableHand(bool) = 0;
		virtual void EnableHand(bool) = 0;
		virtual bool IsDisabled(bool) = 0;
		virtual void DisableWeaponCollision(bool) = 0;
		virtual void EnableWeaponCollision(bool) = 0;
		virtual bool IsWeaponCollisionDisabled(bool) = 0;
		virtual bool IsTwoHanding() = 0;
		virtual void AddStartTwoHandingCallback(void*) = 0;
		virtual void AddStopTwoHandingCallback(void*) = 0;
		virtual bool CanGrabObject(bool) = 0;
		virtual void AddCollisionFilterComparisonCallback(void*) = 0;
		virtual void AddPrePhysicsStepCallback(void*) = 0;
		virtual std::uint64_t GetHiggsLayerBitfield() = 0;
		virtual void SetHiggsLayerBitfield(std::uint64_t) = 0;
		/** @brief HIGGS's hand rigid body; a bhkRigidBody. */
		virtual RE::NiObject* GetHandRigidBody(bool a_isLeft) = 0;
		/** @brief HIGGS's rigid body for the weapon held in a hand; a bhkRigidBody. */
		virtual RE::NiObject* GetWeaponRigidBody(bool a_isLeft) = 0;
		/** @brief The rigid body a hand has grabbed; a bhkRigidBody. */
		virtual RE::NiObject* GetGrabbedRigidBody(bool a_isLeft) = 0;
		virtual void ForceWeaponCollisionEnabled(bool) = 0;
		virtual bool IsHoldingObject(bool a_isLeft) = 0;
	};

	/**
	 * @brief The HIGGS interface, or null when HIGGS is not installed or does not answer.
	 * @details Fetched once, on the first call, through the SKSE message HIGGS answers, so call it only after plugins have loaded
	 *          and only in VR. A failed fetch is not retried.
	 *          The virtual table here goes through IsHoldingObject, which older HIGGS builds lack, so a build below
	 *          1.6.0 (1060000) is treated as unavailable.
	 */
	[[nodiscard]] Interface001* GetInterface();
}
