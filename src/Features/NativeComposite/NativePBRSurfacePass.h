#pragma once

#include "Features/NativePBR/GpuMaterialResources.h"
#include "Features/NativeLighting/NativeLighting.h"
#include "Features/NativePBR/SurfaceFrameContract.h"

#include <array>
#include <memory>

/** @brief DX12-only consumer of a producer-owned opaque surface frame. */
class NativePBRSurfacePass
{
public:
	static constexpr uint32_t kSurfaceInputs = NativePBR::kSurfaceTransportAttachmentCount;
	static constexpr uint32_t kSurfaceOutputs = 5;
	static constexpr uint32_t kMaterialPages = NativePBR::GpuMaterialResources::kTextureDescriptorCount;
	static constexpr uint32_t kProvisionalSurfaceAbiVersion = NativePBR::kSurfaceFrameAbiVersion;

	using InputFrame = NativePBR::SurfaceFrame;

	struct Slot
	{
		std::array<winrt::com_ptr<ID3D12Resource>, kSurfaceOutputs> outputs;
		winrt::com_ptr<ID3D12DescriptorHeap> descriptors, finalDescriptors;
		winrt::com_ptr<ID3D12RootSignature> finalRoot;
		winrt::com_ptr<ID3D12PipelineState> finalPipeline;
		winrt::com_ptr<ID3D12Resource> drawBindings;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		std::shared_ptr<const InputFrame> input;
		std::shared_ptr<const NativePBR::GpuMaterialResources> materials;
		uint64_t bytes = 0;
		uint32_t width = 0, height = 0, drawCapacity = 0;
	};

	/** @brief Initializes the SM6.5 pipeline without requiring a surface producer. */
	bool Initialize(ID3D12Device* a_device);
	/** @brief Records and binds a fully fenced material evaluation, without changing final scene color. */
	bool Record(ID3D12Device* a_device, ID3D12GraphicsCommandList* a_commands, Slot& a_slot,
		std::shared_ptr<const InputFrame> a_input, std::shared_ptr<const NativePBR::GpuMaterialResources> a_materials,
		uint32_t a_frame, uint32_t a_width, uint32_t a_height, uint64_t a_additionalBudget);
	/** @brief Records optional BRDF-based opaque replacement after surface evaluation. */
	bool RecordFinalColor(ID3D12Device* a_device, ID3D12GraphicsCommandList* a_commands,
		Slot& a_slot, ID3D12Resource* a_sceneColor, DXGI_FORMAT a_sceneFormat,
		const NativeLightingFeature::Contribution& a_lighting, uint32_t a_frame,
		uint32_t a_width, uint32_t a_height, float a_compositeStrength, bool a_legacyTransfer);
	/** @brief Count final-color submissions after the owning queue is signaled. */
	void MarkFinalSubmitted() { ++finalSubmissions; finalStatus = "NativePBR opaque final-color pass submitted"; }
	const std::string& FinalStatus() const { return finalStatus; }
	uint64_t FinalSubmissions() const { return finalSubmissions; }
	void DeferFinal(const char* a_reason) { finalStatus = a_reason; }
	/** @brief Reports why an attempted material dispatch was deferred. */
	const std::string& Status() const { return status; }
	uint64_t Submissions() const { return submissions; }
	uint64_t MissingMaterials() const { return missingMaterials; }
	uint64_t MissingRevisions() const { return missingRevisions; }
	uint64_t MissingTextures() const { return missingTextures; }
	uint64_t UnsupportedRecipes() const { return unsupportedRecipes; }
	uint64_t UnsupportedInputs() const { return unsupportedInputs; }
	uint64_t OrdinalMismatches() const { return ordinalMismatches; }
	uint64_t NoEligibleFrames() const { return noEligibleFrames; }
	/** @brief Count a recorded dispatch only after the owning command queue signals successfully. */
	void MarkSubmitted();
	/** Diagnostic output slots are VRAM-limited; another frame may still be composited normally. */
	void DeferForSlotBudget() { status = "NativePBR diagnostic output limited to one fenced composite slot"; }

private:
	winrt::com_ptr<ID3D12RootSignature> root;
	winrt::com_ptr<ID3D12PipelineState> pipeline;
	ID3D12Device* device = nullptr, *finalDevice = nullptr;
	winrt::com_ptr<ID3D12RootSignature> finalRoot;
	winrt::com_ptr<ID3D12PipelineState> finalPipeline;
	bool InitializeFinal(ID3D12Device* a_device);
	std::string finalStatus = "NativePBR final-color composition disabled";
	uint64_t finalSubmissions = 0;
	std::string status = "Waiting for surface input producer";
	uint64_t submissions = 0, missingMaterials = 0, missingRevisions = 0, missingTextures = 0, unsupportedRecipes = 0, unsupportedInputs = 0;
	uint64_t ordinalMismatches = 0, noEligibleFrames = 0;
};
