#pragma once

#include "SurfaceFrameContract.h"

#include <d3d11_4.h>
#include <memory>
#include <string>

namespace NativePBR
{
	/** @brief Independent NativePBR DX11-to-DX12 attachment transfer, without scene or TLAS inputs. */
	class SurfaceFramePublisher
	{
	public:
		struct Sources
		{
			std::array<ID3D11Texture2D*, kSurfaceFrameAttachmentCount> textures{};
			std::shared_ptr<const std::vector<SidecarDraw>> draws;
			uint32_t frame = 0, abiVersion = kSurfaceSourceAbiVersion;
			bool nativeOwnedDraws = false;
		};
		struct Diagnostics
		{
			uint64_t published = 0, projectedPublished = 0, projectedDeferred = 0;
			uint64_t busySlots = 0, budgetDeferrals = 0, unavailableSources = 0, invalidDrawTables = 0;
			uint64_t allocationFailures = 0;
			uint64_t allocatedBytes = 0, effectiveBudgetBytes = 0, localBudgetBytes = 0, localUsageBytes = 0;
			bool dxgiAvailable = false;
			std::string status = "NativePBR surface producer has not published";
		};

		SurfaceFramePublisher();
		~SurfaceFramePublisher();
		SurfaceFramePublisher(const SurfaceFramePublisher&) = delete;
		SurfaceFramePublisher& operator=(const SurfaceFramePublisher&) = delete;
		/** Release completed diagnostic snapshots when the diagnostic consumer is disabled. */
		void ReleaseIdle();

		/** @brief Copies ABI-v2 DX11 attachments and signals a shared producer fence without CPU waiting. */
		bool Publish(ID3D11Device5* a_device11, ID3D11DeviceContext4* a_context11,
			ID3D12Device* a_device12, ID3D12CommandQueue* a_queue12, const Sources& a_sources,
			uint64_t a_optionalBudgetBytes = 0);
		/** @brief Returns only the current frame's immutable surface and draw-table publication. */
		std::shared_ptr<const SurfaceFrame> Get(uint32_t a_frame) const;
		/** @brief Pins a published slot through the GPU consumer's signaled fence value. */
		bool RetainThrough(const std::shared_ptr<const SurfaceFrame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value);
		/** @brief Returns resource, budget, and publication failure diagnostics. */
		const Diagnostics& GetDiagnostics() const;

	private:
		struct State;
		std::unique_ptr<State> state;
	};
}
