#pragma once

#include "MaterialCatalog.h"
#include "MaterialInterpretation.h"
#include "Materials/PBRNifMaterial.h"
#include "Utils/StringUtils.h"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace NativePBR
{
	/** @brief Engine-pointer-free data captured when a NIF material is linked or replaced. */
	struct MaterialSource
	{
		MaterialData base;
		std::string textureSetEditorId;
		std::optional<PBRRecords::NifSurfaceFlags> nifFlags;
		std::optional<std::array<float, 4>> nifThinSurface;
		std::optional<std::array<float, 2>> nifFuzz;
	};

	enum MaterialSourceIssue : uint32_t
	{
		MissingTextureSetConfiguration = 1u << 0,
		InvalidNifThinSurface = 1u << 1,
		InvalidNifFuzz = 1u << 2,
		NonFiniteMaterialParameters = 1u << 3,
		IgnoredNifExtrasForLayeredMaterial = 1u << 4
	};

	inline constexpr std::array<std::string_view, 5> kMaterialSourceIssueNames{
		"missing-texture-set-configuration", "invalid-nif-thin-surface", "invalid-nif-fuzz",
		"non-finite-material-parameters", "ignored-nif-extras-for-layered-material"
	};

	/** @brief Material discovery diagnostics are observations, never render eligibility gates. */
	struct MaterialSourceReport
	{
		enum class Status : uint8_t { Published, Unchanged, InvalidParameters, MissingKey, NotFound };
		Status status = Status::NotFound;
		uint64_t identity = 0, revision = 0;
		uint32_t issues = 0;
	};

	/** @brief Owns native material source identity independently of TruePBR and scene geometry. */
	class MaterialDiscovery
	{
	public:
		/** @brief Register raw NIF/JSON inputs using a stable asset-and-material key. */
		MaterialSourceReport Register(std::string a_key, MaterialSource a_source,
			const std::shared_ptr<const MaterialCatalogSnapshot>& a_catalog)
		{
			if (a_key.empty() || a_key.size() > 2048)
				return { MaterialSourceReport::Status::MissingKey };
			std::scoped_lock lock(mutex);
			a_key = Util::ToLowerAscii(a_key);
			auto [it, inserted] = sources.try_emplace(std::move(a_key));
			if (inserted)
				it->second = std::make_unique<Source>();
			auto& source = *it->second;
			source.input = std::move(a_source);
			return UpdateSource(source, a_catalog);
		}

		/** @brief Reinterpret registered sources after a JSON catalog reload. */
		void RefreshCatalog(const std::shared_ptr<const MaterialCatalogSnapshot>& a_catalog)
		{
			std::scoped_lock lock(mutex);
			for (auto& [key, source] : sources)
				UpdateSource(*source, a_catalog);
		}

		/** @brief Bind a reference and shape without modifying a shared recipe. */
		bool ObserveInstance(std::string_view a_key, std::string a_reference, std::string a_shape, InstanceProperties a_properties = {})
		{
			std::scoped_lock lock(mutex);
			const auto it = sources.find(Util::ToLowerAscii(a_key));
			if (it == sources.end())
				return false;
			registry.ObserveInstance(it->second.get(), std::move(a_reference), std::move(a_shape), std::move(a_properties));
			return true;
		}

		/** @brief Update a verified instance override independently of shared material parameters. */
		bool ReplaceInstance(std::string_view a_key, std::string_view a_reference, std::string_view a_shape,
			InstanceProperties a_properties)
		{
			std::scoped_lock lock(mutex);
			const auto it = sources.find(Util::ToLowerAscii(a_key));
			return it != sources.end() && registry.ReplaceInstance(it->second.get(), a_reference, a_shape, std::move(a_properties));
		}

		/** @brief Retire one source before its storage can be reused. */
		bool Retire(std::string_view a_key)
		{
			std::scoped_lock lock(mutex);
			const auto it = sources.find(Util::ToLowerAscii(a_key));
			if (it == sources.end())
				return false;
			registry.Forget(it->second.get());
			sources.erase(it);
			return true;
		}

		/** @brief Invalidate scene associations while retaining reusable materials. */
		void BeginInstanceEpoch()
		{
			std::scoped_lock lock(mutex);
			registry.BeginInstanceEpoch();
		}

		/** @brief Query source-specific reasons even if its material cannot be published yet. */
		MaterialSourceReport Inspect(std::string_view a_key) const
		{
			std::scoped_lock lock(mutex);
			const auto it = sources.find(Util::ToLowerAscii(a_key));
			return it == sources.end() ? MaterialSourceReport{} : it->second->report;
		}

		/** @brief Resolve diagnostic reasons by an already-published material identity. */
		MaterialSourceReport Inspect(uint64_t a_identity) const
		{
			std::scoped_lock lock(mutex);
			for (const auto& [key, source] : sources)
				if (source->report.identity == a_identity)
					return source->report;
			return {};
		}

		/** @brief Count owned sources without inspecting engine or geometry state. */
		size_t Count() const
		{
			std::scoped_lock lock(mutex);
			return sources.size();
		}

		/** @brief Material service consumes only this registry's immutable exports. */
		const Registry& GetRegistry() const { return registry; }
		/** @brief NativePBR's worker observes the same registry as asset discovery. */
		Registry& MutableRegistry() { return registry; }

	private:
		struct Source
		{
			MaterialSource input;
			std::optional<MaterialData> publishedData;
			MaterialSourceReport report;
		};

		static std::pair<MaterialData, uint32_t> Interpret(const MaterialSource& a_source,
			const std::shared_ptr<const MaterialCatalogSnapshot>& a_catalog)
		{
			MaterialData material = a_source.base;
			uint32_t issues = 0;
			if (a_source.nifFlags && material.kind == MaterialKind::Mesh) {
				const auto nif = PBRRecords::InterpretNifSurface(*a_source.nifFlags);
				if (nif.applyThinSurfaceFlags) {
					material.nifThinSurfaceFlags |= nif.thinSurfaceFlags;
					material.pbrFlags |= material.nifThinSurfaceFlags;
				}
				material.pbrFlags |= nif.pbrFlags;
				if (nif.enableGlint)
					material.glint.enabled = true;
			}
			if (!a_source.textureSetEditorId.empty()) {
				const auto key = Util::ToLowerAscii(a_source.textureSetEditorId);
				if (!a_catalog || !a_catalog->textureSets.contains(key))
					issues |= MissingTextureSetConfiguration;
				else
					material = InterpretTextureSet(material, a_catalog->textureSets.at(key));
			}
			const bool extrasSupported = (material.pbrFlags & (uint32_t(PBRFlags::TwoLayer) | uint32_t(PBRFlags::HairMarschner))) == 0;
			if (!extrasSupported && (a_source.nifThinSurface || a_source.nifFuzz))
				issues |= IgnoredNifExtrasForLayeredMaterial;
			if (extrasSupported && a_source.nifThinSurface && !InterpretNifThinSurface(*a_source.nifThinSurface, material))
				issues |= InvalidNifThinSurface;
			if (extrasSupported && a_source.nifFuzz && !InterpretNifFuzz(*a_source.nifFuzz, material))
				issues |= InvalidNifFuzz;
			material.projectedColor = { 1.0f, 1.0f, 1.0f };
			material.projectedRoughness = 1.0f;
			material.projectedSpecularLevel = 0.04f;
			material.projectedGlint = {};
			if (material.kind == MaterialKind::Mesh)
				material.texturePathsComplete = !material.texturePaths[0].empty() &&
					!material.texturePaths[1].empty() && !material.texturePaths[2].empty();
			if (!IsFinite(material))
				issues |= NonFiniteMaterialParameters;
			return { std::move(material), issues };
		}

		MaterialSourceReport UpdateSource(Source& a_source,
			const std::shared_ptr<const MaterialCatalogSnapshot>& a_catalog)
		{
			auto [material, issues] = Interpret(a_source.input, a_catalog);
			a_source.report.issues = issues;
			if (issues & NonFiniteMaterialParameters) {
				if (a_source.publishedData)
					registry.MarkDirty(&a_source);
				const auto capture = registry.BeginCapture(&a_source);
				a_source.report.identity = capture.identity;
				a_source.report.revision = registry.Revision();
				a_source.report.status = MaterialSourceReport::Status::InvalidParameters;
				return a_source.report;
			}
			if (a_source.publishedData && *a_source.publishedData != material)
				registry.MarkDirty(&a_source);
			const auto capture = registry.BeginCapture(&a_source);
			if (!capture) {
				a_source.report.status = MaterialSourceReport::Status::Unchanged;
				return a_source.report;
			}
			if (!registry.Publish(&a_source, capture, material)) {
				a_source.report.status = MaterialSourceReport::Status::InvalidParameters;
				return a_source.report;
			}
			a_source.report = { MaterialSourceReport::Status::Published, capture.identity, registry.Revision(), issues };
			a_source.publishedData = std::move(material);
			return a_source.report;
		}

		mutable std::mutex mutex;
		Registry registry;
		std::unordered_map<std::string, std::unique_ptr<Source>> sources;
	};
}
