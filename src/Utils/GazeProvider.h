#pragma once

#include "GazeMath.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace Util::Gaze
{
	/** @brief UDP port the OSC listener binds by default. */
	inline constexpr int kDefaultOscPort = 9015;

	/** @brief Which source supplied the last valid gaze sample. */
	enum class Source : std::uint8_t
	{
		kNone = 0,
		kSteamVr,
		kOsc,
	};

	/** @brief One frame's gaze result, in normalised eye UV (y down). */
	struct Sample
	{
		bool valid = false;
		std::array<Vec2, 2> centre{};  // [0] left eye, [1] right eye.
		Source source = Source::kNone;
	};

	/** @brief Diagnostic counters for the settings panel and devbench. */
	struct Status
	{
		Source source = Source::kNone;
		bool valid = false;
		std::array<Vec2, 2> centre{};
		std::uint64_t validFrames = 0;
		std::uint64_t oscPackets = 0;
		bool oscListening = false;
		bool oscBindFailed = false;
		int oscPort = 0;
		std::string lastOscAddress;
	};

	/**
	 * @brief Per-frame gaze source: SteamVR's eye-tracked foveation centre, then OSC over UDP.
	 *
	 * Poll() runs on the render thread; only the OSC listener thread writes the shared
	 * gaze state, under a lock. The destructor closes the socket and joins that thread.
	 */
	class Provider
	{
	public:
		Provider() = default;
		~Provider();
		Provider(const Provider&) = delete;
		Provider& operator=(const Provider&) = delete;

		/**
		 * @brief Applies settings each frame; restarts the OSC listener when its port or enable state changes.
		 * @param a_allowSteamVr False to ignore SteamVR gaze (OSC only).
		 * @param a_allowOsc False to ignore OSC gaze and stop the listener (SteamVR only).
		 * @param a_port UDP port the OSC listener binds.
		 * @param a_offsetX Horizontal gaze offset applied to OSC samples only.
		 * @param a_offsetY Vertical gaze offset applied to OSC samples only.
		 */
		void Configure(bool a_allowSteamVr, bool a_allowOsc, int a_port, float a_offsetX, float a_offsetY);

		/** @brief Samples gaze once per frame; SteamVR first, OSC second. */
		Sample Poll();

		/** @brief Copies the diagnostic counters; safe to call from another thread. */
		Status GetStatus() const;

		/** @brief Closes the OSC socket and joins its thread; safe to call repeatedly. */
		void Shutdown();

	private:
		static constexpr int kSteamVrRetryFrames = 300;
		static constexpr std::int64_t kOscStaleMilliseconds = 250;

		void EnsureOpenVr();
		bool TryAcquireOpenVr();
		void LoadProjection();
		Sample PollSteamVr();
		Sample PollOsc();
		void StartOsc();
		void StopOsc();
		void OscThread(std::uintptr_t a_socket);

		void* steamVrSystem = nullptr;     // newest IVRSystem the loaded openvr_api.dll serves
		bool steamVrHasFoveation = false;  // true when steamVrSystem is IVRSystem_026
		int steamVrRetryFrames = 0;
		std::array<ProjectionRaw, 2> projection = kFallbackProjection;

		bool allowSteamVr = true;
		bool allowOsc = true;
		float offsetX = 0.0f;
		float offsetY = 0.0f;

		std::mutex lifecycleMutex;
		std::thread oscThread;
		std::atomic_bool oscRunning{ false };
		std::atomic_bool oscListening{ false };
		std::atomic_bool oscBindFailed{ false };
		std::atomic<std::uint64_t> oscPackets{ 0 };
		std::uintptr_t oscSocket = ~std::uintptr_t(0);
		int oscPort = kDefaultOscPort;

		std::mutex oscMutex;
		std::array<Direction, 2> oscDirections{};
		bool oscHasGaze = false;
		std::int64_t oscTimestamp = 0;
		std::string lastOscAddress;

		mutable std::mutex statusMutex;
		Status status;
		std::atomic<std::uint64_t> validFrames{ 0 };
	};
}
