#pragma once

#include <cstdint>

struct ID3D11Texture2D;

/** @brief Optional indirect-lighting reconstruction provider; callers retain ownership of inputs. */
struct LightingReconstruction
{
	virtual ~LightingReconstruction() = default;
	/** @brief Reports whether indirect-specular reconstruction is available. */
	virtual bool CanDispatchRayRegeneration() const = 0;
	/** @brief Reconstructs the current signal through the provider's synchronized resource bridge. */
	virtual bool DispatchRayRegeneration(ID3D11Texture2D* a_signal, ID3D11Texture2D* a_linearDepth, ID3D11Texture2D* a_motionVectors,
		ID3D11Texture2D* a_normalRoughness, ID3D11Texture2D* a_specularAlbedo, ID3D11Texture2D* a_diffuseAlbedo,
		ID3D11Texture2D* a_output, uint32_t a_width, uint32_t a_height, bool a_resetHistory) = 0;
	/** @brief Ensures the optional radiance-cache provider is available. */
	virtual bool EnsureRadianceCacheContext() = 0;
	/** @brief Polls completed inference and optionally submits a batch without waiting for GPU completion. */
	virtual bool DispatchRadianceCacheCapturedInference(const float* a_inputValues, const uint32_t* a_sourceQueryIndices,
		uint32_t a_sampleCount, const float* a_trainingInputValues, const float* a_trainingTargetValues,
		uint32_t a_trainingSampleCount, float* a_outputRadiance, uint32_t* a_outputSourceQueryIndices,
		uint32_t* a_outputSampleCount, bool* a_outputReady, bool* a_outputSubmitted) = 0;
	/** @brief Requests a cache reset on the next submitted batch. */
	virtual void RequestRadianceCacheReset() = 0;
	/** @brief Selects reconstruction diagnostics. */
	virtual void SetRRDiagnosticsEnabled(bool a_enabled) = 0;
	/** @brief Applies training parameters to subsequent batches. */
	virtual void SetRadianceCacheTrainingParameters(float a_learningRate, float a_weightSmoothing) = 0;
};
