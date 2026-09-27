#include "EngineFix.h"

#include "EngineFixes/AlphaGeometryGroupCeilingFix.h"
#include "EngineFixes/CullPoolExhaustionFix.h"
#include "EngineFixes/EffectShaderNoDecalsFix.h"
#include "EngineFixes/ShadowParabolicNullAccumulatorFix.h"
#include "EngineFixes/ShadowmapCascadeCullingFix.h"
#include "EngineFixes/ShadowmapCascadeRasterizerFix.h"

const std::vector<EngineFix*>& EngineFix::GetOnPostPostLoadFixesList()
{
	static AlphaGeometryGroupCeilingFix alphaGeometryGroupCeilingFix;
	static CullPoolExhaustionFix cullPoolExhaustionFix;
	static EffectShaderNoDecalsFix effectShaderNoDecalsFix;
	static ShadowmapCascadeCullingFix shadowmapCascadeCullingFix;
	static ShadowmapRasterizerFix shadowmapRasterizerFix;
	static ShadowParabolicNullAccumulatorFix shadowParabolicNullAccumulatorFix;

	static std::vector<EngineFix*> fixes = {
		&alphaGeometryGroupCeilingFix,
		&cullPoolExhaustionFix,
		&effectShaderNoDecalsFix,
		&shadowmapCascadeCullingFix,
		&shadowmapRasterizerFix,
		&shadowParabolicNullAccumulatorFix
	};

	return fixes;
}

const std::vector<EngineFix*>& EngineFix::GetOnDataLoadedFixesList()
{
	static std::vector<EngineFix*> fixes = {};

	return fixes;
}

void EngineFix::InstallFixes(const std::vector<EngineFix*>& fixes)
{
	static const auto isInstalledByEngineFixes = []() {
		const auto module = GetModuleHandleA("EngineFixes.dll") ? GetModuleHandleA("EngineFixes.dll") : GetModuleHandleA("EngineFixesVR.dll");
		return module ? reinterpret_cast<bool (*)(const char*)>(GetProcAddress(module, "EngineFixes_IsFixInstalled")) : nullptr;
	}();

	for (const auto fix : fixes) {
		const auto engineFixesName = fix->GetEngineFixesName();
		if (engineFixesName && isInstalledByEngineFixes && isInstalledByEngineFixes(engineFixesName)) {
			logger::info("[Engine Fixes] Skipped {} (EngineFixes installed it)", fix->GetName());
			continue;
		}
		fix->Install();
		logger::info("[Engine Fixes] Installed {}", fix->GetName());
	}
}

void EngineFix::InstallOnPostPostLoadFixes()
{
	InstallFixes(GetOnPostPostLoadFixesList());
}

void EngineFix::InstallOnDataLoadedFixes()
{
	InstallFixes(GetOnDataLoadedFixesList());
}
