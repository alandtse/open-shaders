#pragma once

namespace PBRRecords
{
	struct GlintParameters
	{
		bool enabled = false;
		float screenSpaceScale = 1.5f;
		float logMicrofacetDensity = 40.f;
		float microfacetRoughness = .015f;
		float densityRandomization = 2.f;
	};

	struct TextureSetData
	{
		float roughnessScale = 1.f;
		float displacementScale = 1.f;
		float specularLevel = 0.04f;

		RE::NiColor subsurfaceColor;
		float subsurfaceOpacity = 0.f;
		float subsurfaceBalance = 0.75f;
		bool thinSurface = false;
		bool foliage = false;
		float thinSurfaceSpecularWeight = 0.5f;
		float thinSurfaceBackSpecularWeight = 0.5f;
		float thinSurfaceBackRoughnessScale = 1.f;

		RE::NiColor coatColor = { 1.f, 1.f, 1.f };
		float coatStrength = 1.f;
		float coatRoughness = 1.f;
		float coatSpecularLevel = 0.04f;
		float innerLayerDisplacementOffset = 0.f;

		RE::NiColor fuzzColor;
		float fuzzWeight = 0.f;
		float fuzzRoughness = -1.f;
		bool fuzzRoughnessTexture = false;

		GlintParameters glintParameters;
	};

	struct MaterialObjectData
	{
		std::array<float, 3> baseColorScale = { 1.f, 1.f, 1.f };
		float roughness = 1.f;
		float specularLevel = 0.04f;

		GlintParameters glintParameters;
	};

	/** @brief Serialize the canonical PBR glint recipe. */
	void to_json(json& a_json, const GlintParameters& a_data);
	/** @brief Deserialize the canonical PBR glint recipe. */
	void from_json(const json& a_json, GlintParameters& a_data);
	/** @brief Serialize a PBR texture-set recipe. */
	void to_json(json& a_json, const TextureSetData& a_data);
	/** @brief Deserialize a PBR texture-set recipe. */
	void from_json(const json& a_json, TextureSetData& a_data);
	/** @brief Serialize a PBR material-object recipe. */
	void to_json(json& a_json, const MaterialObjectData& a_data);
	/** @brief Deserialize a PBR material-object recipe. */
	void from_json(const json& a_json, MaterialObjectData& a_data);
}
