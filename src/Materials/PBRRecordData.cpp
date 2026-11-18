#include "PBRRecordData.h"

#include "Utils/Serialize.h"

namespace PBRRecords
{
	namespace detail
	{
	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
		GlintParameters,
		enabled,
		screenSpaceScale,
		logMicrofacetDensity,
		microfacetRoughness,
		densityRandomization);

	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
		TextureSetData,
		roughnessScale,
		displacementScale,
		specularLevel,
		subsurfaceColor,
		subsurfaceOpacity,
		subsurfaceBalance,
		thinSurface,
		foliage,
		thinSurfaceSpecularWeight,
		thinSurfaceBackSpecularWeight,
		thinSurfaceBackRoughnessScale,
		coatColor,
		coatStrength,
		coatRoughness,
		coatSpecularLevel,
		innerLayerDisplacementOffset,
		fuzzColor,
		fuzzWeight,
		fuzzRoughness,
		fuzzRoughnessTexture,
		glintParameters);

	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
		MaterialObjectData,
		baseColorScale,
		roughness,
		specularLevel,
		glintParameters);
	}

	void to_json(json& a_json, const GlintParameters& a_data) { detail::to_json(a_json, a_data); }
	void from_json(const json& a_json, GlintParameters& a_data) { detail::from_json(a_json, a_data); }
	void to_json(json& a_json, const TextureSetData& a_data) { detail::to_json(a_json, a_data); }
	void from_json(const json& a_json, TextureSetData& a_data) { detail::from_json(a_json, a_data); }
	void to_json(json& a_json, const MaterialObjectData& a_data) { detail::to_json(a_json, a_data); }
	void from_json(const json& a_json, MaterialObjectData& a_data) { detail::from_json(a_json, a_data); }
}
