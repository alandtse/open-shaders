#pragma once

#include "MaterialSidecarContract.h"
#include <d3d11.h>
#include <winrt/base.h>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace NativePBR
{
	enum class PreviewMode : uint32_t
	{
		Off = 0,
		MaterialIdentity = 1,
		UvDerivatives = 2,
		Coverage = 3
	};
	// Diagnostic classifications only. Non-PBR materials never enter the native identity table.
	enum class PreviewSurveyClass : uint32_t
	{
		None = 0,
		OpaquePbrCandidate = 1,
		OrdinaryStatic = 2,
		UnsupportedProjectedUv = 3,
		UnsupportedModelSpaceNormals = 4,
		UnsupportedLandscape = 5,
		UnsupportedOther = 6
	};
	// Independent reasons; a draw may have several, even though it receives only one preview color.
	enum UnsupportedPbrReason : uint32_t
	{
		ProjectedUvReason = 1u << 0,
		ModelSpaceNormalsReason = 1u << 1,
		LandscapeReason = 1u << 2,
		OtherReason = 1u << 3
	};

	/** FXC capture data only. DX12 owns later material evaluation. */
	class MaterialSidecar
	{
	public:
		static constexpr size_t kCount = static_cast<size_t>(SidecarTarget::Count);
		using Sources = std::array<ID3D11Texture2D*, kCount>;

		void Prepare(ID3D11Device* a_device);
		void BeginFrame(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_mainTarget,
			uint32_t a_frame, uint32_t a_width, uint32_t a_height, bool a_captureProjectedUv, bool a_inputOnly = false);
		void BeginPass(uint64_t a_materialIdentity, uint64_t a_materialRevision, std::string a_referenceKey, std::string a_shapeName,
			PreviewSurveyClass a_surveyClass, uint32_t a_unsupportedReasons);
		bool StageInputOnlyShader(ID3D11PixelShader* a_shader);
		bool BeginInputOnlyPass(ID3D11DeviceContext* a_context);
		void EndInputOnlyDraw();
		void EndPass();
		void ReplayDrawIndexed(ID3D11DeviceContext* a_context, UINT a_count, UINT a_first, INT a_base, PreviewMode a_previewMode, bool a_inputBound);
		void FinishFrame(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_finalDepth);
		Sources GetSources(uint32_t a_frame) const;
		std::shared_ptr<const std::vector<SidecarDraw>> GetDrawTable() const { return publishedDraws; }
		size_t PublishedDrawCount() const { return publishedDraws ? publishedDraws->size() : 0; }
		uint32_t GetFrame() const { return frame; }
		uint64_t ReplayedDraws() const { return replayed; }
		uint64_t RejectedDraws() const { return rejected; }
		uint64_t FailedFrames() const { return failedFrames; }
		uint64_t PreviewDraws() const { return previewDraws; }
		uint64_t PreviewMissingIdentities() const { return previewMissingIdentities; }
		uint64_t PreviewOrdinaryDraws() const { return previewOrdinaryDraws; }
		uint64_t PreviewUnsupportedDraws() const { return previewUnsupportedDraws; }
		std::array<uint64_t, 4> PreviewUnsupportedReasons() const { return previewUnsupportedReasons; }
		std::array<uint32_t, 4> UnsupportedReasonsThisFrame() const { return unsupportedReasonsThisFrame; }
		uint32_t SurveyDrawsThisFrame() const { return surveyDrawsThisFrame; }
		uint32_t ProjectedDrawsThisFrame() const { return projectedDrawsThisFrame; }
		uint32_t InputOnlyDrawsThisFrame() const { return inputDrawsThisFrame; }
		uint32_t InputShaderMissesThisFrame() const { return inputShaderMissesThisFrame; }
		uint32_t InputTargetRejectionsThisFrame() const { return inputTargetRejectionsThisFrame; }
		uint32_t InputCapacityMissesThisFrame() const { return inputCapacityMissesThisFrame; }
		uint32_t UnsupportedDrawsThisFrame() const { return unsupportedDrawsThisFrame; }
		uint32_t SetupPassesThisFrame() const { return setupPassesThisFrame; }
		uint32_t PbrCandidatesThisFrame() const { return pbrCandidatesThisFrame; }
		uint32_t MissingPbrIdentitiesThisFrame() const { return missingPbrIdentitiesThisFrame; }
		uint32_t SurveyLimit() const { return kSurveyDrawLimit; }
		bool InputOnly() const { return inputOnly; }
		bool PreviewReady() const { return static_cast<bool>(previewShader); }
		const std::string& Status() const { return status; }

	private:
		static constexpr uint32_t kSurveyDrawLimit = kMaximumFrameDraws;
		struct Target
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11RenderTargetView> rtv;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		};
		struct Constants
		{
			uint32_t ordinal, reserved, materialLow, materialHigh;
		};
		std::array<Target, kCount> targets{};
		winrt::com_ptr<ID3D11PixelShader> shader, projectedShader, previewShader;
		winrt::com_ptr<ID3D11ComputeShader> validation;
		winrt::com_ptr<ID3D11Buffer> drawConstants, validationConstants;
		winrt::com_ptr<ID3D11BlendState> writeAll;
		winrt::com_ptr<ID3D11RenderTargetView> expectedMain;
		winrt::com_ptr<ID3D11PixelShader> stagedInputShader;
		struct DepthState
		{
			winrt::com_ptr<ID3D11DepthStencilState> source, replay;
		};
		std::vector<DepthState> depthStates;
		std::vector<SidecarDraw> draws;
		std::shared_ptr<const std::vector<SidecarDraw>> publishedDraws;
		std::string activeReference, activeShape;
		uint64_t activeMaterial = 0, activeRevision = 0, replayed = 0, rejected = 0, failedFrames = 0, previewDraws = 0,
			previewMissingIdentities = 0, previewOrdinaryDraws = 0, previewUnsupportedDraws = 0;
		std::array<uint64_t, 4> previewUnsupportedReasons{};
		std::array<uint32_t, 4> unsupportedReasonsThisFrame{};
		uint32_t frame = 0, width = 0, height = 0, nextDraw = 1, surveyDrawsThisFrame = 0, projectedDrawsThisFrame = 0;
		uint32_t inputDrawsThisFrame = 0, inputShaderMissesThisFrame = 0, inputTargetRejectionsThisFrame = 0;
		uint32_t inputCapacityMissesThisFrame = 0, unsupportedDrawsThisFrame = 0;
		uint32_t setupPassesThisFrame = 0, pbrCandidatesThisFrame = 0, missingPbrIdentitiesThisFrame = 0;
		uint32_t activeUnsupportedReasons = 0;
		bool recording = false, ready = false, captureProjectedUv = false, inputOnly = false, inputPassActive = false;
		PreviewSurveyClass activeSurveyClass = PreviewSurveyClass::None;
		std::string status;
	};
}
