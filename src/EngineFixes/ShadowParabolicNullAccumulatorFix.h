#pragma once

/**
 * @brief Guards a null-this crash in BSParabolicCullingProcess::SetBackHemisphereAccumulator.
 *
 * The engine calls it from BSShadowLight::Accumulate for any omni/dual-paraboloid light,
 * assuming its per-instance BSParabolicCullingProcess was already lazily allocated by
 * ShadowSceneNode::AccumulateLight. A light newly promoted normal->shadow and Accumulate'd
 * by our own scheduler before that lazy allocation has run reaches this with a null `this`.
 */
struct ShadowParabolicNullAccumulatorFix : EngineFix
{
	std::string GetName() override { return "Shadow Parabolic Null Accumulator Fix"; }

	void Install() override;

	struct BSParabolicCullingProcess_SetBackHemisphereAccumulator
	{
		static void thunk(void* a_this, void* a_accumulator);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
