#include "MaterialCatalog.h"

#include "Materials/PBRRecordConfig.h"
#include "Utils/StringUtils.h"

const PBRRecords::TextureSetData* NativePBR::MaterialCatalogSnapshot::FindTextureSet(std::string_view a_editorId) const
{
	auto it = textureSets.find(Util::ToLowerAscii(a_editorId));
	return it == textureSets.end() ? nullptr : &it->second;
}

const PBRRecords::TextureSetData* NativePBR::MaterialCatalogSnapshot::FindTextureSet(const RE::TESForm* a_form) const
{
	const char* editorId = a_form ? a_form->GetFormEditorID() : nullptr;
	return editorId ? FindTextureSet(editorId) : nullptr;
}

const PBRRecords::MaterialObjectData* NativePBR::MaterialCatalogSnapshot::FindMaterialObject(std::string_view a_editorId) const
{
	auto it = materialObjects.find(Util::ToLowerAscii(a_editorId));
	return it == materialObjects.end() ? nullptr : &it->second;
}

const PBRRecords::MaterialObjectData* NativePBR::MaterialCatalogSnapshot::FindMaterialObject(const RE::TESForm* a_form) const
{
	const char* editorId = a_form ? a_form->GetFormEditorID() : nullptr;
	return editorId ? FindMaterialObject(editorId) : nullptr;
}

void NativePBR::MaterialCatalog::Load(bool a_verbose)
{
	auto catalog = std::make_shared<MaterialCatalogSnapshot>();
	PBRRecords::ReadConfigs("Data\\PBRTextureSets", [catalog](const std::string& a_editorId, const json& a_config) {
		try {
			catalog->textureSets.insert_or_assign(Util::ToLowerAscii(a_editorId), a_config.get<PBRRecords::TextureSetData>());
			return true;
		} catch (const std::exception& e) {
			++catalog->rejectedTextureSets;
			logger::warn("[NativePBR] texture set {} rejected: {}", a_editorId, e.what());
			return false;
		}
	}, a_verbose, "NativePBR");

	PBRRecords::ReadConfigs("Data\\PBRMaterialObjects", [catalog](const std::string& a_editorId, const json& a_config) {
		try {
			catalog->materialObjects.insert_or_assign(Util::ToLowerAscii(a_editorId), a_config.get<PBRRecords::MaterialObjectData>());
			return true;
		} catch (const std::exception& e) {
			++catalog->rejectedMaterialObjects;
			logger::warn("[NativePBR] material object {} rejected: {}", a_editorId, e.what());
			return false;
		}
	}, a_verbose, "NativePBR");

	published.store(std::move(catalog), std::memory_order_release);
}

std::shared_ptr<const NativePBR::MaterialCatalogSnapshot> NativePBR::MaterialCatalog::GetSnapshot() const
{
	return published.load(std::memory_order_acquire);
}
