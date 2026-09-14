#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>

/** Owns projectile hooks and fans immutable observations out to wind routers. */
class ProjectileHookDispatcher
{
public:
	/** Receives an exact projectile collision without taking ownership of runtime objects. */
	using ImpactObserver = void (*)(void*, RE::Projectile&, const RE::NiPoint3&, const RE::NiPoint3&);

	/** Receives one in-flight projectile update without taking ownership of the runtime object. */
	using MotionObserver = void (*)(void*, RE::Projectile&, float);

	/**
	 * @brief Static ImpactObserver trampoline for routers whose ObserveImpact takes the full
	 *  (projectile, position, velocity) triple -- covers callers where the callback body would
	 *  otherwise be a byte-identical static_cast-and-forward one-liner.
	 */
	template <class T, void (T::*Method)(RE::Projectile&, const RE::NiPoint3&, const RE::NiPoint3&)>
	static void ImpactObserverTrampoline(void* a_owner, RE::Projectile& a_projectile,
		const RE::NiPoint3& a_position, const RE::NiPoint3& a_velocity)
	{
		(static_cast<T*>(a_owner)->*Method)(a_projectile, a_position, a_velocity);
	}

	/** Selects which projectile virtual functions a router needs observed. */
	enum class Event : std::uint8_t
	{
		Impact = 1 << 0,
		Motion = 1 << 1
	};

	/** Returns the process-lifetime projectile hook dispatcher. */
	[[nodiscard]] static ProjectileHookDispatcher& GetSingleton();

	/** Registers one impact callback if the owner is not already registered. */
	void AddImpactObserver(void* a_owner, ImpactObserver a_observer);

	/** Registers one motion callback if the owner is not already registered. */
	void AddMotionObserver(void* a_owner, MotionObserver a_observer);

	/** Removes every impact and motion callback registered by an owner. */
	void RemoveObservers(void* a_owner);

	/** Installs the shared hooks required for a projectile record's runtime class. */
	void ObserveProjectileType(const RE::BGSProjectile& a_projectile, Event a_events);

private:
	template <class T>
	struct ObserverRegistration
	{
		void* owner{};
		T observer{};
	};

	template <class T>
	struct ProjectileAddImpactHook;
	template <class T>
	struct ProjectileUpdateHook;

	template <class T>
	void InstallProjectileHooks(Event a_events, RE::BGSProjectileData::Type a_type);
	void DispatchImpact(RE::Projectile& a_projectile, const RE::NiPoint3& a_position,
		const RE::NiPoint3& a_velocity);
	void DispatchMotion(RE::Projectile& a_projectile, float a_deltaTime);

	static constexpr std::size_t kMaximumObservers = 8;
	std::array<ObserverRegistration<ImpactObserver>, kMaximumObservers> impactObservers{};
	std::array<ObserverRegistration<MotionObserver>, kMaximumObservers> motionObservers{};
	std::size_t impactObserverCount{};
	std::size_t motionObserverCount{};
	std::uint16_t installedImpactTypes{};
	std::uint16_t installedMotionTypes{};
	mutable std::shared_mutex observersMutex;
};
