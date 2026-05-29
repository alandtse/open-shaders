#pragma once

#include "Buffer.h"
#include "Features/LightLimitFix/ParticleLights.h"
#include "LightLimitFix/ShadowCasterManager.h"
#include "OverlayFeature.h"

#include <mutex>
#include <shared_mutex>

struct LightLimitFix : OverlayFeature
{
private:
	static constexpr uint32_t MAX_LIGHTS = 1024;
	static constexpr uint32_t CLUSTER_MAX_LIGHTS = 128;

public:
	virtual inline std::string GetName() override { return "Light Limit Fix"; }
	virtual inline std::string GetShortName() override { return "LightLimitFix"; }
	virtual inline std::string_view GetShaderDefineName() override { return "LIGHT_LIMIT_FIX"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Light Limit Fix removes the vanilla game's 4-light limit, allowing unlimited dynamic lights in scenes. "
			"It also extends shadow support to all point and spot lights.",
			{ "Removes 4-light limit",
				"Unlimited dynamic lights",
				"Shadow support for point and spot lights",
				"Improved lighting quality",
				"Particle lights from configurable INI" }
		};
	}

	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	enum class LightFlags : std::uint32_t
	{
		PortalStrict = (1 << 0),
		Shadow = (1 << 1),
		Simple = (1 << 2),

		Initialised = (1 << 8),
		Disabled = (1 << 9),
		InverseSquare = (1 << 10),
		Linear = (1 << 11),
		Particle = (1 << 12),
	};

	struct PositionOpt
	{
		float3 data;
		uint pad0;
	};

	struct alignas(16) LightData
	{
		float3 color;
		float fade = 1.0f;
		float radius;
		float invRadius;
		float fadeZone;
		float sizeBias;
		PositionOpt positionWS[2];
		uint128_t roomFlags = uint32_t(0);
		stl::enumeration<LightFlags> lightFlags;
		uint32_t shadowMapIndex = 0;
		float2 pad0;
	};
	STATIC_ASSERT_ALIGNAS_16(LightData);

	struct ClusterAABB
	{
		float4 minPoint;
		float4 maxPoint;
	};

	struct alignas(16) LightGrid
	{
		uint offset;
		uint lightCount;
		uint pad0[2];
	};
	STATIC_ASSERT_ALIGNAS_16(LightGrid);

	struct alignas(16) LightBuildingCB
	{
		float LightsNear;
		float LightsFar;
		uint pad0[2];
		uint ClusterSize[4];
	};
	STATIC_ASSERT_ALIGNAS_16(LightBuildingCB);

	struct alignas(16) LightCullingCB
	{
		uint LightCount;
		uint pad[3];
		uint ClusterSize[4];
	};
	STATIC_ASSERT_ALIGNAS_16(LightCullingCB);

	struct alignas(16) PerFrame
	{
		uint EnableContactShadows;
		uint ContactShadowMaxSteps;
		float ContactShadowMaxDistance;
		float ContactShadowStride;
		float ContactShadowThickness;
		float ContactShadowDepthFade;
		float ContactShadowMinIntensity;
		uint32_t ShadowMapSlots;  // total shadow map texture-array capacity
		// Cluster config (computed)
		uint ClusterSize[4];
		// Debug (last)
		uint EnableLightsVisualisation;
		uint LightsVisualisationMode;
		uint EnableParticleContactShadows;
		uint pad0;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);
	// Compile-time size lock catches CPU/GPU cbuffer layout drift. STATIC_ASSERT_ALIGNAS_16
	// only enforces the 16-byte alignment / multiple-of-16 contract that HLSL constant
	// buffers require; it doesn't notice if a field is added, removed, or resized in a
	// way that still happens to land on a 16-byte boundary. The shader-side mirror is
	// SharedData::LightLimitFixSettings in package/Shaders/Common/SharedData.hlsli
	// (embedded in the shared FeatureData cbuffer at b6), and must match this layout
	// field-for-field. Update both sides when the layout changes, then bump this constant.
	static_assert(sizeof(PerFrame) == 64,
		"LightLimitFix::PerFrame layout drifted -- update SharedData::LightLimitFixSettings in package/Shaders/Common/SharedData.hlsli to match, then update this assert.");

	PerFrame GetCommonBufferData();

	struct alignas(16) StrictLightDataCB
	{
		uint NumStrictLights;
		int RoomIndex;
		uint ShadowBitMask;
		uint pad0;
		LightData StrictLights[15];
	};
	STATIC_ASSERT_ALIGNAS_16(StrictLightDataCB);

	StrictLightDataCB strictLightDataTemp;

	struct CachedParticleLight
	{
		float grey;
		RE::NiPoint3 position;
		float radius;
	};

	struct ParticleLightInfo
	{
		bool billboard;
		RE::BSGeometry* node;
		RE::NiColorA color;
		float radiusMult = 1.0f;
	};

	struct ParticleLightReference
	{
		bool valid = false;
		bool billboard = false;
		// keeps billboard effect-material/emittance tint after detection
		bool applyEffectMaterialTint = true;
		ParticleLights::Config config{};
		bool hasGradientConfig = false;
		ParticleLights::GradientConfig gradientConfig{};
		RE::NiColorA baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
		std::uint64_t configVersion = 0;
	};

	eastl::hash_map<RE::BSGeometry*, ParticleLightReference> particleLightsReferences;
	eastl::vector<ParticleLightInfo> queuedParticleLights;
	eastl::vector<ParticleLightInfo> currentParticleLights;
	std::mutex particleLightsQueueMutex;

	std::shared_mutex cachedParticleLightsMutex;
	eastl::vector<CachedParticleLight> cachedParticleLights;

	// JSON-placed light cache (rebuilt per frame); paired with InverseSquareLighting metadata.
	eastl::hash_map<RE::NiLight*, bool> jsonPlacedLightCache;
	Util::FrameChecker jsonPlacedLightCacheFrameChecker;

	ConstantBuffer* strictLightDataCB = nullptr;

	int eyeCount = !REL::Module::IsVR() ? 1 : 2;

	// Debug-only visualization state. Lives on the instance rather than in
	// Settings so it can't accidentally persist into a user's config: a
	// shipped JSON with `EnableLightsVisualisation = true` would force every
	// load to compile the heavier LLFDEBUG shader permutation. These reset to
	// off on each session.
	bool EnableLightsVisualisation = false;
	uint LightsVisualisationMode = 0;
	bool previousEnableLightsVisualisation = false;
	bool currentEnableLightsVisualisation = false;

	ID3D11ComputeShader* clusterBuildingCS = nullptr;
	ID3D11ComputeShader* clusterCullingCS = nullptr;

	ConstantBuffer* lightBuildingCB = nullptr;
	ConstantBuffer* lightCullingCB = nullptr;

	eastl::unique_ptr<Buffer> lights = nullptr;
	eastl::unique_ptr<Buffer> clusters = nullptr;
	eastl::unique_ptr<Buffer> lightIndexCounter = nullptr;
	eastl::unique_ptr<Buffer> lightIndexList = nullptr;
	eastl::unique_ptr<Buffer> lightGrid = nullptr;

	std::uint32_t lightCount = 0;
	float lightsNear = 1;
	float lightsFar = 16384;

	RE::NiPoint3 eyePositionCached[2]{};
	bool wasEmpty = false;
	bool wasWorld = false;
	int previousRoomIndex = -1;

	Util::FrameChecker frameChecker;

	// Point/spot shadow resources (t102, t103 -- t100/t101 reserved for Grass Collision)
	// shadowLights is lazily allocated in CopyShadowLightData() since shadowMapSlots
	// is not known until Deferred::SetupResources() runs (after Feature::SetupResources()).
	Buffer* shadowLights = nullptr;
	uint32_t shadowLightsCapacity = 0;

	// Per-frame shadow accounting (displayed in DrawSettings Statistics tree).
	uint32_t shadowLightCount = 0;            // distinct lights processed (including dropped)
	uint32_t shadowUnshadowedLightCount = 0;  // lights that exceeded slot capacity

	/// Generate a text legend mapping each shadow-map slot index to its golden-ratio hue
	/// and light type.  Used for RenderDoc capture comments when mode 8 is active.
	std::string BuildShadowSlotColorLegend() const;

	virtual void SetupResources() override;
	virtual void Reset() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	virtual void DrawSettings() override;
	virtual void DrawOverlay() override;
	virtual bool IsOverlayVisible() const override
	{
		return EnableLightsVisualisation || settings.ShowShadowOverlay ||
		       ShadowCasterManager::HasSuppressedLights() || ShadowCasterManager::HasAnyOverrides();
	}

	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual void ClearShaderCache() override;

	float CalculateLightDistance(float3 a_lightPosition, float a_radius);
	void AddCachedParticleLights(eastl::vector<LightData>& lightsData, LightLimitFix::LightData& light);
	void SetLightPosition(LightLimitFix::LightData& a_light, RE::NiPoint3 a_initialPosition, bool a_cached = true);
	void RefreshJsonPlacedLightCacheFrame();
	bool IsJsonPlacedLight(RE::BSLight* a_bsLight, RE::NiLight* a_niLight);
	void ApplyJsonPlacedLightIntensityScale(
		LightData& a_light,
		RE::BSLight* a_bsLight,
		RE::NiLight* a_niLight,
		bool a_isPortalStrict,
		bool a_isInterior);
	void UpdateLights();
	void UpdateStructure();
	virtual void EarlyPrepass() override;
	virtual void Prepass() override;
	void CopyShadowLightData();

	// Shadow rendering helpers (implemented in LightLimitFix/ShadowRenderer.cpp)

	float CalculateLuminance(CachedParticleLight& light, RE::NiPoint3& point);
	void AddParticleLightLuminance(RE::NiPoint3& targetPosition, int& numHits, float& lightLevel);

	ParticleLightReference GetParticleLightConfigs(RE::BSRenderPass* a_pass);
	bool AddParticleLight(RE::BSRenderPass* a_pass, ParticleLightReference a_reference);
	bool CheckParticleLights(RE::BSRenderPass* a_pass, uint32_t a_technique);
	void ProcessQueuedParticleLights(eastl::vector<LightData>& lightsData);

	// Inline-defined because Particle.cpp calls this; static-inline class-scope helpers must have
	// the body visible in every translation unit that uses them.
	static inline float3 Saturation(float3 color, float saturation)
	{
		const float grey = color.Dot(float3(0.3f, 0.59f, 0.11f));
		color.x = std::max(std::lerp(grey, color.x, saturation), 0.0f);
		color.y = std::max(std::lerp(grey, color.y, saturation), 0.0f);
		color.z = std::max(std::lerp(grey, color.z, saturation), 0.0f);
		return color;
	}
	static inline bool IsValidLight(RE::BSLight* a_light);
	static inline bool IsGlobalLight(RE::BSLight* a_light);

	struct Settings
	{
		bool EnableContactShadows = false;
		// Max raymarch steps at zero depth; linearly ramps to 0 at MaxDistance.
		uint ContactShadowMaxSteps = 4;
		// View-space depth at which contact shadows fade fully off.
		float ContactShadowMaxDistance = 1024.0f;
		// Per-step march length in view-space units. Larger -> longer shadows, coarser detail.
		float ContactShadowStride = 2.0f;
		// Depth-delta multiplier for shadow onset (higher -> darker contact).
		float ContactShadowThickness = 0.20f;
		// Depth-delta multiplier for shadow falloff (higher -> shorter shadow).
		float ContactShadowDepthFade = 0.05f;
		// Skip contact shadows for CLUSTERED lights whose normalized distance falloff
		// (1 - (lightDist/radius)^2) at the pixel is below this threshold. Strict
		// lights always raymarch. 0 = never skip; 1 = always skip.
		float ContactShadowMinIntensity = 0.25f;

		/// Show the shadow caster overlay (suppression / debug-override table)
		/// independently of the visualization mode and suppression state.
		/// Without this, the overlay only appeared when a light was suppressed
		/// or visualisation was active — making it hard to access the overlay's
		/// debug controls (cycle button, solo, hover-pulse) in the default state.
		bool ShowShadowOverlay = false;

		// Shadow caster scheduling (ShadowCasterManager)
		ShadowCasterManager::Settings ShadowSettings;

		bool EnableParticleContactShadows = false;

		// Particle Lights.
		bool EnableParticleLights = true;
		bool EnableParticleLightsCulling = true;
		bool EnableParticleLightsDetection = true;
		bool EnableParticleLightsOptimization = true;
		float ParticleLightsSaturation = 1.0f;
		float ParticleBrightness = 1.0f;
		float ParticleRadius = 1.0f;
		float BillboardBrightness = 1.0f;
		float BillboardRadius = 1.0f;
		float ParticleClusterThreshold = 32.0f;
		int MaxParticlesPerEmitter = 256;
		float MaxParticleDistance = 6000.0f;

		// JSON-placed light intensity (requires Inverse Square Lighting runtime metadata).
		float JsonPlacedLightIntensity = 1.0f;
		bool JsonPlacedLightsInteriorsOnly = false;
		bool JsonPlacedLightsPortalStrictOnly = false;
	};

	uint clusterSize[3] = { 16 };

	Settings settings;

	void BSLightingShader_SetupGeometry_Before(RE::BSRenderPass* a_pass);

	void BSLightingShader_SetupGeometry_GeometrySetupConstantPointLights(RE::BSRenderPass* a_pass);

	void BSLightingShader_SetupGeometry_After(RE::BSRenderPass* a_pass);

	eastl::hash_map<RE::NiNode*, uint8_t> roomNodes;

	struct Hooks
	{
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSEffectShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSWaterShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct AIProcess_CalculateLightValue_GetLuminance
		{
			static float thunk(RE::ShadowSceneNode* shadowSceneNode,
				RE::NiPoint3& targetPosition,
				int& numHits,
				float& sunLightLevel,
				float& lightLevel,
				RE::NiLight& refLight,
				int32_t shadowBitMask);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <int N>
		struct ValidLight
		{
			static bool thunk(RE::BSShaderProperty* a_property, RE::BSLight* a_light)
			{
				return func(a_property, a_light) && (a_light->portalStrict || !a_light->portalGraph || a_light->IsShadowLight());
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		using ValidLight1 = ValidLight<1>;
		using ValidLight2 = ValidLight<2>;
		using ValidLight3 = ValidLight<3>;

		static void Install()
		{
			stl::write_thunk_call<AIProcess_CalculateLightValue_GetLuminance>(
				REL::RelocationID(38900, 39946).address() + REL::Relocate(0x1C9, 0x1D3));
			stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
			stl::write_vfunc<0x6, BSEffectShader_SetupGeometry>(RE::VTABLE_BSEffectShader[0]);
			stl::write_vfunc<0x6, BSWaterShader_SetupGeometry>(RE::VTABLE_BSWaterShader[0]);

			stl::write_thunk_call<ValidLight1>(REL::RelocationID(100994, 107781).address() + 0x92);
			stl::write_thunk_call<ValidLight2>(REL::RelocationID(100997, 107784).address() + REL::Relocate(0x139, 0x12A, 0x133));
			stl::write_thunk_call<ValidLight3>(REL::RelocationID(101296, 108283).address() + REL::Relocate(0xB7, 0x7E));

			logger::info("[LLF] Installed hooks");
		}
	};

	virtual bool SupportsVR() override { return true; };
	virtual bool IsCore() const override { return true; }
};

template <>
struct fmt::formatter<LightLimitFix::LightData>
{
	// Presentation format: 'f' - fixed.
	char presentation = 'f';

	// Parses format specifications of the form ['f'].
	constexpr auto parse(format_parse_context& ctx) -> format_parse_context::iterator
	{
		auto it = ctx.begin(), end = ctx.end();
		if (it != end && (*it == 'f'))
			presentation = *it++;

		// Check if reached the end of the range:
		if (it != end && *it != '}')
			throw format_error("invalid format");

		// Return an iterator past the end of the parsed range:
		return it;
	}

	// Formats the point p using the parsed format specification (presentation)
	// stored in this formatter.
	auto format(const LightLimitFix::LightData& l, format_context& ctx) const -> format_context::iterator
	{
		// ctx.out() is an output iterator to write to.
		return fmt::format_to(ctx.out(), "{{address {:x} color {} radius {} posWS {} {}}}",
			reinterpret_cast<uintptr_t>(&l),
			(Vector3)l.color,
			l.radius,
			(Vector3)l.positionWS[0].data, (Vector3)l.positionWS[1].data);
	}
};
