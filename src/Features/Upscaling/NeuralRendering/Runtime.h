#pragma once

#include "Guide.h"
#include "Lifecycle.h"
#include "Tuning.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace NR
{
	class D3D12Interop;

	/** @brief True while the process is exiting: NGX teardown is skipped and nothing may log. */
	inline std::atomic<bool> processTerminating{ false };

	/**
	 * @brief Marks process teardown. Declare it as the LAST member of the object that owns the
	 *        runtime: members are destroyed in reverse order, so it flips before any NGX teardown.
	 */
	struct TerminationSentinel
	{
		~TerminationSentinel() { processTerminating.store(true, std::memory_order_relaxed); }
	};

	/** @brief Per-call state the caller supplies and receives for one eye's evaluate. */
	struct FrameParameters
	{
		bool reset = true;     ///< Send DLSSNR.Reset=1: the temporal history must restart.
		bool created = false;  ///< The Feature 18 handle was created by this call.
		uint32_t result = 0;   ///< Last NGX result code, for diagnostics.
	};

	/** @brief Result of an attempt; the caller latches once instead of catching a throw. */
	struct AttemptResult
	{
		bool ok = false;
		FailureKind kind = FailureKind::kNone;
		std::string reason;
	};

	/** @brief Owns the cached NGX Feature 18 ABI and one handle and parameter block per eye. */
	class Runtime
	{
	public:
		Runtime();
		~Runtime();
		Runtime(const Runtime&) = delete;
		Runtime& operator=(const Runtime&) = delete;

		/**
		 * @brief Loads the supported runtime DLL, spoofs the caller identity and initializes NGX.
		 * @param a_interop Interop supplying the D3D12 device; drained before any teardown.
		 * @param a_directory Streamline plugin directory that must hold nvngx_dlssnr.dll.
		 * @return Failure detail instead of an exception, so a bad DLL or driver can only latch.
		 */
		AttemptResult Initialize(D3D12Interop& a_interop, const std::filesystem::path& a_directory);

		/** @brief Releases both Feature 18 handles; the caller must have retired GPU work first. */
		void ResetFeatures();

		/**
		 * @brief Drains the interop, then releases handles, blocks, NGX and both modules.
		 * @return A failed result when a drain of a live device throws; the runtime and its
		 *         resources are then kept alive, because the GPU may still reference them. A
		 *         removed device reports ok carrying kDeviceRemoved, with everything released.
		 */
		AttemptResult Shutdown();

		/**
		 * @brief Creates (first call) or evaluates (later calls) one eye's Feature 18 instance.
		 * @return false on an NGX create or evaluate failure; a caller error (missing or empty
		 *         guide) throws, because it can only be a pass-integration bug.
		 */
		bool Evaluate(ID3D12GraphicsCommandList* a_commands, uint32_t a_eyeIndex,
			ID3D12Resource* a_color, ID3D12Resource* a_depth, ID3D12Resource* a_motion, ID3D12Resource* a_output,
			uint32_t a_width, uint32_t a_height, const GuideParameters& a_guides,
			FrameParameters& a_frame, const Tuning& a_tuning);

		/** @brief True once NGX accepted the device and both parameter blocks exist. */
		[[nodiscard]] bool Initialized() const;

		/** @brief True after a structured exception escaped an NGX call: NGX is never re-entered. */
		[[nodiscard]] bool Poisoned() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
