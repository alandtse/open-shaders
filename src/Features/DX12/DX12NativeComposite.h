#pragma once

#include "DX12NativeScene.h"

#include <d3d12.h>
#include <deque>
#include <memory>
#include <optional>
#include <winrt/base.h>

/** @brief RayQuery shadow mask and raster-color composite feeding the existing runtime upscaler. */
struct DX12NativeComposite final : Feature
{
	~DX12NativeComposite();

	struct Settings
	{
		bool Enabled = true;
		float ShadowStrength = 1.0f;
	};

	struct Surfaces
	{
		winrt::com_ptr<ID3D12Resource> shadowMask;
		winrt::com_ptr<ID3D12Resource> color;
		winrt::com_ptr<ID3D12Resource> rasterSource, depthSource;
		winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
		uint32_t width = 0, height = 0;
		DXGI_FORMAT rasterFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
	};

	struct Submission
	{
		std::shared_ptr<Surfaces> surfaces;
		std::shared_ptr<const DX12::AccelerationSceneReady> scene;
		winrt::com_ptr<ID3D12Resource> camera;
		winrt::com_ptr<ID3D12Resource> lights;
		uint64_t geometryRevision = 0, lightRevision = 0;
		ID3D12Resource* Color() const { return surfaces->color.get(); }
	};

	std::string GetName() override { return "DX12 Native Composite"; }
	std::string GetShortName() override { return "DX12NativeComposite"; }
	std::string_view GetCategory() const override { return FeatureCategories::kDX12; }
	bool IsCore() const override { return true; }
	bool SupportsVR() override { return false; }
	void DrawSettings() override;
	void LoadSettings(json& input) override;
	void SaveSettings(json& output) override;
	void RestoreDefaultSettings() override;
	void EarlyPrepass() override;
	void OnRuntimeDisabled() override;
	void OnSceneTransitionReset(bool opening) override;
	bool IsActive() const;
	std::optional<Submission> Dispatch(ID3D12GraphicsCommandList4* commands, ID3D12Resource* rasterColor,
		ID3D12Resource* depth, uint32_t width, uint32_t height);
	void OnSubmitted(ID3D12Fence* fence, uint64_t value, Submission submission, bool applied);

private:
	bool EnsurePipelines(ID3D12Device* device);
	std::shared_ptr<Surfaces> MakeSurfaces(ID3D12Resource* color, ID3D12Resource* depth, uint32_t width, uint32_t height);
	winrt::com_ptr<ID3D12Resource> Upload(const void* source, size_t bytes);
	D3D12_GPU_DESCRIPTOR_HANDLE Descriptor(const Surfaces& surfaces, uint32_t index) const;
	void Collect();

	struct Pending
	{
		winrt::com_ptr<ID3D12Fence> fence;
		uint64_t value = 0;
		Submission submission;
	};
	std::deque<Pending> pending;
	std::shared_ptr<Surfaces> surfaces;
	winrt::com_ptr<ID3D12Device> device;
	winrt::com_ptr<ID3D12RootSignature> shadowRoot, compositeRoot;
	winrt::com_ptr<ID3D12PipelineState> shadowPipeline, compositePipeline;
	uint32_t descriptorSize = 0;
	uint32_t lastSubmitFrame = 0;
	uint64_t submittedGeometry = 0, submittedLights = 0;
	bool unavailable = false;
	Settings settings;
};
