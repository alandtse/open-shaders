#include "SnowDeformation.h"

#include "Features/Skylighting.h"
#include "Features/SnowCover.h"
#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/ActorUtils.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/UI.h"
#include "Utils/VersionedRelocation.h"

#include "RE/B/bhkPickData.h"

#include <bit>

#define I18N_KEY_PREFIX "feature.snow_deformation."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SnowDeformation::Settings,
	Enabled,
	IncludeNPCs,
	IncludeObjects,
	UseSnowCover,
	DisplaceShadows,
	SinkActors,
	BlanketHeight,
	Depth,
	Lifetime,
	TrianglePixels)

namespace
{
	constexpr float kRimHeight = 0.2f;
	constexpr float kRimWidth = 0.7f;
	constexpr float kShoulder = 0.4f;
	constexpr float kChurn = 0.8f;
	constexpr float kRimNoise = 0.6f;
	constexpr float kSnowfallRefill = 0.05f;
	constexpr float kFootReach = 48.0f;
	constexpr float kGroundClearance = 2.0f;
	constexpr float kMaxTessellation = 64.0f;
	constexpr float kDetailDistance = 512.0f;
	constexpr float kMinSpacing = 2.0f;
	constexpr float kAmplitudePixels = 4.0f;
	constexpr float kMinNormalZ = 0.8f;
	constexpr float kCapFloor = 8.0f;
	constexpr float kCapClearance = 4.0f;
	constexpr uint32_t kCapRaysPerFrame = 32;
	constexpr uint64_t kCapRefreshFrames = 1024;
	constexpr float kObjectReach = 576.0f;
	constexpr uint64_t kObjectScanFrames = 12;
	constexpr size_t kMaxObjectRefs = 256;
	constexpr size_t kMaxObjectShapes = 3;
	constexpr float kObjectContact = 48.0f;
	constexpr float kHeatMelt = 0.85f;
	constexpr float kHeatDepthScale = 0.8f;
	constexpr float kTorchRadius = 40.0f;
	constexpr float kTorchScale = 0.45f;
	constexpr float kImpactReach = 2800.0f;
	constexpr float kImpactRadius = 36.0f;
	constexpr float kImpactDepth = 32.0f;
	constexpr float kShoutRange = 1200.0f;
	constexpr float kShoutWidth = 500.0f;
	constexpr float kShoutDepth = 24.0f;
	constexpr float kExplosionRadiusScale = 0.5f;
	constexpr float kExplosionMaxRadius = 256.0f;
	constexpr float kExplosionDepth = 18.0f;
	constexpr float kImpactRimScale = 0.1f;
	constexpr float kImpactSnowMelt = 0.3f;
	constexpr float kImpactMaxDepth = 64.0f;
	constexpr uint8_t kKindPress = 0;
	constexpr uint8_t kKindMelt = 1;
	constexpr uint8_t kKindWade = 2;
	constexpr float kWadeSpread = 1.6f;
	constexpr float kWadeMinDepth = 0.5f;
	constexpr float kWadeReachMargin = 8.0f;
	constexpr float kTrailMinMotion = 0.1f;
	constexpr float kTrailSteer = 0.5f;
	constexpr float kTrailNarrowRate = 8.0f;
	constexpr float kTrailMaxWidth = 48.0f;
	constexpr float kMaxContactMotion = 96.0f;
	constexpr uint8_t kImpactProjectile = 0;
	constexpr uint8_t kImpactShout = 1;
	constexpr uint8_t kImpactExplosion = 2;
	constexpr uint8_t kElementForce = 0;
	constexpr uint8_t kElementFire = 1;
	constexpr uint8_t kElementFrost = 2;
	constexpr uint8_t kElementShock = 3;
	constexpr float kActiveEpsilon = 0.02f;
	constexpr float kMaxDeltaTime = 0.25f;
	constexpr float kMaxActorMotion = 48.0f;
	constexpr uint64_t kMotionForgetFrames = 120;
	constexpr uint64_t kShapeAuditFrames = 60;
	constexpr float kFadeStartTiles = 11.5f;
	constexpr float kFadeEndTiles = 14.8f;
	constexpr float kReposeRate = 0.1f;
	constexpr float kReposeSlope = 1.19175f;
	constexpr float kBoundDepth = 2.0f;
	constexpr float kFootLength = 1.3f;
	constexpr float kFootAspect = 0.5f;
	constexpr float kSlopeProbe = 48.0f;
	constexpr float kBelowGroundLimit = 32.0f;
	constexpr float kProbeSpan = 0.5f;
	constexpr float kSinkResponse = 0.12f;
	constexpr float kSinkHold = 0.6f;
	constexpr float kMinSink = 0.05f;
	constexpr float kMaxSink = 64.0f;
	constexpr float kBlanketFadeStart = 3072.0f;
	constexpr float kBlanketFadeEnd = 3968.0f;
	constexpr float kBlanketSpacing = 32.0f;
	constexpr uint32_t kBlanketLandBudget = 1024;
	constexpr uint64_t kBlanketRetryFrames = 60;
	constexpr int32_t kFlagFresh = 1;
	constexpr int32_t kFlagClear = 2;
	constexpr int32_t kFlagSeed = 4;

	struct alignas(16) UpdateParams
	{
		float4 Control;
		float4 Shape;
		float4 Options;
		float4 EyeOffset;
		float4 Probe;
		int32_t OriginCell[4];
		int32_t BlanketCell[4];
		float4 BlanketShape;
		float4 BlanketFade;
		float4 Coarse;
		float4 Stamps[SnowDeformation::kMaxStamps];
		float4 StampShape[SnowDeformation::kMaxStamps];
		float4 StampMotion[SnowDeformation::kMaxStamps];
		float4 StampSurface[SnowDeformation::kMaxStamps];
		float4 StampBounds[SnowDeformation::kMaxStamps];
	};
	STATIC_ASSERT_ALIGNAS_16(UpdateParams);

	struct alignas(16) DrawParams
	{
		float4 InverseRow0;
		float4 InverseRow1;
		float4 InverseRow2;
		float4 Translation;
		float4 DepthColumn;
		float4 WindowOffset;
		float4 EyePosition;
		float4 FieldParams;
		float4 FadeParams;
		float4 TessParams;
		float4 RangeParams;
		int32_t SlotParams[4];
		float4 BlanketParams;
		float4 WindowOffset1;
		float4 FieldParams1;
		float4 FadeParams1;
		int32_t SlotParams1[4];
		float4 BlanketWindow;
		float4 DetailParams;
	};
	STATIC_ASSERT_ALIGNAS_16(DrawParams);
	static_assert(sizeof(DrawParams) == 19 * 16);

	struct TileEntry
	{
		int32_t packed = 0;
		int32_t tileX = 0;
		int32_t tileY = 0;
		int32_t pad = 0;
	};
	static_assert(sizeof(TileEntry) == 16);

	struct ExpectedElement
	{
		const char* semantic;
		uint32_t index;
		uint32_t reg;
		uint8_t mask;
	};

	constexpr ExpectedElement kLightingLayout[] = {
		{ "SV_POSITION", 0, 0, 0xF },
		{ "TEXCOORD", 0, 1, 0xF },
		{ "TEXCOORD", 1, 2, 0x7 },
		{ "TEXCOORD", 2, 3, 0x7 },
		{ "TEXCOORD", 3, 4, 0x7 },
		{ "TEXCOORD", 6, 5, 0xF },
		{ "TEXCOORD", 7, 6, 0xF },
		{ "POSITION", 1, 7, 0xF },
		{ "POSITION", 2, 8, 0xF },
		{ "COLOR", 0, 9, 0xF },
		{ "COLOR", 1, 10, 0xF },
		{ "TEXCOORD", 12, 11, 0x7 },
	};

	constexpr ExpectedElement kDepthLayout[] = {
		{ "SV_POSITION", 0, 0, 0xF },
	};

	template <class T>
	bool ReadValue(const uint8_t* a_data, size_t a_size, size_t a_offset, T& a_value)
	{
		if (a_offset > a_size || a_size - a_offset < sizeof(T))
			return false;
		std::memcpy(&a_value, a_data + a_offset, sizeof(T));
		return true;
	}

	struct ParsedElement
	{
		std::string name;
		uint32_t index = 0;
		uint32_t reg = 0;
		uint32_t type = 0;
		uint8_t mask = 0;
	};

	bool ParseLayout(const void* a_bytecode, size_t a_size, std::vector<ParsedElement>& a_elements)
	{
		const auto* bytes = static_cast<const uint8_t*>(a_bytecode);
		uint32_t chunkCount = 0;
		if (!bytes || a_size < 32 || std::memcmp(bytes, "DXBC", 4) != 0 || !ReadValue(bytes, a_size, 28, chunkCount))
			return false;

		for (uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
			uint32_t chunkOffset = 0;
			uint32_t chunkSize = 0;
			if (!ReadValue(bytes, a_size, 32 + static_cast<size_t>(chunk) * 4, chunkOffset) || !ReadValue(bytes, a_size, static_cast<size_t>(chunkOffset) + 4, chunkSize))
				return false;
			const auto* tag = bytes + chunkOffset;
			const bool osgn = std::memcmp(tag, "OSGN", 4) == 0;
			const bool osg5 = std::memcmp(tag, "OSG5", 4) == 0;
			const bool osg1 = std::memcmp(tag, "OSG1", 4) == 0;
			if (!osgn && !osg5 && !osg1)
				continue;

			const size_t dataOffset = static_cast<size_t>(chunkOffset) + 8;
			if (dataOffset > a_size || a_size - dataOffset < chunkSize)
				return false;
			const auto* data = bytes + dataOffset;
			uint32_t elementCount = 0;
			if (!ReadValue(data, chunkSize, 0, elementCount) || elementCount > 32)
				return false;

			const size_t stride = osgn ? 24 : (osg5 ? 28 : 32);
			const size_t fieldOffset = osgn ? 0 : 4;
			a_elements.clear();
			for (uint32_t i = 0; i < elementCount; ++i) {
				const size_t element = 8 + static_cast<size_t>(i) * stride + fieldOffset;
				uint32_t nameOffset = 0, semanticIndex = 0, componentType = 0, reg = 0, stream = 0;
				uint8_t mask = 0;
				if (!ReadValue(data, chunkSize, element, nameOffset) || !ReadValue(data, chunkSize, element + 4, semanticIndex) ||
					!ReadValue(data, chunkSize, element + 12, componentType) || !ReadValue(data, chunkSize, element + 16, reg) ||
					!ReadValue(data, chunkSize, element + 20, mask))
					return false;
				if (!osgn && !ReadValue(data, chunkSize, element - 4, stream))
					return false;
				if (nameOffset >= chunkSize || stream != 0)
					return false;
				const auto* name = reinterpret_cast<const char*>(data + nameOffset);
				const size_t nameLength = strnlen(name, chunkSize - nameOffset);
				if (nameLength == chunkSize - nameOffset)
					return false;
				a_elements.push_back({ std::string(name, nameLength), semanticIndex, reg, componentType, static_cast<uint8_t>(mask & 0xF) });
			}
			return true;
		}
		return false;
	}

	bool MatchesLayout(const void* a_bytecode, size_t a_size, std::span<const ExpectedElement> a_layout)
	{
		std::vector<ParsedElement> elements;
		if (!ParseLayout(a_bytecode, a_size, elements) || elements.size() != a_layout.size())
			return false;
		for (size_t i = 0; i < elements.size(); ++i) {
			const auto& parsed = elements[i];
			const auto& expected = a_layout[i];
			if (_stricmp(parsed.name.c_str(), expected.semantic) != 0 || parsed.index != expected.index || parsed.reg != expected.reg || parsed.mask != expected.mask || parsed.type != 3)
				return false;
		}
		return true;
	}

	bool GenerateDecalLayout(const std::vector<ParsedElement>& a_elements, std::string& a_layout, std::string& a_fields, std::string& a_world)
	{
		bool position = false;
		for (size_t i = 0; i < a_elements.size(); ++i) {
			const auto& element = a_elements[i];
			const int components = std::popcount(static_cast<unsigned>(element.mask));
			if (components < 1)
				return false;
			const bool clip = _stricmp(element.name.c_str(), "SV_POSITION") == 0 && element.index == 0;
			const char* type = element.type == 3 ? "float" : (element.type == 1 ? "uint" : "int");
			const std::string field = clip ? std::string("Position") : std::format("f{}", i);
			a_layout += std::format("{}{} {} : {}{};", type, components > 1 ? std::to_string(components) : std::string(), field, element.name, element.index);
			if (clip) {
				position = components == 4 && element.type == 3;
				continue;
			}
			a_fields += std::format("{}({});", element.type == 3 ? "SD_INTERPOLATE" : "SD_COPY", field);
			if (element.type == 3 && components >= 3 && _stricmp(element.name.c_str(), "POSITION") == 0 && (element.index == 1 || element.index == 2))
				a_world += std::format("output.{}.z += displacement;", field);
		}
		return position;
	}

	bool IsLandscapeGeometry(RE::BSGeometry* a_geometry)
	{
		if (!a_geometry || a_geometry->GetGeometryRuntimeData().skinInstance)
			return false;
		auto property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
		auto material = property ? property->GetBaseMaterial() : nullptr;
		if (!material)
			return false;
		const auto feature = material->GetFeature();
		return feature == RE::BSShaderMaterial::Feature::kMultiTexLand || feature == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend;
	}

	bool IsVanillaSnow(RE::MATERIAL_ID a_material)
	{
		return a_material == RE::MATERIAL_ID::kSnow || a_material == RE::MATERIAL_ID::kSnowStairs;
	}

	float LandNormalZ(RE::TES* a_tes, const RE::NiPoint3& a_position, float a_landZ)
	{
		float heightX0 = a_landZ, heightX1 = a_landZ, heightY0 = a_landZ, heightY1 = a_landZ;
		a_tes->GetLandHeight(RE::NiPoint3(a_position.x - kSlopeProbe, a_position.y, a_position.z), heightX0);
		a_tes->GetLandHeight(RE::NiPoint3(a_position.x + kSlopeProbe, a_position.y, a_position.z), heightX1);
		a_tes->GetLandHeight(RE::NiPoint3(a_position.x, a_position.y - kSlopeProbe, a_position.z), heightY0);
		a_tes->GetLandHeight(RE::NiPoint3(a_position.x, a_position.y + kSlopeProbe, a_position.z), heightY1);
		const float slopeX = (heightX1 - heightX0) / (2.0f * kSlopeProbe);
		const float slopeY = (heightY1 - heightY0) / (2.0f * kSlopeProbe);
		return 1.0f / std::sqrt(1.0f + slopeX * slopeX + slopeY * slopeY);
	}

	bool IsActorLayer(RE::COL_LAYER a_layer)
	{
		return a_layer == RE::COL_LAYER::kCharController || a_layer == RE::COL_LAYER::kBiped || a_layer == RE::COL_LAYER::kBipedNoCC || a_layer == RE::COL_LAYER::kDeadBip;
	}

	float MeshCap(RE::TES* a_tes, float a_x, float a_y, float a_landZ, float a_height)
	{
		const float scale = RE::bhkWorld::GetWorldScale();
		RE::bhkPickData pick{};
		pick.rayInput.from = RE::hkVector4(a_x * scale, a_y * scale, (a_landZ + a_height + kCapFloor) * scale, 0.0f);
		pick.rayInput.to = RE::hkVector4(a_x * scale, a_y * scale, (a_landZ + kCapFloor) * scale, 0.0f);
		pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
		a_tes->Pick(pick);
		if (!pick.rayOutput.HasHit())
			return 1.0f;
		const auto layer = pick.rayOutput.rootCollidable->GetCollisionLayer();
		if (IsActorLayer(layer) || layer == RE::COL_LAYER::kTerrain || layer == RE::COL_LAYER::kGround)
			return 1.0f;
		const float above = a_height + kCapFloor - a_height * std::clamp(pick.rayOutput.hitFraction, 0.0f, 1.0f);
		return std::clamp((above - kCapFloor - kCapClearance) / a_height, 0.0f, 1.0f);
	}

	bool ClassifyElement(RE::MagicItem* a_spell, uint8_t& a_element)
	{
		if (!a_spell)
			return false;
		bool physical = false;
		for (const auto effect : a_spell->effects) {
			const auto base = effect ? effect->baseEffect : nullptr;
			if (!base || !base->IsHostile())
				continue;
			const auto& data = base->data;
			if (data.resistVariable == RE::ActorValue::kResistFire) {
				a_element = kElementFire;
				return true;
			}
			if (data.resistVariable == RE::ActorValue::kResistFrost) {
				a_element = kElementFrost;
				return true;
			}
			if (data.resistVariable == RE::ActorValue::kResistShock) {
				a_element = kElementShock;
				return true;
			}
			physical = physical || data.archetype == RE::EffectArchetypes::ArchetypeID::kStagger || data.archetype == RE::EffectArchetypes::ArchetypeID::kConcussion;
		}
		a_element = kElementForce;
		return physical;
	}

	bool GroundShout(RE::MagicItem* a_spell)
	{
		if (!a_spell || a_spell->GetSpellType() != RE::MagicSystem::SpellType::kVoicePower)
			return false;
		for (const auto effect : a_spell->effects) {
			const auto base = effect ? effect->baseEffect : nullptr;
			if (!base)
				continue;
			const auto projectile = base->data.projectileBase;
			if ((projectile && (projectile->IsCone() || projectile->IsFlamethrower())) || base->data.archetype == RE::EffectArchetypes::ArchetypeID::kStagger ||
				base->data.archetype == RE::EffectArchetypes::ArchetypeID::kConcussion)
				return true;
		}
		return false;
	}

	RE::FormID SpaceOf(RE::TESObjectREFR* a_ref)
	{
		auto cell = a_ref ? a_ref->GetParentCell() : nullptr;
		if (!cell)
			return 0;
		auto world = cell->GetRuntimeData().worldSpace;
		return world ? world->GetFormID() : cell->GetFormID();
	}

	bool IsFinite(const RE::NiPoint3& a_point)
	{
		return std::isfinite(a_point.x) && std::isfinite(a_point.y) && std::isfinite(a_point.z);
	}

	struct ShapeSegment
	{
		RE::NiPoint3 low;
		RE::NiPoint3 high;
		float radius = 0.0f;
	};

	bool GetShapeSegment(RE::bhkNiCollisionObject* a_object, const RE::NiPoint3& a_centre, ShapeSegment& a_segment)
	{
		auto rigid = a_object && a_object->body ? a_object->body->AsBhkRigidBody() : nullptr;
		auto body = rigid ? skyrim_cast<RE::hkpRigidBody*>(rigid->referencedObject.get()) : nullptr;
		auto shape = body ? body->collidable.GetShape() : nullptr;
		if (!shape)
			return false;

		const auto& transform = body->motion.motionState.transform;
		float axes[3][4];
		float origin[4];
		_mm_storeu_ps(axes[0], transform.rotation.col0.quad);
		_mm_storeu_ps(axes[1], transform.rotation.col1.quad);
		_mm_storeu_ps(axes[2], transform.rotation.col2.quad);
		_mm_storeu_ps(origin, transform.translation.quad);
		const float scale = RE::bhkWorld::GetWorldScaleInverse();
		auto toWorld = [&](const RE::hkVector4& a_local) {
			float local[4];
			_mm_storeu_ps(local, a_local.quad);
			return RE::NiPoint3(
				(origin[0] + axes[0][0] * local[0] + axes[1][0] * local[1] + axes[2][0] * local[2]) * scale,
				(origin[1] + axes[0][1] * local[0] + axes[1][1] * local[1] + axes[2][1] * local[2]) * scale,
				(origin[2] + axes[0][2] * local[0] + axes[1][2] * local[1] + axes[2][2] * local[2]) * scale);
		};

		if (shape->type == RE::hkpShapeType::kCapsule) {
			const auto capsule = static_cast<const RE::hkpCapsuleShape*>(shape);
			a_segment.low = toWorld(capsule->vertexA);
			a_segment.high = toWorld(capsule->vertexB);
			if (a_segment.low.z > a_segment.high.z)
				std::swap(a_segment.low, a_segment.high);
			a_segment.radius = capsule->radius * scale;
		} else {
			auto extent = [&](uint32_t a_axis) {
				const RE::hkVector4 positive{ axes[0][a_axis], axes[1][a_axis], axes[2][a_axis], 0.0f };
				const RE::hkVector4 negative{ -axes[0][a_axis], -axes[1][a_axis], -axes[2][a_axis], 0.0f };
				return 0.5f * (shape->GetMaximumProjection(positive) + shape->GetMaximumProjection(negative)) * scale;
			};
			const float reach = shape->GetMaximumProjection(RE::hkVector4{ -axes[0][2], -axes[1][2], -axes[2][2], 0.0f }) * scale;
			a_segment.radius = std::max(std::sqrt(std::max(extent(0) * extent(1), 0.0f)), 1.0f);
			a_segment.low = RE::NiPoint3(a_centre.x, a_centre.y, origin[2] * scale - reach + a_segment.radius);
			a_segment.high = a_segment.low;
		}
		return IsFinite(a_segment.low) && IsFinite(a_segment.high) && a_segment.radius > 0.0f && a_segment.radius < 128.0f;
	}

	bool ClearPath(RE::TES* a_tes, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
	{
		const float scale = RE::bhkWorld::GetWorldScale();
		RE::bhkPickData pick{};
		pick.rayInput.from = RE::hkVector4(a_from.x * scale, a_from.y * scale, a_from.z * scale, 0.0f);
		pick.rayInput.to = RE::hkVector4(a_to.x * scale, a_to.y * scale, a_to.z * scale, 0.0f);
		pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
		a_tes->Pick(pick);
		return !pick.rayOutput.HasHit() || pick.rayOutput.hitFraction >= 0.98f;
	}

	struct Stroke
	{
		float x = 0.0f;
		float y = 0.0f;
		float motionX = 0.0f;
		float motionY = 0.0f;
		float radius = 0.0f;
		float strength = 0.0f;
	};

	struct Pattern
	{
		std::array<Stroke, 9> strokes{};
		size_t count = 0;
	};

	Pattern ExplosionPattern(float a_reach)
	{
		Pattern out{};
		if (!(a_reach > 0.0f) || !std::isfinite(a_reach))
			return out;
		out.strokes[out.count++] = { 0.0f, 0.0f, 0.0f, 0.0f, a_reach * 0.48f, 1.0f };
		for (int i = 0; i < 6; ++i) {
			const float angle = static_cast<float>(i) * 1.04719755f + 0.17f;
			const float x = std::cos(angle);
			const float y = std::sin(angle);
			out.strokes[out.count++] = { x * a_reach * 0.76f, y * a_reach * 0.76f, x * a_reach * 0.42f, y * a_reach * 0.42f, a_reach * 0.18f, (i % 2) ? 0.36f : 0.48f };
		}
		return out;
	}

	Pattern DirectionalPattern(float a_reach, float a_width, float a_forwardX, float a_forwardY, bool a_shout)
	{
		Pattern out{};
		const float length = std::hypot(a_forwardX, a_forwardY);
		if (!(a_reach > 0.0f) || !(a_width > 0.0f) || !std::isfinite(a_reach) || !std::isfinite(a_width) || !(length > 0.001f) || !std::isfinite(length))
			return out;
		const float fx = a_forwardX / length;
		const float fy = a_forwardY / length;
		const int lanes = a_shout ? 3 : 1;
		for (int lane = 0; lane < lanes; ++lane) {
			const float side = a_shout ? static_cast<float>(lane - 1) : 0.0f;
			for (int step = 0; step < 3; ++step) {
				const float t0 = static_cast<float>(step) / 3.0f;
				const float t1 = static_cast<float>(step + 1) / 3.0f;
				const float lateral0 = side * a_width * t0 * 0.52f;
				const float lateral1 = side * a_width * t1 * 0.52f;
				const float x0 = fx * a_reach * t0 - fy * lateral0;
				const float y0 = fy * a_reach * t0 + fx * lateral0;
				const float x1 = fx * a_reach * t1 - fy * lateral1;
				const float y1 = fy * a_reach * t1 + fx * lateral1;
				out.strokes[out.count++] = { x1, y1, x1 - x0, y1 - y0, a_width * (a_shout ? (0.14f + 0.13f * t1) : 0.5f), (1.0f - 0.5f * t1) * (side == 0.0f ? 1.0f : 0.65f) };
			}
		}
		return out;
	}

	bool LeavesAMark(RE::TESObjectREFR* a_ref)
	{
		if (auto projectile = a_ref->As<RE::Projectile>()) {
			const auto base = projectile->GetProjectileBase();
			const char* model = base ? base->GetModel() : nullptr;
			if (!model)
				return true;
			std::string_view name(model);
			if (const auto slash = name.find_last_of("/\\"); slash != std::string_view::npos)
				name.remove_prefix(slash + 1);
			return _strnicmp(name.data(), "bloodsprayprojectile", 20) != 0;
		}
		const auto base = a_ref->GetBaseObject();
		if (!base)
			return false;
		switch (base->GetFormType()) {
		case RE::FormType::Weapon:
		case RE::FormType::Armor:
		case RE::FormType::Ammo:
		case RE::FormType::Misc:
		case RE::FormType::Ingredient:
		case RE::FormType::AlchemyItem:
		case RE::FormType::Book:
		case RE::FormType::Scroll:
		case RE::FormType::SoulGem:
		case RE::FormType::KeyMaster:
		case RE::FormType::Apparatus:
		case RE::FormType::Light:
			return true;
		default:
			return false;
		}
	}

	bool IsReflectionPass()
	{
		return (globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)) != 0;
	}

	bool IsCameraDepthTechnique(uint32_t a_technique)
	{
		using enum RE::BSUtilityShader::Flags;
		constexpr uint32_t rejected = static_cast<uint32_t>(AlphaTest) | static_cast<uint32_t>(LodLandscape) | static_cast<uint32_t>(RenderNormal) |
		                              static_cast<uint32_t>(RenderNormalFalloff) | static_cast<uint32_t>(RenderNormalClamp) | static_cast<uint32_t>(RenderNormalClear) |
		                              static_cast<uint32_t>(RenderShadowmap) | static_cast<uint32_t>(RenderShadowmapClamped) | static_cast<uint32_t>(RenderShadowmapPb) |
		                              static_cast<uint32_t>(DepthWriteDecals) | static_cast<uint32_t>(DebugShadowSplit) | static_cast<uint32_t>(DebugColor) |
		                              static_cast<uint32_t>(RenderShadowmask) | static_cast<uint32_t>(RenderShadowmaskSpot) | static_cast<uint32_t>(RenderShadowmaskPb) |
		                              static_cast<uint32_t>(RenderShadowmaskDpb) | static_cast<uint32_t>(RenderBaseTexture) | static_cast<uint32_t>(TreeAnim) |
		                              static_cast<uint32_t>(LodObject) | static_cast<uint32_t>(LocalMapFogOfWar);
		return (a_technique & static_cast<uint32_t>(RenderDepth)) != 0 && (a_technique & rejected) == 0;
	}

	bool IsShadowTechnique(uint32_t a_technique)
	{
		using enum RE::BSUtilityShader::Flags;
		constexpr uint32_t rejected = static_cast<uint32_t>(AlphaTest) | static_cast<uint32_t>(LodLandscape) | static_cast<uint32_t>(RenderShadowmapPb) |
		                              static_cast<uint32_t>(DebugShadowSplit) | static_cast<uint32_t>(DebugColor) | static_cast<uint32_t>(RenderShadowmask) |
		                              static_cast<uint32_t>(RenderShadowmaskSpot) | static_cast<uint32_t>(RenderShadowmaskPb) | static_cast<uint32_t>(RenderShadowmaskDpb) |
		                              static_cast<uint32_t>(RenderBaseTexture) | static_cast<uint32_t>(TreeAnim) | static_cast<uint32_t>(LodObject) |
		                              static_cast<uint32_t>(LocalMapFogOfWar);
		return (a_technique & static_cast<uint32_t>(RenderShadowmap)) != 0 && (a_technique & rejected) == 0;
	}

	bool InvertRows(const std::array<std::array<double, 4>, 3>& a_rows, std::array<std::array<double, 3>, 3>& a_inverse)
	{
		const auto& m = a_rows;
		const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
		const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
		const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
		const double determinant = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
		if (!std::isfinite(determinant) || std::abs(determinant) < 1e-30)
			return false;
		const double inverse = 1.0 / determinant;
		a_inverse[0] = { c00 * inverse, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inverse, (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inverse };
		a_inverse[1] = { c01 * inverse, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inverse, (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inverse };
		a_inverse[2] = { c02 * inverse, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inverse, (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inverse };
		return true;
	}

	int32_t FloorDiv(double a_value, double a_size)
	{
		return static_cast<int32_t>(std::floor(a_value / a_size));
	}

	RE::NiAVObject* FindSinkNode(RE::NiAVObject* a_root)
	{
		auto rootNode = a_root ? a_root->AsNode() : nullptr;
		if (!rootNode)
			return nullptr;
		static const RE::BSFixedString skeletonHolder{ "NPC" };
		static const RE::BSFixedString skeletonRoot{ "NPC Root [Root]" };
		for (const auto& child : rootNode->GetChildren()) {
			if (child && child->AsNode() && child->name == skeletonHolder)
				return child.get();
		}
		auto skeleton = rootNode->GetObjectByName(skeletonRoot);
		auto holder = skeleton ? skeleton->parent : nullptr;
		return holder && holder != rootNode && holder->parent == rootNode ? holder : nullptr;
	}
}

void SnowDeformation::SetupResources()
{
	auto device = globals::d3d::device;

	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kTexels;
		desc.Height = kTexels;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

		D3D11_TEXTURE2D_DESC activityDesc = desc;
		activityDesc.Width = kSlots;
		activityDesc.Height = kSlots;
		activityDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		for (uint32_t i = 0; i < kLevels; ++i) {
			auto& level = levels[i];
			level.cellSize = kCellSize * (i == 0 ? 1.0f : kCoarseScale);
			level.tileSize = level.cellSize * static_cast<float>(kTileTexels);
			level.fadeStart = kFadeStartTiles * level.tileSize;
			level.fadeEnd = kFadeEndTiles * level.tileSize;
			level.texture = new Texture2D(desc, i == 0 ? "SnowDeformation::Field" : "SnowDeformation::FieldCoarse");
			level.texture->CreateSRV(srvDesc);
			level.texture->CreateUAV(uavDesc);
			level.activityTexture = new Texture2D(activityDesc, i == 0 ? "SnowDeformation::Activity" : "SnowDeformation::ActivityCoarse");
			level.activityTexture->CreateSRV(srvDesc);
		}

		beforeTexture = new Texture2D(desc, "SnowDeformation::FieldBefore");
		beforeTexture->CreateSRV(srvDesc);
		beforeTexture->CreateUAV(uavDesc);
	}

	tileList = eastl::make_unique<StructuredBuffer>(StructuredBufferDesc<TileEntry>(kMaxTileEntries), kMaxTileEntries, "SnowDeformation::TileList");
	tileList->CreateSRV();

	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kBlanketTexels;
		desc.Height = kBlanketTexels;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		blanketLandTexture = new Texture2D(desc, "SnowDeformation::BlanketLand");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		blanketLandTexture->CreateSRV(srvDesc);

		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		blanketTexture = new Texture2D(desc, "SnowDeformation::Blanket");
		srvDesc.Format = desc.Format;
		blanketTexture->CreateSRV(srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		blanketTexture->CreateUAV(uavDesc);

		const float zeros[4] = {};
		globals::d3d::context->ClearUnorderedAccessViewFloat(blanketTexture->uav.get(), zeros);

		constexpr size_t count = static_cast<size_t>(kBlanketTexels) * kBlanketTexels;
		blanketLand.assign(count, { 0.0f, -1.0f, 1.0f, 0.0f });
		blanketOwner.assign(count, { std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::min() });
		blanketRetry.assign(count, 0);
		blanketCapFrame.assign(count, 0);
	}

	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = sizeof(float) * kMaxStamps;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		probeBuffer = eastl::make_unique<Buffer>(desc, nullptr, "SnowDeformation::ProbeDepths");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = kMaxStamps;
		probeBuffer->CreateUAV(uavDesc);

		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (auto& staging : probeStaging) {
			DX::ThrowIfFailed(device->CreateBuffer(&desc, nullptr, staging.put()));
			Util::SetResourceName(staging.get(), "SnowDeformation::ProbeReadback");
		}
	}

	updateConstants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<UpdateParams>(), "SnowDeformation::UpdateParams");
	drawConstants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<DrawParams>(), "SnowDeformation::DrawParams");

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, fieldSampler.put()));
	Util::SetResourceName(fieldSampler.get(), "SnowDeformation::FieldSampler");

	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearSampler.put()));
	Util::SetResourceName(linearSampler.get(), "SnowDeformation::LinearSampler");

	ClearField();
}

void SnowDeformation::ClearShaderCache()
{
	EndLandscapeDraw();
	updateCS = nullptr;
	copyCS = nullptr;
	probeCS = nullptr;
	blanketCS = nullptr;
	lightingHS = nullptr;
	lightingDS = nullptr;
	depthHS = nullptr;
	depthDS = nullptr;
	shaderCompileFailed = false;
	lightingLayouts.clear();
	depthLayouts.clear();
	decalShaders.clear();
	lightingRejected.clear();
	lightingLayoutSeen = false;
}

bool SnowDeformation::EnsureShaders()
{
	if (shaderCompileFailed)
		return false;
	if (updateCS && copyCS && lightingHS && lightingDS && depthHS && depthDS)
		return true;

	const std::vector<std::pair<const char*, const char*>> lightingDefines;
	const std::vector<std::pair<const char*, const char*>> depthDefines{ { "DEPTH_ONLY", "" } };
	std::vector<std::pair<const char*, const char*>> updateDefines;
	updateSkylighting = globals::features::skylighting.loaded;
	if (updateSkylighting)
		updateDefines.push_back({ "SKYLIGHTING", "" });

	if (!updateCS)
		updateCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowDeformationUpdateCS.hlsl", updateDefines, "cs_5_0")));
	if (!copyCS)
		copyCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowDeformationCopyCS.hlsl", {}, "cs_5_0")));
	if (!lightingHS)
		lightingHS.attach(static_cast<ID3D11HullShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowDeformationHS.hlsl", lightingDefines, "hs_5_0")));
	if (!lightingDS)
		lightingDS.attach(static_cast<ID3D11DomainShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowDeformationDS.hlsl", lightingDefines, "ds_5_0")));
	if (!depthHS)
		depthHS.attach(static_cast<ID3D11HullShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowDeformationHS.hlsl", depthDefines, "hs_5_0")));
	if (!depthDS)
		depthDS.attach(static_cast<ID3D11DomainShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowDeformationDS.hlsl", depthDefines, "ds_5_0")));
	if (!probeCS)
		probeCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowDeformationProbeCS.hlsl", {}, "cs_5_0")));
	if (!blanketCS)
		blanketCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowDeformationBlanketCS.hlsl", updateDefines, "cs_5_0")));

	shaderCompileFailed = !(updateCS && copyCS && lightingHS && lightingDS && depthHS && depthDS);
	if (shaderCompileFailed)
		logger::error("[SnowDeformation] Shader compilation failed, prints are disabled until the shader cache is cleared");
	return !shaderCompileFailed;
}

bool SnowDeformation::IsActiveContext() const
{
	if (!loaded || !settings.Enabled || REL::Module::IsVR() || !globals::shaderCache->IsEnabled())
		return false;
	auto player = globals::game::player;
	if (!player || !player->Is3DLoaded() || Util::IsInterior())
		return false;
	auto cell = player->GetParentCell();
	return cell && !cell->IsInteriorCell() && player->GetWorldspace();
}

bool SnowDeformation::IsSnowCoverActive() const
{
	const auto& snowCover = globals::features::snowCover;
	return settings.UseSnowCover && snowCover.loaded && snowCover.wsettings.EnableSnowCover && snowCover.views[6];
}

void SnowDeformation::ClearLevel(Level& a_level)
{
	if (a_level.texture) {
		const float zeros[4] = {};
		globals::d3d::context->ClearUnorderedAccessViewFloat(a_level.texture->uav.get(), zeros);
	}
	a_level.slots = {};
	a_level.windowValid = false;
	a_level.prevValid = false;
	a_level.anyActive = false;
	a_level.maxBound = 0.0f;
	a_level.activityDirty = true;
	++fieldSerial;
}

void SnowDeformation::ClearField()
{
	for (auto& level : levels)
		ClearLevel(level);
	windowValid = false;
	snapshotValid = false;
	lightingRejected.clear();
	for (auto& frame : probeFrames)
		frame.pending = false;
}

void SnowDeformation::TrackShapeMotion(const void* a_key, const RE::NiPoint3& a_position, bool a_contact, float& a_motionX, float& a_motionY)
{
	a_motionX = 0.0f;
	a_motionY = 0.0f;
	auto& tracked = shapeMotion[a_key];
	if (tracked.frame + 1 == gatherFrame) {
		const float dx = a_position.x - tracked.x;
		const float dy = a_position.y - tracked.y;
		const float limit = a_contact && tracked.contact ? kMaxContactMotion : kMaxActorMotion;
		if (dx * dx + dy * dy <= limit * limit) {
			a_motionX = dx;
			a_motionY = dy;
		}
	}
	tracked = { a_position.x, a_position.y, gatherFrame, a_contact };
}

void SnowDeformation::AppendActorStamps(RE::Actor* a_actor, const RE::NiPoint3& a_anchor, std::vector<Stamp>& a_stamps)
{
	if (!a_actor || a_stamps.size() >= kMaxStamps || a_actor->IsOnMount())
		return;

	auto root = a_actor->Get3D(false);
	auto tes = globals::game::tes;
	if (!root || !tes)
		return;

	const RE::NiPoint3 position = a_actor->GetPosition();
	const float reach = static_cast<float>(kWindowTiles) * kTileSize;
	if (std::abs(position.x - a_anchor.x) > reach || std::abs(position.y - a_anchor.y) > reach)
		return;

	float landZ = 0.0f;
	if (!tes->GetLandHeight(position, landZ))
		return;
	const float normalZ = LandNormalZ(tes, position, landZ);

	const float heading = a_actor->GetAngleZ();
	const float forwardX = std::sin(heading);
	const float forwardY = std::cos(heading);

	bool oriented = false;
	if (auto race = a_actor->GetRace()) {
		auto cached = bootedRaces.find(race->GetFormID());
		if (cached == bootedRaces.end())
			cached = bootedRaces.emplace(race->GetFormID(), race->HasKeywordString("ActorTypeNPC")).first;
		oriented = cached->second;
	}

	const float depth = std::clamp(settings.Depth, 1.0f, 20.0f);
	const float rim = depth * kRimHeight;

	if (auto equipped = a_actor->GetEquippedObject(true); equipped && equipped->GetFormType() == RE::FormType::Light && !a_actor->IsDead()) {
		Stamp torch{};
		torch.x = position.x;
		torch.y = position.y;
		torch.radius = kTorchRadius;
		torch.depth = kHeatMelt * kTorchScale;
		torch.rim = -std::clamp(settings.BlanketHeight, 0.0f, kMaxBlanketHeight) * kHeatDepthScale;
		torch.vanillaSnow = IsVanillaSnow(tes->GetLandMaterialType(position)) ? 1.0f : 0.0f;
		torch.landZ = landZ;
		torch.normalZ = normalZ;
		torch.kind = kKindMelt;
		a_stamps.push_back(torch);
		if (a_stamps.size() >= kMaxStamps)
			return;
	}

	const size_t firstStamp = a_stamps.size();
	const RE::FormID owner = a_actor->GetFormID();
	float applied = 0.0f;
	if (const auto sunk = actorSinks.find(owner); sunk != actorSinks.end() && sunk->second.applied)
		applied = sunk->second.sink;

	auto makeStamp = [&](const RE::NiPoint3& a_centre, float a_radius, float a_landZ, float a_motionX, float a_motionY) {
		Stamp stamp{};
		stamp.x = a_centre.x;
		stamp.y = a_centre.y;
		stamp.radius = a_radius;
		stamp.depth = depth;
		stamp.rim = rim;
		if (oriented) {
			stamp.forwardX = forwardX;
			stamp.forwardY = forwardY;
			stamp.radius *= kFootLength;
			stamp.halfWidth = stamp.radius * kFootAspect;
		}
		stamp.motionX = a_motionX;
		stamp.motionY = a_motionY;
		stamp.vanillaSnow = IsVanillaSnow(tes->GetLandMaterialType(a_centre)) ? 1.0f : 0.0f;
		stamp.landZ = a_landZ;
		stamp.normalZ = normalZ;
		stamp.owner = owner;
		a_stamps.push_back(stamp);
	};

	const float wadeHeight = std::clamp(settings.BlanketHeight, 0.0f, kMaxBlanketHeight);
	const float rootClearance = position.z - landZ;
	const bool trail = wadeHeight > 0.0f && rootClearance >= -kBelowGroundLimit && rootClearance < wadeHeight - kWadeMinDepth && !a_actor->IsDead() && !a_actor->IsInRagdollState();
	const void* trailKey = static_cast<const std::byte*>(static_cast<const void*>(a_actor)) + 1;
	float trailHeld = 0.0f;
	float trailX = 0.0f;
	float trailY = 0.0f;
	if (trail) {
		if (const auto held = shapeMotion.find(trailKey); held != shapeMotion.end() && held->second.frame + 1 == gatherFrame)
			trailHeld = held->second.width;
		TrackShapeMotion(trailKey, position, true, trailX, trailY);
	}
	const float trailLength = std::sqrt(trailX * trailX + trailY * trailY);
	const bool steered = trailLength > kTrailSteer;
	const float acrossX = steered ? trailY / trailLength : forwardY;
	const float acrossY = steered ? -trailX / trailLength : -forwardX;
	float trailWidth = 0.0f;
	auto widen = [&](const RE::NiPoint3& a_point, float a_radius) {
		const float side = std::abs((a_point.x - position.x) * acrossX + (a_point.y - position.y) * acrossY);
		trailWidth = std::max(trailWidth, side + a_radius * kWadeSpread);
	};
	auto boundRadius = [&]() {
		return std::clamp(a_actor->GetBoundMax().x - a_actor->GetBoundMin().x, 8.0f, 96.0f) * 0.25f;
	};

	auto makeWade = [&](const ShapeSegment& a_segment, float a_landZ, const void* a_key) {
		const float top = a_landZ + wadeHeight;
		const float lowBottom = a_segment.low.z - a_segment.radius;
		if (lowBottom > top - kWadeMinDepth)
			return;

		RE::NiPoint3 end = a_segment.high;
		if (a_segment.high.z - a_segment.radius > top) {
			const float t = (top - lowBottom) / std::max(a_segment.high.z - a_segment.low.z, 1e-3f);
			end = a_segment.low + (a_segment.high - a_segment.low) * std::clamp(t, 0.0f, 1.0f);
		}
		widen(a_segment.low, a_segment.radius);
		widen(end, a_segment.radius);

		const float width = a_segment.radius * kWadeSpread;
		const float spanX = end.x - a_segment.low.x;
		const float spanY = end.y - a_segment.low.y;
		const float span = std::sqrt(spanX * spanX + spanY * spanY);
		const RE::NiPoint3 centre(a_segment.low.x + spanX * 0.5f, a_segment.low.y + spanY * 0.5f, a_segment.low.z);

		float motionX = 0.0f;
		float motionY = 0.0f;
		TrackShapeMotion(a_key, centre, true, motionX, motionY);

		Stamp stamp{};
		stamp.x = centre.x;
		stamp.y = centre.y;
		stamp.radius = span * 0.5f + width;
		if (span > 1e-3f) {
			stamp.forwardX = spanX / span;
			stamp.forwardY = spanY / span;
			stamp.halfWidth = width;
		}
		stamp.clearance = std::max(lowBottom - a_landZ, 0.0f);
		stamp.rim = rim * std::clamp(1.0f - stamp.clearance / wadeHeight, 0.0f, 1.0f);
		stamp.motionX = motionX;
		stamp.motionY = motionY;
		stamp.vanillaSnow = IsVanillaSnow(tes->GetLandMaterialType(centre)) ? 1.0f : 0.0f;
		stamp.landZ = a_landZ;
		stamp.normalZ = normalZ;
		stamp.owner = owner;
		stamp.kind = kKindWade;
		a_stamps.push_back(stamp);
	};

	contactScratch.clear();
	double bodyOffset = 0.0;
	uint32_t bodyCount = 0;
	for (const auto& shape : ActorCollisions(a_actor, root)) {
		auto a_object = shape.object.get();
		RE::NiPoint3 centre;
		float radius = 0.0f;
		if (!Util::GetShapeBound(a_object, centre, radius) || !(radius > 0.0f) || radius > 128.0f)
			continue;
		contactScratch.push_back({ a_object, centre, radius, a_object });

		if (applied > 0.0f) {
			if (auto body = a_object->body ? a_object->body->AsBhkRigidBody() : nullptr) {
				RE::hkVector4 origin;
				body->GetPosition(origin);
				float originValues[4];
				_mm_storeu_ps(originValues, origin.quad);
				bodyOffset += originValues[2] * RE::bhkWorld::GetWorldScaleInverse() - shape.owner->world.translate.z;
				++bodyCount;
			}
		}
	}

	const float lift = bodyCount > 0 && std::abs(bodyOffset / bodyCount) < 0.5 * applied ? applied : 0.0f;
	const float shapeReach = std::max(kFootReach, wadeHeight + kWadeReachMargin);
	const size_t reserve = trail ? 1 : 0;
	for (const auto& contact : contactScratch) {
		if (a_stamps.size() + reserve >= kMaxStamps || a_stamps.size() - firstStamp + reserve >= kMaxStampsPerActor)
			break;

		const float bottom = contact.centre.z + lift - contact.radius;
		if (bottom > position.z + shapeReach)
			continue;

		float shapeLandZ = 0.0f;
		const bool landed = tes->GetLandHeight(contact.centre, shapeLandZ) && bottom >= shapeLandZ - kBelowGroundLimit;
		const bool pressed = landed && bottom - shapeLandZ <= kGroundClearance;

		float motionX = 0.0f;
		float motionY = 0.0f;
		TrackShapeMotion(contact.key, contact.centre, pressed, motionX, motionY);
		const void* wadeKey = static_cast<const std::byte*>(contact.key) + 1;

		if (pressed) {
			makeStamp(contact.centre, contact.radius, shapeLandZ, motionX, motionY);
			if (wadeHeight > 0.0f) {
				float wadeX = 0.0f;
				float wadeY = 0.0f;
				TrackShapeMotion(wadeKey, contact.centre, true, wadeX, wadeY);
				widen(contact.centre, contact.radius);
			}
			continue;
		}

		ShapeSegment segment;
		if (landed && wadeHeight > 0.0f && GetShapeSegment(contact.object, contact.centre, segment)) {
			segment.low.z += lift;
			segment.high.z += lift;
			makeWade(segment, shapeLandZ, wadeKey);
		}
	}

	if (contactScratch.empty() && a_stamps.size() + reserve < kMaxStamps) {
		const bool pressed = position.z - landZ <= kGroundClearance && position.z - landZ >= -kBelowGroundLimit;
		float motionX = 0.0f;
		float motionY = 0.0f;
		TrackShapeMotion(a_actor, position, pressed, motionX, motionY);
		if (pressed)
			makeStamp(position, boundRadius(), landZ, motionX, motionY);
	}

	if (trail && a_stamps.size() < kMaxStamps) {
		const float deltaTime = std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, kMaxDeltaTime);
		float width = std::max(trailWidth, trailHeld - kTrailNarrowRate * deltaTime);
		if (!(width > 0.0f))
			width = boundRadius() * kWadeSpread;
		width = std::min(width, kTrailMaxWidth);
		shapeMotion[trailKey].width = width;

		if (trailLength > kTrailMinMotion) {
			Stamp stamp{};
			stamp.x = position.x;
			stamp.y = position.y;
			stamp.radius = width;
			stamp.clearance = std::max(rootClearance, 0.0f);
			stamp.rim = rim * std::clamp(1.0f - stamp.clearance / wadeHeight, 0.0f, 1.0f);
			stamp.motionX = trailX;
			stamp.motionY = trailY;
			stamp.vanillaSnow = IsVanillaSnow(tes->GetLandMaterialType(position)) ? 1.0f : 0.0f;
			stamp.landZ = landZ;
			stamp.normalZ = normalZ;
			stamp.owner = owner;
			stamp.kind = kKindWade;
			a_stamps.push_back(stamp);
		}
	}
}

const std::vector<SnowDeformation::CachedShape>& SnowDeformation::ActorCollisions(RE::Actor* a_actor, RE::NiAVObject* a_root)
{
	auto& entry = actorShapes[a_actor->GetFormID()];
	const bool ragdoll = a_actor->IsInRagdollState();
	bool rebuild = entry.root.get() != a_root || entry.ragdoll != ragdoll || dirtyScratch.contains(a_actor->GetFormID());
	if (!rebuild && gatherFrame >= entry.audit) {
		for (const auto& shape : entry.shapes) {
			RE::NiAVObject* owner = shape.owner.get();
			if (owner->collisionObject.get() != shape.object.get()) {
				rebuild = true;
				break;
			}
			uint32_t depth = 0;
			while (owner && owner != a_root && depth++ < 256)
				owner = owner->parent;
			if (owner != a_root) {
				rebuild = true;
				break;
			}
		}
		entry.audit = gatherFrame + kShapeAuditFrames + (a_actor->GetFormID() % 17);
	}
	if (rebuild) {
		entry.shapes.clear();
		entry.root.reset(a_root);
		entry.ragdoll = ragdoll;
		entry.audit = gatherFrame + kShapeAuditFrames + (a_actor->GetFormID() % 17);
		RE::BSVisit::TraverseScenegraphCollision(a_root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
			if (a_object->sceneObject)
				entry.shapes.push_back({ RE::NiPointer<RE::bhkNiCollisionObject>(a_object), RE::NiPointer<RE::NiAVObject>(a_object->sceneObject) });
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	}
	entry.seen = gatherFrame;
	return entry.shapes;
}

RE::BSEventNotifyControl SnowDeformation::EquipSink::ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*)
{
	if (a_event && a_event->actor) {
		auto& singleton = globals::features::snowDeformation;
		std::scoped_lock lock(singleton.dirtyLock);
		if (singleton.dirtyActors.size() < 4096)
			singleton.dirtyActors.insert(a_event->actor->GetFormID());
	}
	return RE::BSEventNotifyControl::kContinue;
}

void SnowDeformation::GatherStamps()
{
	if (loadClearMotion.exchange(false, std::memory_order_relaxed)) {
		shapeMotion.clear();
		actorShapes.clear();
		RestoreActorSinks();
	}
	if (!gatherEnabled.load(std::memory_order_relaxed)) {
		shapeMotion.clear();
		actorShapes.clear();
		RestoreActorSinks();
		std::scoped_lock lock(stampLock);
		pendingStamps.clear();
		measuredSinks.clear();
		return;
	}

	++gatherFrame;
	for (auto it = shapeMotion.begin(); it != shapeMotion.end();)
		it = (it->second.frame + kMotionForgetFrames < gatherFrame) ? shapeMotion.erase(it) : std::next(it);
	{
		std::scoped_lock lock(dirtyLock);
		dirtyScratch.swap(dirtyActors);
		dirtyActors.clear();
	}

	gatherStamps.clear();
	std::vector<RE::ActorPtr> actors;
	auto player = globals::game::player;
	if (player && player->Is3DLoaded()) {
		const RE::NiPoint3 anchor = player->GetPosition();
		AppendImpactStamps(gatherStamps);
		if (auto handle = player->GetHandle().get())
			actors.push_back(handle);
		RE::ActorPtr mount;
		if (player->IsOnMount() && player->GetMount(mount) && mount && mount->Is3DLoaded())
			actors.push_back(mount);
		const size_t fixedActors = actors.size();

		if (settings.IncludeNPCs) {
			if (auto lists = RE::ProcessLists::GetSingleton()) {
				for (auto& handle : lists->highActorHandles) {
					auto actor = handle.get();
					if (actor && actor.get() != player && actor != mount && actor->Is3DLoaded())
						actors.push_back(actor);
				}
			}
			std::sort(actors.begin() + fixedActors, actors.end(), [&anchor](const RE::ActorPtr& a_lhs, const RE::ActorPtr& a_rhs) {
				return anchor.GetSquaredDistance(a_lhs->GetPosition()) < anchor.GetSquaredDistance(a_rhs->GetPosition());
			});
		}

		for (const auto& actor : actors) {
			AppendActorStamps(actor.get(), anchor, gatherStamps);
			if (gatherStamps.size() >= kMaxStamps)
				break;
		}
		if (settings.IncludeObjects)
			AppendObjectStamps(gatherStamps);
		AppendHeatStamps(gatherStamps);
	}
	UpdateActorSinks(actors);
	MeasureBlanket();
	dirtyScratch.clear();
	for (auto it = actorShapes.begin(); it != actorShapes.end();)
		it = it->second.seen != gatherFrame ? actorShapes.erase(it) : std::next(it);

	std::scoped_lock lock(stampLock);
	pendingStamps.swap(gatherStamps);
}

void SnowDeformation::MeasureBlanket()
{
	{
		std::scoped_lock lock(stampLock);
		blanketScratch.swap(blanketRequests);
		blanketRequests.clear();
	}
	auto tes = globals::game::tes;
	if (blanketScratch.empty() || !tes)
		return;

	const float height = std::clamp(settings.BlanketHeight, 0.0f, kMaxBlanketHeight);
	std::vector<BlanketResult> results;
	results.reserve(blanketScratch.size());
	for (const auto& request : blanketScratch) {
		BlanketResult result{};
		result.x = request.x;
		result.y = request.y;
		const RE::NiPoint3 centre((static_cast<float>(request.x) + 0.5f) * kBlanketCell, (static_cast<float>(request.y) + 0.5f) * kBlanketCell, 0.0f);
		float landZ = request.landZ;
		bool landValid = !request.wantLand;
		if (request.wantLand) {
			result.hasLand = true;
			if (tes->GetLandHeight(centre, landZ) && std::isfinite(landZ)) {
				result.landZ = landZ;
				result.snow = IsVanillaSnow(tes->GetLandMaterialType(centre)) ? 1.0f : 0.0f;
				landValid = true;
			}
		}
		if (request.wantCap && landValid) {
			result.hasCap = true;
			result.cap = height > 0.0f ? MeshCap(tes, centre.x, centre.y, landZ, height) : 1.0f;
		}
		results.push_back(result);
	}
	blanketScratch.clear();

	std::scoped_lock lock(stampLock);
	if (blanketResults.size() < 4 * (kBlanketLandBudget + kCapRaysPerFrame))
		blanketResults.insert(blanketResults.end(), results.begin(), results.end());
}

void SnowDeformation::AppendObjectStamps(std::vector<Stamp>& a_stamps)
{
	auto tes = globals::game::tes;
	auto player = globals::game::player;
	if (!tes || !player || a_stamps.size() >= kMaxStamps)
		return;

	if (objectScanFrame == 0 || gatherFrame - objectScanFrame >= kObjectScanFrames) {
		objectScanFrame = gatherFrame;
		const RE::NiPoint3 anchor = player->GetPosition();
		std::vector<std::pair<float, RE::ObjectRefHandle>> found;
		tes->ForEachReferenceInRange(player, kObjectReach, [&](RE::TESObjectREFR* a_ref) {
			if (found.size() >= kMaxObjectRefs)
				return RE::BSContainer::ForEachResult::kStop;
			if (a_ref && !a_ref->As<RE::Actor>() && a_ref->Is3DLoaded() && !a_ref->IsDisabled() && !a_ref->IsMarkedForDeletion() && LeavesAMark(a_ref))
				found.emplace_back(anchor.GetSquaredDistance(a_ref->GetPosition()), a_ref->CreateRefHandle());
			return RE::BSContainer::ForEachResult::kContinue;
		});
		std::sort(found.begin(), found.end(), [](const auto& a_lhs, const auto& a_rhs) { return a_lhs.first < a_rhs.first; });
		objectRefs.clear();
		for (const auto& entry : found)
			objectRefs.push_back(entry.second);
	}

	const float depth = std::clamp(settings.Depth, 1.0f, 20.0f);
	const float rim = depth * kRimHeight;
	const float carve = std::clamp(settings.BlanketHeight, 0.0f, kMaxBlanketHeight);
	for (const auto& handle : objectRefs) {
		if (a_stamps.size() >= kMaxStamps)
			break;
		auto ref = handle.get();
		auto root = ref && ref->Is3DLoaded() ? ref->Get3D() : nullptr;
		if (!root)
			continue;

		contactScratch.clear();
		RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
			RE::NiPoint3 centre;
			float radius = 0.0f;
			if (Util::GetShapeBound(a_object, centre, radius) && radius > 0.1f && radius <= 128.0f)
				contactScratch.push_back({ a_object, centre, radius });
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		if (contactScratch.empty()) {
			const auto& bound = root->worldBound;
			if (!(bound.radius > 0.1f) || bound.radius > 128.0f)
				continue;
			contactScratch.push_back({ root, bound.center, bound.radius });
		}
		std::sort(contactScratch.begin(), contactScratch.end(), [](const Contact& a_lhs, const Contact& a_rhs) { return a_lhs.radius > a_rhs.radius; });
		if (contactScratch.size() > kMaxObjectShapes)
			contactScratch.resize(kMaxObjectShapes);

		float bottom = contactScratch[0].centre.z - contactScratch[0].radius;
		for (const auto& contact : contactScratch)
			bottom = std::min(bottom, contact.centre.z - contact.radius);
		const RE::NiPoint3 probe = contactScratch[0].centre;
		float landZ = 0.0f;
		if (!tes->GetLandHeight(probe, landZ))
			continue;
		const float drop = bottom - landZ;
		if (drop > kObjectContact || drop < -(carve + kBelowGroundLimit))
			continue;

		const float normalZ = LandNormalZ(tes, probe, landZ);
		const float vanillaSnow = IsVanillaSnow(tes->GetLandMaterialType(probe)) ? 1.0f : 0.0f;
		for (const auto& contact : contactScratch) {
			if (a_stamps.size() >= kMaxStamps)
				break;
			Stamp stamp{};
			stamp.x = contact.centre.x;
			stamp.y = contact.centre.y;
			stamp.radius = contact.radius;
			stamp.depth = depth;
			stamp.rim = rim;
			TrackShapeMotion(contact.key, contact.centre, false, stamp.motionX, stamp.motionY);
			stamp.vanillaSnow = vanillaSnow;
			stamp.landZ = landZ;
			stamp.normalZ = normalZ;
			a_stamps.push_back(stamp);
		}
	}
}

void SnowDeformation::AppendHeatStamps(std::vector<Stamp>& a_stamps)
{
	const auto& snowCover = globals::features::snowCover;
	auto tes = globals::game::tes;
	if (!tes || !snowCover.loaded || !snowCover.fireMeltSettings.Enabled || a_stamps.size() >= kMaxStamps)
		return;

	const auto& fires = snowCover.perFrame.fireMelt;
	const float meltFloor = -std::clamp(settings.BlanketHeight, 0.0f, kMaxBlanketHeight) * kHeatDepthScale;
	const uint32_t count = std::min<uint32_t>(fires.Count, static_cast<uint32_t>(std::size(fires.Spheres)));
	for (uint32_t i = 0; i < count && a_stamps.size() < kMaxStamps; ++i) {
		const auto& sphere = fires.Spheres[i];
		const RE::NiPoint3 position(sphere.x, sphere.y, sphere.z);
		float landZ = 0.0f;
		if (!tes->GetLandHeight(position, landZ) || std::abs(position.z - landZ) > 128.0f)
			continue;
		Stamp stamp{};
		stamp.x = position.x;
		stamp.y = position.y;
		stamp.radius = std::clamp(sphere.w * fires.RadiusScale * 0.5f, 8.0f, kExplosionMaxRadius);
		stamp.depth = kHeatMelt;
		stamp.rim = meltFloor;
		stamp.vanillaSnow = IsVanillaSnow(tes->GetLandMaterialType(position)) ? 1.0f : 0.0f;
		stamp.landZ = landZ;
		stamp.normalZ = LandNormalZ(tes, position, landZ);
		stamp.kind = kKindMelt;
		a_stamps.push_back(stamp);
	}
}

void SnowDeformation::QueueImpact(ImpactEvent a_event)
{
	if (!gatherEnabled.load(std::memory_order_relaxed) || !a_event.space || !IsFinite(a_event.position) || !IsFinite(a_event.direction))
		return;
	a_event.time = std::chrono::steady_clock::now();
	std::scoped_lock lock(impactLock);
	for (size_t i = 0; i < impactCount; ++i) {
		const auto& previous = impactQueue[(impactHead + i) % impactQueue.size()];
		if (previous.source == a_event.source && previous.kind == a_event.kind && a_event.time - previous.time < std::chrono::milliseconds(100) &&
			previous.position.GetSquaredDistance(a_event.position) < 256.0f)
			return;
	}
	if (impactCount == impactQueue.size())
		return;
	impactQueue[(impactHead + impactCount++) % impactQueue.size()] = a_event;
}

void SnowDeformation::AppendImpactStamps(std::vector<Stamp>& a_stamps)
{
	if (a_stamps.size() + 9 > kMaxStamps)
		return;
	ImpactEvent event{};
	bool found = false;
	{
		std::scoped_lock lock(impactLock);
		while (impactCount > 0) {
			event = impactQueue[impactHead];
			impactHead = (impactHead + 1) % impactQueue.size();
			--impactCount;
			if (std::chrono::steady_clock::now() - event.time < std::chrono::seconds(1)) {
				found = true;
				break;
			}
		}
	}
	if (found)
		BuildImpact(event, a_stamps);
}

void SnowDeformation::BuildImpact(const ImpactEvent& a_event, std::vector<Stamp>& a_stamps)
{
	auto tes = globals::game::tes;
	auto player = globals::game::player;
	if (!tes || !player || a_event.space != SpaceOf(player) || player->GetPosition().GetSquaredDistance(a_event.position) > kImpactReach * kImpactReach)
		return;
	float landZ = 0.0f;
	if (!tes->GetLandHeight(a_event.position, landZ))
		return;
	const float above = a_event.position.z - landZ;

	float reach = kImpactRadius;
	float depth = kImpactDepth;
	Pattern pattern{};
	if (a_event.kind == kImpactExplosion) {
		reach = std::clamp(a_event.radius * kExplosionRadiusScale, 0.0f, kExplosionMaxRadius);
		if (!(reach > 1.0f) || above < -16.0f || above >= reach)
			return;
		reach = std::sqrt(std::max(0.0f, reach * reach - above * above));
		depth = kExplosionDepth;
		pattern = ExplosionPattern(reach);
	} else if (a_event.kind == kImpactShout) {
		if (a_event.direction.z > 0.45f || above < 0.0f || above > 150.0f)
			return;
		reach = kShoutRange;
		depth = kShoutDepth;
		pattern = DirectionalPattern(reach, kShoutWidth, a_event.direction.x, a_event.direction.y, true);
	} else {
		if (above < -16.0f || above > 48.0f)
			return;
		const float planar = std::hypot(a_event.direction.x, a_event.direction.y);
		if (planar > std::abs(a_event.direction.z) * 0.35f && planar > 0.001f)
			pattern = DirectionalPattern(reach * 1.8f, reach, a_event.direction.x, a_event.direction.y, false);
		else
			pattern.strokes[pattern.count++] = { 0.0f, 0.0f, 0.0f, 0.0f, reach, 1.0f };
	}

	const float elementScale = a_event.element == kElementFrost ? 0.45f : (a_event.element == kElementFire ? 0.7f : 1.0f);
	const float blanket = std::clamp(settings.BlanketHeight, 0.0f, kMaxBlanketHeight);
	const float support = 1.0f + 2.0f * kRimWidth;
	for (size_t i = 0; i < pattern.count && a_stamps.size() < kMaxStamps; ++i) {
		const auto& stroke = pattern.strokes[i];
		RE::NiPoint3 point(a_event.position.x + stroke.x, a_event.position.y + stroke.y, landZ);
		float groundZ = 0.0f;
		if (!tes->GetLandHeight(point, groundZ))
			continue;
		point.z = groundZ + 3.0f;
		if (a_event.kind == kImpactShout) {
			const float distance = std::hypot(stroke.x, stroke.y);
			const float planar = std::max(std::hypot(a_event.direction.x, a_event.direction.y), 0.01f);
			const float beamZ = a_event.position.z + a_event.direction.z * distance / planar;
			if (beamZ - groundZ > 140.0f || beamZ - groundZ < -40.0f)
				continue;
		}
		RE::NiPoint3 origin = a_event.position;
		origin.z += 8.0f;
		if (!ClearPath(tes, origin, point))
			continue;

		Stamp stamp{};
		stamp.x = point.x;
		stamp.y = point.y;
		stamp.radius = stroke.radius / support;
		if (stamp.radius <= 0.5f)
			continue;
		stamp.motionX = stroke.motionX;
		stamp.motionY = stroke.motionY;
		stamp.depth = std::clamp(depth * elementScale * stroke.strength, 0.0f, kImpactMaxDepth);
		if (a_event.element == kElementFire)
			stamp.depth = std::clamp(std::max(stamp.depth, blanket * kImpactSnowMelt * stroke.strength), 0.0f, kImpactMaxDepth);
		stamp.rim = stamp.depth * kImpactRimScale;
		stamp.vanillaSnow = IsVanillaSnow(tes->GetLandMaterialType(point)) ? 1.0f : 0.0f;
		stamp.landZ = groundZ;
		stamp.normalZ = LandNormalZ(tes, point, groundZ);
		a_stamps.push_back(stamp);
	}
}

bool SnowDeformation::IsSinkBlocked(RE::Actor* a_actor) const
{
	if (!settings.SinkActors || settings.BlanketHeight > 0.0f || a_actor->IsDead() || a_actor->IsInRagdollState() || a_actor->IsOnMount())
		return true;
	if (auto state = a_actor->AsActorState(); state && state->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal)
		return true;
	auto controller = a_actor->GetCharController();
	if (!controller)
		return true;
	const auto support = controller->supportBody.get();
	return support && support->collidable.GetCollisionLayer() != RE::COL_LAYER::kTerrain;
}

void SnowDeformation::ApplySink(RE::TESObjectREFR* a_ref, ActorSink& a_state, float a_sink)
{
	auto root = a_ref ? a_ref->Get3D(false) : nullptr;
	auto node = FindSinkNode(root);
	if (!node) {
		a_state.applied = false;
		return;
	}

	const RE::NiPoint3 current = node->local.translate;
	const RE::NiPoint3 base = a_state.applied && current == a_state.written ? current - a_state.offset : current;
	RE::NiPoint3 offset;
	if (a_sink > 0.0f) {
		const auto& world = root->world;
		const float scale = world.scale > 1e-3f ? world.scale : 1.0f;
		offset = RE::NiPoint3(world.rotate.entry[2][0], world.rotate.entry[2][1], world.rotate.entry[2][2]) * (-a_sink / scale);
	}
	node->local.translate = base + offset;
	a_state.written = node->local.translate;
	a_state.offset = offset;
	a_state.applied = a_sink > 0.0f;
}

void SnowDeformation::RestoreActorSinks()
{
	for (auto& [id, state] : actorSinks) {
		if (state.applied) {
			if (auto ref = state.handle.get())
				ApplySink(ref.get(), state, 0.0f);
		}
	}
	actorSinks.clear();
	sunkActors = 0;
	deepestSink = 0.0f;
}

void SnowDeformation::UpdateActorSinks(const std::vector<RE::ActorPtr>& a_actors)
{
	gatherMeasured.clear();
	{
		std::scoped_lock lock(stampLock);
		gatherMeasured.swap(measuredSinks);
	}

	++sinkFrame;
	const bool paused = globals::game::ui && globals::game::ui->GameIsPaused();
	const float deltaTime = paused ? 0.0f : std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, kMaxDeltaTime);
	const float response = deltaTime > 0.0f ? 1.0f - std::exp(-deltaTime / kSinkResponse) : 0.0f;
	constexpr float amount = 1.0f;
	sunkActors = 0;
	deepestSink = 0.0f;

	for (const auto& actor : a_actors) {
		const RE::FormID id = actor->GetFormID();
		const auto measured = gatherMeasured.find(id);
		const bool blocked = IsSinkBlocked(actor.get());
		auto found = actorSinks.find(id);
		if (found == actorSinks.end()) {
			if (blocked || measured == gatherMeasured.end() || measured->second * amount < kMinSink)
				continue;
			found = actorSinks.emplace(id, ActorSink{ .handle = actor->GetHandle() }).first;
		}

		auto& state = found->second;
		state.frame = sinkFrame;
		if (blocked) {
			state.target = 0.0f;
		} else if (measured != gatherMeasured.end()) {
			state.target = measured->second * amount;
			state.sinceMeasure = 0.0f;
		} else if ((state.sinceMeasure += deltaTime) > kSinkHold) {
			state.target = 0.0f;
		}
		state.sink += (state.target - state.sink) * response;
		if (state.target <= 0.0f && state.sink < kMinSink)
			state.sink = 0.0f;

		ApplySink(actor.get(), state, state.sink);
		if (state.applied) {
			++sunkActors;
			deepestSink = std::max(deepestSink, state.sink);
		}
	}

	for (auto it = actorSinks.begin(); it != actorSinks.end();) {
		auto& state = it->second;
		if (state.frame == sinkFrame && (state.applied || state.target > 0.0f)) {
			++it;
			continue;
		}
		if (state.frame != sinkFrame && state.applied) {
			if (auto ref = state.handle.get())
				ApplySink(ref.get(), state, 0.0f);
		}
		it = actorSinks.erase(it);
	}
}

void SnowDeformation::RebuildActivity(Level& a_level)
{
	std::array<float, kWindowSpan * kWindowSpan> bounds{};
	for (const auto& slot : a_level.slots) {
		if (!slot.active)
			continue;
		const int32_t x = slot.tileX - a_level.windowTileX + kWindowTiles;
		const int32_t y = slot.tileY - a_level.windowTileY + kWindowTiles;
		if (x >= 0 && x < kWindowSpan && y >= 0 && y < kWindowSpan)
			bounds[y * kWindowSpan + x] = slot.bound;
	}

	a_level.activity.fill(0.0f);
	a_level.activitySum.fill(0);
	for (int32_t y = 0; y < kWindowSpan; ++y) {
		for (int32_t x = 0; x < kWindowSpan; ++x) {
			float dilated = 0.0f;
			for (int32_t dy = -1; dy <= 1; ++dy) {
				for (int32_t dx = -1; dx <= 1; ++dx) {
					const int32_t nx = x + dx;
					const int32_t ny = y + dy;
					if (nx >= 0 && nx < kWindowSpan && ny >= 0 && ny < kWindowSpan)
						dilated = std::max(dilated, bounds[ny * kWindowSpan + nx]);
				}
			}
			const uint32_t slotX = static_cast<uint32_t>(a_level.windowTileX + x - kWindowTiles) & (kSlots - 1);
			const uint32_t slotY = static_cast<uint32_t>(a_level.windowTileY + y - kWindowTiles) & (kSlots - 1);
			a_level.activity[slotY * kSlots + slotX] = dilated;
			a_level.activitySum[(y + 1) * (kWindowSpan + 1) + (x + 1)] = static_cast<uint16_t>(
				(dilated > 0.0f ? 1 : 0) + a_level.activitySum[y * (kWindowSpan + 1) + (x + 1)] + a_level.activitySum[(y + 1) * (kWindowSpan + 1) + x] - a_level.activitySum[y * (kWindowSpan + 1) + x]);
		}
	}

	globals::d3d::context->UpdateSubresource(a_level.activityTexture->resource.get(), 0, nullptr, a_level.activity.data(), kSlots * sizeof(float), 0);
	a_level.activityDirty = false;
}

bool SnowDeformation::OverlapsLevel(const Level& a_level, const RE::NiBound& a_bound) const
{
	if (!a_level.windowValid || !a_level.anyActive)
		return false;
	const double originX = static_cast<double>(a_level.windowTileX) * a_level.tileSize;
	const double originY = static_cast<double>(a_level.windowTileY) * a_level.tileSize;
	const double radius = (a_bound.radius > 0.0f ? a_bound.radius : 1.0e6f) + 2.0 * a_level.tileSize + 64.0;
	const int32_t x0 = std::max(FloorDiv(a_bound.center.x - originX - radius, a_level.tileSize) + kWindowTiles, 0);
	const int32_t x1 = std::min(FloorDiv(a_bound.center.x - originX + radius, a_level.tileSize) + kWindowTiles, kWindowSpan - 1);
	const int32_t y0 = std::max(FloorDiv(a_bound.center.y - originY - radius, a_level.tileSize) + kWindowTiles, 0);
	const int32_t y1 = std::min(FloorDiv(a_bound.center.y - originY + radius, a_level.tileSize) + kWindowTiles, kWindowSpan - 1);
	if (x0 > x1 || y0 > y1)
		return false;
	constexpr int32_t stride = kWindowSpan + 1;
	const int32_t sum = a_level.activitySum[(y1 + 1) * stride + (x1 + 1)] - a_level.activitySum[y0 * stride + (x1 + 1)] - a_level.activitySum[(y1 + 1) * stride + x0] + a_level.activitySum[y0 * stride + x0];
	return sum > 0;
}

bool SnowDeformation::OverlapsActivity(const RE::NiBound& a_bound) const
{
	for (const auto& level : levels) {
		if (OverlapsLevel(level, a_bound))
			return true;
	}
	return false;
}

void SnowDeformation::UpdateLevel(uint32_t a_index, const RE::NiPoint3& a_position, float a_decay, float a_deltaTime, bool a_sinking, bool a_gateSnowCover, bool a_skylightingReady)
{
	auto& level = levels[a_index];
	const bool fine = a_index == 0;
	const Level* coarse = fine ? &levels[1] : nullptr;

	const int32_t tileX = FloorDiv(a_position.x, level.tileSize);
	const int32_t tileY = FloorDiv(a_position.y, level.tileSize);
	if (!level.windowValid || std::abs(tileX - level.windowTileX) > kWindowTiles || std::abs(tileY - level.windowTileY) > kWindowTiles)
		ClearLevel(level);
	if (!level.windowValid || tileX != level.windowTileX || tileY != level.windowTileY) {
		++fieldSerial;
		level.activityDirty = true;
	}
	level.prevValid = level.windowValid;
	level.prevTileX = level.windowTileX;
	level.prevTileY = level.windowTileY;
	level.windowValid = true;
	level.windowTileX = tileX;
	level.windowTileY = tileY;

	std::array<TileEntry, kMaxTileEntries> entries{};
	std::array<int16_t, kSlots * kSlots> entryOfSlot;
	entryOfSlot.fill(-1);
	uint32_t entryCount = 0;
	auto setEntry = [&](uint32_t a_slot, int32_t a_flags) {
		const auto& slot = level.slots[a_slot];
		int16_t index = entryOfSlot[a_slot];
		if (index < 0) {
			index = static_cast<int16_t>(entryCount++);
			entryOfSlot[a_slot] = index;
		}
		auto& entry = entries[index];
		entry.packed = static_cast<int32_t>(a_slot & (kSlots - 1)) | static_cast<int32_t>((a_slot / kSlots) << 8) | (a_flags << 16);
		entry.tileX = slot.tileX;
		entry.tileY = slot.tileY;
	};
	auto slotIndex = [](int32_t a_tileX, int32_t a_tileY) {
		return (static_cast<uint32_t>(a_tileY) & (kSlots - 1)) * kSlots + (static_cast<uint32_t>(a_tileX) & (kSlots - 1));
	};
	auto inWindow = [](int32_t a_tileX, int32_t a_tileY, int32_t a_centreX, int32_t a_centreY) {
		return std::abs(a_tileX - a_centreX) <= kWindowTiles && std::abs(a_tileY - a_centreY) <= kWindowTiles;
	};

	for (uint32_t i = 0; i < level.slots.size(); ++i) {
		auto& slot = level.slots[i];
		if (!slot.active)
			continue;
		slot.bound *= a_decay;
		if (!inWindow(slot.tileX, slot.tileY, tileX, tileY) || slot.bound < kActiveEpsilon) {
			setEntry(i, kFlagClear);
			slot.active = false;
			slot.bound = 0.0f;
			level.activityDirty = true;
		} else if (a_decay < 1.0f) {
			setEntry(i, 0);
			level.activityDirty = true;
		}
	}

	const bool seeding = coarse && coarse->windowValid && coarse->anyActive;
	if (seeding) {
		constexpr int32_t ratio = static_cast<int32_t>(kCoarseScale);
		for (const auto& coarseSlot : coarse->slots) {
			if (!coarseSlot.active)
				continue;
			for (int32_t fy = coarseSlot.tileY * ratio; fy < (coarseSlot.tileY + 1) * ratio; ++fy) {
				for (int32_t fx = coarseSlot.tileX * ratio; fx < (coarseSlot.tileX + 1) * ratio; ++fx) {
					if (!inWindow(fx, fy, tileX, tileY) || (level.prevValid && inWindow(fx, fy, level.prevTileX, level.prevTileY)))
						continue;
					const uint32_t index = slotIndex(fx, fy);
					auto& slot = level.slots[index];
					if (slot.active)
						continue;
					slot = { fx, fy, coarseSlot.bound, true };
					setEntry(index, kFlagFresh | kFlagSeed);
					level.activityDirty = true;
				}
			}
		}
	}

	UpdateParams updateParams{};
	const double originX = static_cast<double>(tileX) * level.tileSize;
	const double originY = static_cast<double>(tileY) * level.tileSize;
	const float carve = blanketActive ? blanketHeight : 0.0f;

	for (uint32_t i = 0; i < frameStampCount; ++i) {
		const auto& stamp = frameStamps[i];
		const float snow = stampSnow[i];
		const float reach = stampReach[i];
		const float localX = static_cast<float>(stamp.x - originX);
		const float localY = static_cast<float>(stamp.y - originY);
		const float lowX = localX + std::min(0.0f, -stamp.motionX) - reach;
		const float lowY = localY + std::min(0.0f, -stamp.motionY) - reach;
		const float highX = localX + std::max(0.0f, -stamp.motionX) + reach;
		const float highY = localY + std::max(0.0f, -stamp.motionY) + reach;
		const bool wade = stamp.kind == kKindWade;
		const float stampCarve = wade ? std::max(carve - stamp.clearance, 0.0f) : carve;
		const float stampBound = stamp.kind == kKindMelt ? std::max(-stamp.rim, 2.0f * kActiveEpsilon) :
		                                                   std::max(stamp.depth * (1.0f + kChurn) * snow + stampCarve, stamp.rim * (1.0f + kRimNoise) * snow);

		updateParams.Stamps[i] = { localX, localY, stamp.radius, stamp.depth };
		updateParams.StampShape[i] = { stamp.forwardX, stamp.forwardY, stamp.halfWidth, stamp.rim };
		updateParams.StampMotion[i] = { stamp.motionX, stamp.motionY, stamp.vanillaSnow, carve };
		updateParams.StampSurface[i] = { stamp.landZ, stamp.normalZ, stamp.kind == kKindMelt ? 1.0f : 0.0f, wade ? 1.0f + stamp.clearance : 0.0f };
		updateParams.StampBounds[i] = { lowX, lowY, highX, highY };

		const int32_t tx0 = std::max(FloorDiv(lowX, level.tileSize), -kWindowTiles);
		const int32_t tx1 = std::min(FloorDiv(highX, level.tileSize), kWindowTiles);
		const int32_t ty0 = std::max(FloorDiv(lowY, level.tileSize), -kWindowTiles);
		const int32_t ty1 = std::min(FloorDiv(highY, level.tileSize), kWindowTiles);
		for (int32_t ty = ty0; ty <= ty1; ++ty) {
			for (int32_t tx = tx0; tx <= tx1; ++tx) {
				const int32_t absoluteX = tileX + tx;
				const int32_t absoluteY = tileY + ty;
				const uint32_t index = slotIndex(absoluteX, absoluteY);
				auto& slot = level.slots[index];
				if (slot.active && slot.tileX == absoluteX && slot.tileY == absoluteY) {
					slot.bound = std::max(slot.bound, stampBound);
					const int16_t existing = entryOfSlot[index];
					setEntry(index, existing >= 0 ? (entries[existing].packed >> 16) & (kFlagFresh | kFlagSeed) : 0);
				} else {
					slot = { absoluteX, absoluteY, stampBound, true };
					setEntry(index, seeding ? (kFlagFresh | kFlagSeed) : kFlagFresh);
				}
				level.activityDirty = true;
			}
		}
	}

	auto context = globals::d3d::context;
	const bool runBlanket = fine && blanketActive && blanketCS;
	if (entryCount > 0 || runBlanket) {
		auto& skylighting = globals::features::skylighting;
		const double coarseOriginX = coarse ? static_cast<double>(coarse->windowTileX) * coarse->tileSize : 0.0;
		const double coarseOriginY = coarse ? static_cast<double>(coarse->windowTileY) * coarse->tileSize : 0.0;

		updateParams.Control = { a_decay, static_cast<float>(frameStampCount), level.cellSize, kShoulder };
		updateParams.Shape = { kRimWidth, kRimNoise, kChurn, kReposeSlope };
		updateParams.Options = { a_gateSnowCover ? 1.0f : 0.0f, a_skylightingReady ? 1.0f : 0.0f, seeding ? 1.0f : 0.0f, kReposeRate };
		updateParams.EyeOffset = { static_cast<float>(originX - snapshotEye.x), static_cast<float>(originY - snapshotEye.y), -snapshotEye.z, 0.0f };
		updateParams.Probe = { level.tileSize * 0.5f, level.fadeStart, 1.0f / (level.fadeEnd - level.fadeStart), kProbeSpan };
		updateParams.OriginCell[0] = tileX * static_cast<int32_t>(kTileTexels);
		updateParams.OriginCell[1] = tileY * static_cast<int32_t>(kTileTexels);
		updateParams.OriginCell[2] = coarse ? static_cast<int32_t>((static_cast<uint32_t>(coarse->windowTileX) & (kSlots - 1)) * kTileTexels) : 0;
		updateParams.OriginCell[3] = coarse ? static_cast<int32_t>((static_cast<uint32_t>(coarse->windowTileY) & (kSlots - 1)) * kTileTexels) : 0;
		updateParams.BlanketCell[0] = blanketMinX;
		updateParams.BlanketCell[1] = blanketMinY;
		updateParams.BlanketShape = { blanketHeight, 0.0f, 0.0f, 0.0f };
		updateParams.BlanketFade = { static_cast<float>(static_cast<double>(blanketCentre.x) - originX), static_cast<float>(static_cast<double>(blanketCentre.y) - originY),
			kBlanketFadeStart, 1.0f / (kBlanketFadeEnd - kBlanketFadeStart) };
		updateParams.Coarse = { static_cast<float>(originX - coarseOriginX), static_cast<float>(originY - coarseOriginY), coarse ? 1.0f / coarse->cellSize : 0.0f, a_deltaTime };
		updateConstants->Update(updateParams);

		ID3D11Buffer* constants[1] = { updateConstants->CB() };
		ID3D11Buffer* liveBuffers[2] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
		ID3D11Buffer* sharedBuffers[2] = { snapshotValid ? sharedSnapshot.get() : liveBuffers[0], snapshotValid ? featureSnapshot.get() : liveBuffers[1] };
		ID3D11SamplerState* samplers[2] = { linearSampler.get(), fieldSampler.get() };
		ID3D11ShaderResourceView* snowMap = a_gateSnowCover ? globals::features::snowCover.views[6].get() : nullptr;
		ID3D11ShaderResourceView* probes = a_skylightingReady ? skylighting.texProbeArray->srv.get() : nullptr;

		globals::profiler->BeginPass(fine ? "SnowDeformation::Update" : "SnowDeformation::UpdateCoarse");
		context->CSSetConstantBuffers(0, 1, constants);
		context->CSSetConstantBuffers(5, 2, sharedBuffers);
		context->CSSetSamplers(0, 2, samplers);

		if (entryCount > 0) {
			tileList->Update(entries.data(), sizeof(TileEntry) * entries.size());
			{
				constexpr uint32_t copyBlocks = (kTileTexels + 2 + 7) / 8;
				ID3D11ShaderResourceView* copyResources[4] = { level.texture->srv.get(), nullptr, nullptr, tileList->SRV() };
				ID3D11UnorderedAccessView* copyUAV = beforeTexture->uav.get();
				context->CSSetShaderResources(0, 4, copyResources);
				context->CSSetUnorderedAccessViews(0, 1, &copyUAV, nullptr);
				context->CSSetShader(copyCS.get(), nullptr, 0);
				context->Dispatch(copyBlocks * copyBlocks, entryCount, 1);
				ID3D11ShaderResourceView* nullSource = nullptr;
				ID3D11UnorderedAccessView* nullCopyUAV = nullptr;
				context->CSSetShaderResources(0, 1, &nullSource);
				context->CSSetUnorderedAccessViews(0, 1, &nullCopyUAV, nullptr);
			}
			ID3D11ShaderResourceView* raise = blanketActive && blanketTexture ? blanketTexture->srv.get() : nullptr;
			ID3D11ShaderResourceView* resources[7] = { nullptr, snowMap, probes, tileList->SRV(), raise, beforeTexture->srv.get(), seeding ? coarse->texture->srv.get() : nullptr };
			ID3D11UnorderedAccessView* uav = level.texture->uav.get();
			context->CSSetShaderResources(0, 7, resources);
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(updateCS.get(), nullptr, 0);
			context->Dispatch((kTileTexels / 8) * (kTileTexels / 8), entryCount, 1);
			if (fine && a_sinking)
				DispatchProbes(frameStampCount);
		}

		if (runBlanket) {
			ID3D11ShaderResourceView* resources[5] = { nullptr, snowMap, probes, nullptr, blanketLandTexture->srv.get() };
			ID3D11UnorderedAccessView* uav = blanketTexture->uav.get();
			context->CSSetSamplers(0, 2, samplers);
			context->CSSetShaderResources(0, 5, resources);
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(blanketCS.get(), nullptr, 0);
			context->Dispatch(kBlanketTexels / 8, kBlanketTexels / 8, 1);
		}
		globals::profiler->EndPass();

		ID3D11ShaderResourceView* nullResources[7] = {};
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetShaderResources(0, 7, nullResources);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		context->CSSetConstantBuffers(0, 1, &nullBuffer);
		context->CSSetConstantBuffers(5, 2, liveBuffers);
		context->CSSetShader(nullptr, nullptr, 0);
		if (entryCount > 0)
			++fieldSerial;
	}
	debug.dispatchedTiles += entryCount;

	level.anyActive = false;
	level.maxBound = 0.0f;
	for (const auto& slot : level.slots) {
		if (slot.active) {
			level.anyActive = true;
			level.maxBound = std::max(level.maxBound, slot.bound);
			++debug.activeTiles;
		}
	}

	if (level.activityDirty)
		RebuildActivity(level);
}

void SnowDeformation::UpdateField()
{
	EndLandscapeDraw();
	prepassRouted.clear();
	prepassRejected.clear();
	mainCamera.valid = false;
	boundCamera.valid = false;
	debugShown = debug;
	debug = {};

	const bool active = IsActiveContext();
	gatherEnabled.store(active, std::memory_order_relaxed);
	if (loadClearField.exchange(false, std::memory_order_relaxed)) {
		ClearField();
		lightingRejected.clear();
		std::scoped_lock lock(stampLock);
		pendingStamps.clear();
		measuredSinks.clear();
	}
	if (!gatherOnUpdate)
		GatherStamps();
	{
		std::scoped_lock lock(decalLock);
		for (auto it = decalGeometries.begin(); it != decalGeometries.end();)
			it = (!active || it->second->GetRefCount() <= 1) ? decalGeometries.erase(it) : std::next(it);
	}
	if (!active) {
		if (windowValid)
			ClearField();
		std::scoped_lock lock(stampLock);
		pendingStamps.clear();
		return;
	}
	if (globals::state->isMapMenuOpen || !EnsureShaders() || !beforeTexture || !globals::state->sharedDataCB || !globals::state->featureDataCB)
		return;

	auto player = globals::game::player;
	auto worldspaceForm = player->GetWorldspace();
	if (!worldspaceForm)
		return;
	const RE::NiPoint3 position = player->GetPosition();
	const RE::FormID worldspace = worldspaceForm->GetFormID();
	if (!windowValid || worldspace != windowWorldspace)
		ClearField();
	windowValid = true;
	windowWorldspace = worldspace;

	ReadProbes();
	UpdateBlanket(position, worldspace);
	{
		std::scoped_lock lock(stampLock);
		frameStamps.swap(pendingStamps);
		pendingStamps.clear();
	}

	const bool paused = globals::game::ui && globals::game::ui->GameIsPaused();
	const float deltaTime = paused ? 0.0f : std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, kMaxDeltaTime);
	bool snowing = false;
	if (auto sky = globals::game::sky; sky && sky->currentWeather)
		snowing = sky->currentWeather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow);
	const float keepPerSecond = std::pow(0.1f, 1.0f / std::clamp(settings.Lifetime, 10.0f, 1800.0f)) * (1.0f - (snowing ? kSnowfallRefill : 0.0f));
	const float decay = deltaTime > 0.0f ? std::pow(keepPerSecond, deltaTime) : 1.0f;

	const bool snowCover = IsSnowCoverActive();
	probeOwners.clear();
	stampSnow.clear();
	stampReach.clear();
	std::vector<std::pair<RE::FormID, float>> offSnow;
	uint32_t kept = 0;
	for (size_t s = 0; s < frameStamps.size() && kept < kMaxStamps; ++s) {
		const auto& stamp = frameStamps[s];
		if (stamp.kind == kKindWade && !blanketActive)
			continue;
		const float reach = std::max(stamp.radius, stamp.halfWidth) * (1.0f + 2.0f * kRimWidth) + 2.0f * kCellSize * kCoarseScale;
		const float snow = std::max(stamp.vanillaSnow, snowCover ? SnowCoverUpperBound(stamp, reach) : 0.0f);
		const float wade = stamp.kind == kKindWade ? blanketHeight - stamp.clearance : 0.0f;
		if (snow * std::max({ stamp.depth, stamp.rim, wade }) < kActiveEpsilon) {
			offSnow.emplace_back(stamp.owner, 0.0f);
			continue;
		}
		frameStamps[kept++] = stamp;
		stampSnow.push_back(snow);
		stampReach.push_back(reach);
		probeOwners.push_back(stamp.owner);
	}
	frameStampCount = kept;
	debug.stamps = kept;

	const bool sinking = settings.SinkActors && settings.BlanketHeight <= 0.0f;
	if (sinking) {
		std::erase_if(offSnow, [this](const auto& a_entry) { return std::find(probeOwners.begin(), probeOwners.end(), a_entry.first) != probeOwners.end(); });
		PublishMeasurements(offSnow);
	}

	auto& skylighting = globals::features::skylighting;
	const bool gateSnowCover = snowCover && snapshotValid;
	const bool skylightingReady = gateSnowCover && updateSkylighting && skylighting.loaded && skylighting.texProbeArray && skylighting.texProbeArray->srv;
	debug.snowCover = gateSnowCover;

	for (uint32_t i = kLevels; i-- > 0;)
		UpdateLevel(i, position, decay, deltaTime, sinking, gateSnowCover, skylightingReady);
}

void SnowDeformation::UpdateBlanket(const RE::NiPoint3& a_position, RE::FormID a_worldspace)
{
	const float height = std::clamp(settings.BlanketHeight, 0.0f, kMaxBlanketHeight);
	const bool wasActive = blanketActive;
	const float lastHeight = blanketHeight;
	blanketActive = false;
	blanketHeight = height;

	auto tes = globals::game::tes;
	if (height > 0.0f && tes && blanketLandTexture && blanketTexture && blanketCS && !blanketLand.empty()) {
		if (a_worldspace != blanketWorldspace) {
			std::fill(blanketOwner.begin(), blanketOwner.end(), std::array<int32_t, 2>{ std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::min() });
			blanketWorldspace = a_worldspace;
		}

		blanketCentre = a_position;
		blanketMinX = FloorDiv(a_position.x, kBlanketCell) - kBlanketTexels / 2;
		blanketMinY = FloorDiv(a_position.y, kBlanketCell) - kBlanketTexels / 2;

		uint32_t landBudget = kBlanketLandBudget;
		bool dirty = false;
		bool vanillaSnow = false;
		std::vector<BlanketRequest> requests;
		{
			std::scoped_lock lock(stampLock);
			for (const auto& result : blanketResults) {
				const size_t index = static_cast<size_t>(result.y & (kBlanketTexels - 1)) * kBlanketTexels + static_cast<size_t>(result.x & (kBlanketTexels - 1));
				if (blanketOwner[index][0] != result.x || blanketOwner[index][1] != result.y)
					continue;
				auto& land = blanketLand[index];
				if (result.hasLand) {
					if (result.snow >= 0.0f) {
						land[0] = result.landZ;
						land[1] = result.snow;
					} else {
						land[1] = -1.0f;
						blanketRetry[index] = frameIndex + kBlanketRetryFrames;
					}
				}
				if (result.hasCap) {
					land[2] = result.cap;
					blanketCapFrame[index] = frameIndex;
				}
				dirty = true;
			}
			blanketResults.clear();
		}
		float maxLand = -std::numeric_limits<float>::max();
		blanketReady = 0;
		for (int32_t sy = 0; sy < kBlanketTexels; ++sy) {
			const int32_t wy = blanketMinY + ((sy - blanketMinY) & (kBlanketTexels - 1));
			for (int32_t sx = 0; sx < kBlanketTexels; ++sx) {
				const int32_t wx = blanketMinX + ((sx - blanketMinX) & (kBlanketTexels - 1));
				const size_t index = static_cast<size_t>(sy) * kBlanketTexels + sx;
				auto& owner = blanketOwner[index];
				auto& land = blanketLand[index];
				if (owner[0] != wx || owner[1] != wy) {
					owner = { wx, wy };
					land = { 0.0f, -1.0f, 1.0f, 0.0f };
					blanketRetry[index] = 0;
					blanketCapFrame[index] = 0;
					dirty = true;
				}
				if (land[1] < 0.0f) {
					if (landBudget > 0 && blanketRetry[index] <= frameIndex) {
						--landBudget;
						blanketRetry[index] = frameIndex + 4;
						requests.push_back({ wx, wy, 0.0f, true, wasActive && blanketCapFrame[index] == 0 });
					}
					continue;
				}
				++blanketReady;
				vanillaSnow = vanillaSnow || land[1] > 0.5f;
				maxLand = std::max(maxLand, land[0]);
			}
		}

		if (dirty)
			globals::d3d::context->UpdateSubresource(blanketLandTexture->resource.get(), 0, nullptr, blanketLand.data(), kBlanketTexels * sizeof(float) * 4, 0);

		if (wasActive) {
			constexpr uint32_t cells = kBlanketTexels * kBlanketTexels;
			uint32_t rays = 0;
			for (uint32_t index = 0; index < cells && rays < kCapRaysPerFrame; ++index) {
				if (blanketLand[index][1] >= 0.0f && blanketCapFrame[index] == 0) {
					blanketCapFrame[index] = frameIndex;
					requests.push_back({ blanketOwner[index][0], blanketOwner[index][1], blanketLand[index][0], false, true });
					++rays;
				}
			}
			for (uint32_t step = 0; step < cells && rays < kCapRaysPerFrame; ++step) {
				const uint32_t index = (blanketCapCursor + step) % cells;
				if (blanketLand[index][1] >= 0.0f && frameIndex - blanketCapFrame[index] >= kCapRefreshFrames) {
					blanketCapFrame[index] = frameIndex;
					requests.push_back({ blanketOwner[index][0], blanketOwner[index][1], blanketLand[index][0], false, true });
					++rays;
					blanketCapCursor = (index + 1) % cells;
				}
			}
		}
		{
			std::scoped_lock lock(stampLock);
			blanketRequests.swap(requests);
		}

		bool snowCoverPossible = false;
		if (blanketReady > 0 && IsSnowCoverActive()) {
			Stamp probe{};
			probe.x = a_position.x;
			probe.y = a_position.y;
			probe.landZ = maxLand;
			probe.normalZ = 1.0f;
			snowCoverPossible = SnowCoverUpperBound(probe, kBlanketCell * static_cast<float>(kBlanketTexels / 2)) > 0.0f;
		}
		blanketActive = blanketReady > 0 && (vanillaSnow || snowCoverPossible);
	}

	if (blanketActive != wasActive || (blanketActive && blanketHeight != lastHeight))
		++fieldSerial;
}

bool SnowDeformation::InBlanketRange(const RE::NiBound& a_bound) const
{
	if (!blanketActive)
		return false;
	const float reach = kBlanketFadeEnd + kBlanketCell + std::max(a_bound.radius, 0.0f);
	return std::abs(a_bound.center.x - blanketCentre.x) < reach && std::abs(a_bound.center.y - blanketCentre.y) < reach;
}

void SnowDeformation::DispatchProbes(uint32_t a_stampCount)
{
	auto& staging = probeStaging[probeWrite];
	if (!probeCS || !probeBuffer || !staging || a_stampCount == 0)
		return;

	auto context = globals::d3d::context;
	ID3D11UnorderedAccessView* uav = probeBuffer->uav.get();
	ID3D11ShaderResourceView* field = levels[0].texture->srv.get();
	ID3D11SamplerState* sampler = fieldSampler.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 1, &field);
	context->CSSetSamplers(0, 1, &sampler);
	context->CSSetShader(probeCS.get(), nullptr, 0);
	context->Dispatch((a_stampCount + 63) / 64, 1, 1);
	context->CopyResource(staging.get(), probeBuffer->resource.get());

	auto& frame = probeFrames[probeWrite];
	frame.owners.assign(probeOwners.begin(), probeOwners.begin() + a_stampCount);
	frame.pending = true;
	probeWrite = (probeWrite + 1) % kProbeLatency;
}

void SnowDeformation::ReadProbes()
{
	auto context = globals::d3d::context;
	std::vector<std::pair<RE::FormID, float>> measurements;
	std::vector<std::pair<RE::FormID, float>> frameMeasurements;
	for (uint32_t n = 0; n < kProbeLatency; ++n) {
		const uint32_t index = (probeWrite + n) % kProbeLatency;
		auto& frame = probeFrames[index];
		if (!frame.pending || !probeStaging[index])
			continue;

		D3D11_MAPPED_SUBRESOURCE mapped{};
		const HRESULT result = context->Map(probeStaging[index].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
		if (result == DXGI_ERROR_WAS_STILL_DRAWING)
			break;
		frame.pending = false;
		if (FAILED(result))
			continue;

		frameMeasurements.clear();
		const auto* depths = static_cast<const float*>(mapped.pData);
		for (size_t i = 0; i < frame.owners.size(); ++i) {
			const float depth = std::isfinite(depths[i]) ? std::clamp(depths[i], 0.0f, kMaxSink) : 0.0f;
			auto found = std::find_if(frameMeasurements.begin(), frameMeasurements.end(), [&](const auto& a_entry) { return a_entry.first == frame.owners[i]; });
			if (found == frameMeasurements.end())
				frameMeasurements.emplace_back(frame.owners[i], depth);
			else
				found->second = std::max(found->second, depth);
		}
		context->Unmap(probeStaging[index].get(), 0);

		for (const auto& entry : frameMeasurements) {
			auto found = std::find_if(measurements.begin(), measurements.end(), [&](const auto& a_entry) { return a_entry.first == entry.first; });
			if (found == measurements.end())
				measurements.push_back(entry);
			else
				found->second = entry.second;
		}
	}
	PublishMeasurements(measurements);
}

void SnowDeformation::PublishMeasurements(const std::vector<std::pair<RE::FormID, float>>& a_measurements)
{
	if (a_measurements.empty())
		return;
	std::scoped_lock lock(stampLock);
	for (const auto& [owner, depth] : a_measurements)
		measuredSinks[owner] = depth;
}

float SnowDeformation::SnowCoverUpperBound(const Stamp& a_stamp, float a_reach) const
{
	const auto& snowCover = globals::features::snowCover;
	const auto& world = snowCover.wsettings;
	float map = world.mapZscale < 0.0f ? 0.0f : 1.0f;
	const float u0 = static_cast<float>(a_stamp.x - a_reach) * world.mapScale.x + world.mapOffset.x;
	const float u1 = static_cast<float>(a_stamp.x + a_reach) * world.mapScale.x + world.mapOffset.x;
	const float v0 = static_cast<float>(a_stamp.y - a_reach) * world.mapScale.y + world.mapOffset.y;
	const float v1 = static_cast<float>(a_stamp.y + a_reach) * world.mapScale.y + world.mapOffset.y;
	if (float value = 0.0f; world.mapZscale >= 0.0f && snowCover.GetSnowMapMax(u0, v0, u1, v1, value))
		map = value;

	const float environment = (a_stamp.landZ - snowCover.settings.SnowHeightOffset + (map - 0.5f) * world.mapZscale + snowCover.perFrame.SeasonalAltitude) /
	                          std::max(world.BlendSmoothness, 1.0f);
	const float timeSnowing = snowCover.perFrame.TimeSnowing;
	const float weather = std::clamp(timeSnowing * timeSnowing * timeSnowing * std::max(500.0f, snowCover.perFrame.SnowingDensity) / 500.0f * std::max(world.MinAngle, a_stamp.normalZ), -1.0f, 1.0f);
	const float slope = std::clamp((a_stamp.normalZ - world.MinAngle) / std::max(world.MaxAngle - world.MinAngle, 1e-4f), 0.0f, 1.0f);
	return std::clamp(std::max(environment, weather), 0.0f, 1.0f) * slope * slope * (3.0f - 2.0f * slope);
}

bool SnowDeformation::IsLandscapePass(RE::BSRenderPass* a_pass) const
{
	if (!a_pass || !a_pass->shaderProperty || !a_pass->geometry)
		return false;
	if (!a_pass->shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape))
		return false;
	auto material = a_pass->shaderProperty->GetBaseMaterial();
	if (!material)
		return false;
	const auto feature = material->GetFeature();
	return feature == RE::BSShaderMaterial::Feature::kMultiTexLand || feature == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend;
}

SnowDeformation::Layout SnowDeformation::ValidateBoundVertexShader(DrawKind a_kind)
{
	auto context = globals::d3d::context;
	ID3D11VertexShader* bound = nullptr;
	context->VSGetShader(&bound, nullptr, nullptr);
	if (bound)
		bound->Release();
	if (!bound)
		return Layout::Invalid;

	const bool lighting = a_kind == DrawKind::Lighting;
	auto& cache = lighting ? lightingLayouts : depthLayouts;
	const auto layout = lighting ? std::span<const ExpectedElement>(kLightingLayout) : std::span<const ExpectedElement>(kDepthLayout);
	const auto& terrainBlending = globals::features::terrainBlending;
	allowedShaderCount = 0;

	auto validate = [&](ID3D11VertexShader* a_shader) -> Layout {
		if (a_kind == DrawKind::Depth && terrainBlending.loaded && a_shader == terrainBlending.terrainOffsetVertexShader)
			return Layout::Valid;

		const RE::BSGraphics::VertexShader* source = nullptr;
		auto vanilla = *globals::game::currentVertexShader;
		if (vanilla && reinterpret_cast<ID3D11VertexShader*>(vanilla->shader) == a_shader) {
			source = vanilla;
		} else if (auto state = globals::state; state->currentShader) {
			auto replacement = globals::shaderCache->GetVertexShader(*state->currentShader, state->modifiedVertexDescriptor);
			if (replacement && reinterpret_cast<ID3D11VertexShader*>(replacement->shader) == a_shader)
				source = replacement;
		}
		if (!source || source->byteCodeSize < 20)
			return Layout::Unknown;

		uint64_t checksum[2] = {};
		std::memcpy(checksum, source->rawBytecode + 4, sizeof(checksum));
		const uint64_t key = checksum[0] ^ (checksum[1] * 0x9E3779B97F4A7C15ull);
		if (auto found = cache.find(key); found != cache.end())
			return found->second;

		const Layout result = MatchesLayout(source->rawBytecode, source->byteCodeSize, layout) ? Layout::Valid : Layout::Invalid;
		cache.emplace(key, result);
		if (result == Layout::Invalid)
			logger::info("[SnowDeformation] {} vertex shader {:X} does not have the landscape output layout; its draws are not displaced", lighting ? "Lighting" : "Depth", source->id);
		return result;
	};

	auto allow = [&](ID3D11VertexShader* a_shader) {
		if (allowedShaderCount < allowedShaders.size())
			allowedShaders[allowedShaderCount++] = a_shader;
	};

	Layout result = validate(bound);
	if (result == Layout::Valid)
		allow(bound);
	if (result == Layout::Valid && a_kind == DrawKind::Depth && terrainBlending.loaded && terrainBlending.settings.Enabled) {
		auto vanilla = *globals::game::currentVertexShader;
		auto vanillaShader = vanilla ? reinterpret_cast<ID3D11VertexShader*>(vanilla->shader) : nullptr;
		if (!vanillaShader) {
			result = Layout::Invalid;
		} else if (vanillaShader != bound) {
			result = validate(vanillaShader);
			if (result == Layout::Valid)
				allow(vanillaShader);
		}
		if (result == Layout::Valid && terrainBlending.terrainOffsetVertexShader && terrainBlending.terrainOffsetVertexShader != bound)
			allow(terrainBlending.terrainOffsetVertexShader);
	}
	if (result != Layout::Valid)
		allowedShaderCount = 0;
	return result;
}

bool SnowDeformation::PrepareCamera(DrawKind a_kind, bool a_clamped)
{
	const auto& frameBuffer = globals::game::frameBufferCached;
	const auto& viewProj = frameBuffer.GetCameraViewProj();
	const auto& posAdjust = frameBuffer.GetCameraPosAdjust();

	CameraKey key{};
	std::memcpy(key.viewProj.data(), &viewProj, sizeof(key.viewProj));
	key.posAdjust = { posAdjust.x, posAdjust.y, posAdjust.z, 0.0f };
	key.serial = fieldSerial;
	key.clamped = a_clamped;
	key.shadow = a_kind == DrawKind::Shadow;
	key.valid = true;

	const auto m = [&](int a_row, int a_column) { return static_cast<double>(key.viewProj[a_row * 4 + a_column]); };
	const bool perspective = std::abs(m(3, 0)) > 1e-6 || std::abs(m(3, 1)) > 1e-6 || std::abs(m(3, 2)) > 1e-6;
	if (!perspective && a_kind != DrawKind::Shadow)
		return false;

	if (a_kind != DrawKind::Shadow) {
		if (!mainCamera.valid) {
			if (a_kind != DrawKind::Depth)
				return false;
			mainCamera = key;
			mainPosAdjust = { key.posAdjust.x, key.posAdjust.y, key.posAdjust.z };
			const float renderHeight = Util::ConvertToDynamic({ 0.0f, static_cast<float>(globals::game::graphicsState->screenHeight) }).y;
			mainPixelScale = static_cast<float>(std::sqrt(m(1, 0) * m(1, 0) + m(1, 1) * m(1, 1) + m(1, 2) * m(1, 2))) * 0.5f * renderHeight;
		} else if (std::memcmp(mainCamera.viewProj.data(), key.viewProj.data(), sizeof(key.viewProj)) != 0 ||
				   mainCamera.posAdjust.x != key.posAdjust.x || mainCamera.posAdjust.y != key.posAdjust.y || mainCamera.posAdjust.z != key.posAdjust.z) {
			return false;
		}
	}

	if (boundCamera.valid && boundCamera.serial == key.serial && boundCamera.clamped == key.clamped && boundCamera.shadow == key.shadow && boundCamera.posAdjust.x == key.posAdjust.x &&
		boundCamera.posAdjust.y == key.posAdjust.y &&
		boundCamera.posAdjust.z == key.posAdjust.z && std::memcmp(boundCamera.viewProj.data(), key.viewProj.data(), sizeof(key.viewProj)) == 0)
		return true;

	const int thirdRow = perspective ? 3 : 2;
	std::array<std::array<double, 4>, 3> rows{};
	for (int column = 0; column < 4; ++column) {
		rows[0][column] = m(0, column);
		rows[1][column] = m(1, column);
		rows[2][column] = m(thirdRow, column);
	}
	std::array<std::array<double, 3>, 3> inverse{};
	if (!InvertRows(rows, inverse))
		return false;

	const auto& fine = levels[0];
	const auto& coarse = levels[1];
	const double originX = static_cast<double>(fine.windowTileX) * fine.tileSize;
	const double originY = static_cast<double>(fine.windowTileY) * fine.tileSize;
	const double coarseOriginX = static_cast<double>(coarse.windowTileX) * coarse.tileSize;
	const double coarseOriginY = static_cast<double>(coarse.windowTileY) * coarse.tileSize;
	const float maxBound = std::max(fine.maxBound, coarse.maxBound);

	DrawParams params{};
	params.InverseRow0 = { static_cast<float>(inverse[0][0]), static_cast<float>(inverse[0][1]), static_cast<float>(inverse[0][2]), 0.0f };
	params.InverseRow1 = { static_cast<float>(inverse[1][0]), static_cast<float>(inverse[1][1]), static_cast<float>(inverse[1][2]), 0.0f };
	params.InverseRow2 = { static_cast<float>(inverse[2][0]), static_cast<float>(inverse[2][1]), static_cast<float>(inverse[2][2]), 0.0f };
	params.Translation = { static_cast<float>(rows[0][3]), static_cast<float>(rows[1][3]), static_cast<float>(rows[2][3]), perspective ? 1.0f : 0.0f };
	params.DepthColumn = { static_cast<float>(m(0, 2)), static_cast<float>(m(1, 2)), static_cast<float>(m(2, 2)), static_cast<float>(m(3, 2)) };
	params.WindowOffset = { static_cast<float>(static_cast<double>(posAdjust.x) - originX), static_cast<float>(static_cast<double>(posAdjust.y) - originY), fine.tileSize, 1.0f / fine.tileSize };
	params.EyePosition = { mainPosAdjust.x - posAdjust.x, mainPosAdjust.y - posAdjust.y, mainPosAdjust.z - posAdjust.z, 0.0f };
	params.FieldParams = { 1.0f / fine.cellSize, static_cast<float>((static_cast<uint32_t>(fine.windowTileX) & (kSlots - 1)) * kTileTexels),
		static_cast<float>((static_cast<uint32_t>(fine.windowTileY) & (kSlots - 1)) * kTileTexels), 1.0f / static_cast<float>(kTexels) };
	params.FadeParams = { fine.tileSize * 0.5f, fine.fadeStart, 1.0f / (fine.fadeEnd - fine.fadeStart), 2.0f * fine.cellSize };
	const float spacingScale = a_kind == DrawKind::Shadow ? 2.0f : 1.0f;
	params.TessParams = { kMaxTessellation, 1.0f / (kMinSpacing * spacingScale), 1.0f / kDetailDistance, kMinNormalZ };
	params.DetailParams = { mainPixelScale, 1.0f / (std::clamp(settings.TrianglePixels, 2.0f, 32.0f) * spacingScale), 1.0f / kAmplitudePixels, 0.0f };
	const bool blanket = blanketActive;
	params.RangeParams = { maxBound, maxBound + (blanket ? blanketHeight : 0.0f), a_clamped ? 1.0f : 0.0f, 1.0f / kBoundDepth };
	const double blanketPeriod = static_cast<double>(kBlanketTexels);
	const double blanketOffsetX = std::fmod(originX / kBlanketCell, blanketPeriod);
	const double blanketOffsetY = std::fmod(originY / kBlanketCell, blanketPeriod);
	params.BlanketParams = { static_cast<float>(blanketOffsetX < 0.0 ? blanketOffsetX + blanketPeriod : blanketOffsetX),
		static_cast<float>(blanketOffsetY < 0.0 ? blanketOffsetY + blanketPeriod : blanketOffsetY), blanket ? 1.0f : 0.0f, 1.0f / (kBlanketSpacing * spacingScale) };
	params.BlanketWindow = { static_cast<float>(static_cast<double>(blanketMinX) * kBlanketCell - originX), static_cast<float>(static_cast<double>(blanketMinY) * kBlanketCell - originY), 0.0f, 0.0f };
	params.SlotParams[0] = static_cast<int32_t>(static_cast<uint32_t>(fine.windowTileX) & (kSlots - 1));
	params.SlotParams[1] = static_cast<int32_t>(static_cast<uint32_t>(fine.windowTileY) & (kSlots - 1));
	params.SlotParams[2] = kWindowTiles;
	params.SlotParams[3] = static_cast<int32_t>(kSlots - 1);
	params.WindowOffset1 = { static_cast<float>(originX - coarseOriginX), static_cast<float>(originY - coarseOriginY), coarse.tileSize, 1.0f / coarse.tileSize };
	params.FieldParams1 = { 1.0f / coarse.cellSize, static_cast<float>((static_cast<uint32_t>(coarse.windowTileX) & (kSlots - 1)) * kTileTexels),
		static_cast<float>((static_cast<uint32_t>(coarse.windowTileY) & (kSlots - 1)) * kTileTexels), 2.0f * coarse.cellSize };
	params.FadeParams1 = { coarse.tileSize * 0.5f, coarse.fadeStart, 1.0f / (coarse.fadeEnd - coarse.fadeStart), coarse.windowValid ? 1.0f : 0.0f };
	params.SlotParams1[0] = static_cast<int32_t>(static_cast<uint32_t>(coarse.windowTileX) & (kSlots - 1));
	params.SlotParams1[1] = static_cast<int32_t>(static_cast<uint32_t>(coarse.windowTileY) & (kSlots - 1));
	params.SlotParams1[2] = kWindowTiles;
	params.SlotParams1[3] = static_cast<int32_t>(kSlots - 1);
	drawConstants->Update(params);

	boundCamera = key;
	return true;
}

void SnowDeformation::BeginLandscapeDraw(RE::BSRenderPass* a_pass, DrawKind a_kind)
{
	if (drawActive)
		EndLandscapeDraw();
	if (shaderCompileFailed || !windowValid)
		return;

	const bool learnLayout = a_kind == DrawKind::Lighting && !lightingLayoutSeen;
	const bool displacing = levels[0].anyActive || levels[1].anyActive || blanketActive;
	if (!displacing && !learnLayout)
		return;

	auto state = globals::state;
	if (state->isMapMenuOpen || !lightingHS || !depthHS || !lightingDS || !depthDS)
		return;

	if (!IsLandscapePass(a_pass)) {
		if (a_kind == DrawKind::Lighting && displacing && BeginDecalDraw(a_pass)) {
			activeKind = DrawKind::Decal;
			activeGeometry = a_pass->geometry;
			drawActive = true;
		}
		return;
	}

	switch (a_kind) {
	case DrawKind::Lighting:
		{
			const Layout layout = ValidateBoundVertexShader(DrawKind::Lighting);
			if (layout == Layout::Valid)
				lightingLayoutSeen = true;
			if (!displacing || IsReflectionPass() || !prepassRouted.contains(a_pass->geometry) || prepassRejected.contains(a_pass->geometry))
				return;
			if (layout != Layout::Valid) {
				debug.layoutRejects++;
				BlockLighting(a_pass->geometry);
				return;
			}
			if (!PrepareCamera(DrawKind::Lighting, false)) {
				debug.skippedDraws++;
				return;
			}
			debug.lightingDraws++;
			break;
		}
	case DrawKind::Depth:
		{
			if (!inZPrepass || !lightingLayoutSeen || IsReflectionPass() || state->currentShader == nullptr ||
				state->currentShader->shaderType.get() != RE::BSShader::Type::Utility || !IsCameraDepthTechnique(state->modifiedVertexDescriptor))
				return;
			if (IsLightingBlocked(a_pass->geometry) || !(OverlapsActivity(a_pass->geometry->worldBound) || InBlanketRange(a_pass->geometry->worldBound)))
				return;
			if (ValidateBoundVertexShader(DrawKind::Depth) != Layout::Valid) {
				debug.layoutRejects++;
				return;
			}
			if (!PrepareCamera(DrawKind::Depth, false)) {
				debug.skippedDraws++;
				return;
			}
			debug.depthDraws++;
			break;
		}
	case DrawKind::Shadow:
		{
			if (!settings.DisplaceShadows || state->currentShader == nullptr || state->currentShader->shaderType.get() != RE::BSShader::Type::Utility ||
				!IsShadowTechnique(state->modifiedVertexDescriptor))
				return;
			if (!(OverlapsActivity(a_pass->geometry->worldBound) || InBlanketRange(a_pass->geometry->worldBound)))
				return;
			if (ValidateBoundVertexShader(DrawKind::Shadow) != Layout::Valid) {
				debug.layoutRejects++;
				return;
			}
			const bool clamped = (state->modifiedVertexDescriptor & static_cast<uint32_t>(RE::BSUtilityShader::Flags::RenderShadowmapClamped)) != 0;
			if (!PrepareCamera(DrawKind::Shadow, clamped)) {
				debug.skippedDraws++;
				return;
			}
			debug.shadowDraws++;
			break;
		}
	}

	activeKind = a_kind;
	activeGeometry = a_pass->geometry;
	activeHS = a_kind == DrawKind::Lighting ? lightingHS.get() : depthHS.get();
	activeDS = a_kind == DrawKind::Lighting ? lightingDS.get() : depthDS.get();
	drawActive = true;
}

bool SnowDeformation::BeginDecalDraw(RE::BSRenderPass* a_pass)
{
	auto property = a_pass->shaderProperty;
	using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
	if (!property || !property->flags.any(Flag::kDecal, Flag::kDynamicDecal) || IsReflectionPass())
		return false;
	{
		std::scoped_lock lock(decalLock);
		if (!decalGeometries.contains(a_pass->geometry))
			return false;
	}
	const auto& bound = a_pass->geometry->worldBound;
	if (!(OverlapsActivity(bound) || InBlanketRange(bound)))
		return false;

	ID3D11VertexShader* vertexShader = nullptr;
	globals::d3d::context->VSGetShader(&vertexShader, nullptr, nullptr);
	if (vertexShader)
		vertexShader->Release();
	const DecalShaders* shaders = vertexShader ? DecalShadersFor(vertexShader) : nullptr;
	if (!shaders) {
		debug.layoutRejects++;
		return false;
	}
	if (!PrepareCamera(DrawKind::Decal, false)) {
		debug.skippedDraws++;
		return false;
	}
	allowedShaderCount = 0;
	allowedShaders[allowedShaderCount++] = vertexShader;
	activeHS = shaders->hs.get();
	activeDS = shaders->ds.get();
	debug.decalDraws++;
	return true;
}

const SnowDeformation::DecalShaders* SnowDeformation::DecalShadersFor(ID3D11VertexShader* a_bound)
{
	const RE::BSGraphics::VertexShader* source = nullptr;
	auto vanilla = *globals::game::currentVertexShader;
	if (vanilla && reinterpret_cast<ID3D11VertexShader*>(vanilla->shader) == a_bound) {
		source = vanilla;
	} else if (auto state = globals::state; state->currentShader) {
		auto replacement = globals::shaderCache->GetVertexShader(*state->currentShader, state->modifiedVertexDescriptor);
		if (replacement && reinterpret_cast<ID3D11VertexShader*>(replacement->shader) == a_bound)
			source = replacement;
	}
	if (!source || source->byteCodeSize < 20)
		return nullptr;

	uint64_t checksum[2] = {};
	std::memcpy(checksum, source->rawBytecode + 4, sizeof(checksum));
	const uint64_t key = checksum[0] ^ (checksum[1] * 0x9E3779B97F4A7C15ull);
	auto found = decalShaders.find(key);
	if (found != decalShaders.end())
		return found->second.failed ? nullptr : &found->second;

	auto& entry = decalShaders[key];
	std::vector<ParsedElement> elements;
	std::string layout, fields, world;
	if (!ParseLayout(source->rawBytecode, source->byteCodeSize, elements) || !GenerateDecalLayout(elements, layout, fields, world)) {
		entry.failed = true;
		logger::info("[SnowDeformation] Decal vertex shader {:X} has no usable output layout; its decals stay undisplaced", source->id);
		return nullptr;
	}
	if (world.empty())
		world = " ";
	const std::vector<std::pair<const char*, const char*>> defines{ { "SD_LAYOUT", layout.c_str() }, { "SD_FIELDS", fields.c_str() }, { "SD_WORLD", world.c_str() } };
	entry.hs.attach(static_cast<ID3D11HullShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowDeformationHS.hlsl", defines, "hs_5_0")));
	entry.ds.attach(static_cast<ID3D11DomainShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowDeformationDecalDS.hlsl", defines, "ds_5_0")));
	entry.failed = !entry.hs || !entry.ds;
	if (entry.failed) {
		entry.hs = nullptr;
		entry.ds = nullptr;
		logger::error("[SnowDeformation] Decal displacement shaders failed to compile for vertex shader {:X}", source->id);
		return nullptr;
	}
	return &entry;
}

bool SnowDeformation::IsDrawVertexShaderValid() const
{
	ID3D11VertexShader* bound = nullptr;
	globals::d3d::context->VSGetShader(&bound, nullptr, nullptr);
	if (bound)
		bound->Release();
	if (!bound)
		return false;
	for (uint32_t i = 0; i < allowedShaderCount; ++i) {
		if (allowedShaders[i] == bound)
			return true;
	}
	return false;
}

bool SnowDeformation::IsLightingBlocked(const RE::BSGeometry* a_geometry) const
{
	const auto found = lightingRejected.find(a_geometry);
	return found != lightingRejected.end() && frameIndex < found->second;
}

void SnowDeformation::BlockLighting(const RE::BSGeometry* a_geometry)
{
	if (lightingRejected.size() > 1024) {
		for (auto it = lightingRejected.begin(); it != lightingRejected.end();)
			it = frameIndex >= it->second ? lightingRejected.erase(it) : std::next(it);
	}
	lightingRejected[a_geometry] = frameIndex + 120;
}

void SnowDeformation::ApplyDrawState()
{
	if (!drawActive)
		return;

	auto context = globals::d3d::context;
	if (globals::game::shadowState->GetRuntimeData().topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST || !IsDrawVertexShaderValid()) {
		debug.layoutRejects++;
		if (activeKind == DrawKind::Depth)
			prepassRejected.insert(activeGeometry);
		else if (activeKind == DrawKind::Lighting)
			BlockLighting(activeGeometry);
		EndLandscapeDraw();
		return;
	}
	if (activeKind == DrawKind::Depth)
		prepassRouted.insert(activeGeometry);

	ID3D11Buffer* constants = drawConstants->CB();
	ID3D11ShaderResourceView* blanket = blanketTexture ? blanketTexture->srv.get() : nullptr;
	ID3D11ShaderResourceView* hullResources[5] = { nullptr, levels[0].activityTexture->srv.get(), blanket, nullptr, levels[1].activityTexture->srv.get() };
	ID3D11ShaderResourceView* domainResources[5] = { levels[0].texture->srv.get(), nullptr, blanket, levels[1].texture->srv.get(), nullptr };
	ID3D11SamplerState* sampler = fieldSampler.get();

	context->HSSetConstantBuffers(0, 1, &constants);
	context->HSSetShaderResources(0, 5, hullResources);
	context->HSSetSamplers(0, 1, &sampler);
	context->DSSetConstantBuffers(0, 1, &constants);
	context->DSSetShaderResources(0, 5, domainResources);
	context->DSSetSamplers(0, 1, &sampler);
	context->HSSetShader(activeHS, nullptr, 0);
	context->DSSetShader(activeDS, nullptr, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
	stagesBound = true;
}

void SnowDeformation::EndLandscapeDraw()
{
	if (!drawActive)
		return;
	drawActive = false;
	activeGeometry = nullptr;
	activeHS = nullptr;
	activeDS = nullptr;
	if (!stagesBound)
		return;
	stagesBound = false;

	auto context = globals::d3d::context;
	ID3D11ShaderResourceView* nullViews[5] = {};
	ID3D11Buffer* nullBuffers[1] = {};
	ID3D11SamplerState* nullSampler = nullptr;
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->HSSetShaderResources(0, 5, nullViews);
	context->DSSetShaderResources(0, 5, nullViews);
	context->HSSetConstantBuffers(0, 1, nullBuffers);
	context->DSSetConstantBuffers(0, 1, nullBuffers);
	context->HSSetSamplers(0, 1, &nullSampler);
	context->DSSetSamplers(0, 1, &nullSampler);
	context->IASetPrimitiveTopology(globals::game::shadowState->GetRuntimeData().topology);
}

void SnowDeformation::UpdateFieldOnce()
{
	if (fieldUpdated)
		return;
	fieldUpdated = true;
	++frameIndex;
	UpdateField();
}

void SnowDeformation::EarlyPrepass()
{
	UpdateFieldOnce();
}

void SnowDeformation::Prepass()
{
	auto state = globals::state;
	if (!windowValid || state->isMapMenuOpen || !state->sharedDataCB || !state->featureDataCB)
		return;

	auto snapshot = [](ID3D11Buffer* a_source, winrt::com_ptr<ID3D11Buffer>& a_target, const char* a_name) {
		D3D11_BUFFER_DESC desc{};
		a_source->GetDesc(&desc);
		if (a_target) {
			D3D11_BUFFER_DESC existing{};
			a_target->GetDesc(&existing);
			if (existing.ByteWidth != desc.ByteWidth)
				a_target = nullptr;
		}
		if (!a_target) {
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = 0;
			desc.StructureByteStride = 0;
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, a_target.put())))
				return false;
			Util::SetResourceName(a_target.get(), a_name);
		}
		globals::d3d::context->CopyResource(a_target.get(), a_source);
		return true;
	};

	snapshotValid = snapshot(state->sharedDataCB->CB(), sharedSnapshot, "SnowDeformation::SharedDataSnapshot") &&
	                snapshot(state->featureDataCB->CB(), featureSnapshot, "SnowDeformation::FeatureDataSnapshot");
	snapshotEye = Util::GetEyePosition();
}

void SnowDeformation::GameLoaded()
{
	loadClearField.store(true, std::memory_order_relaxed);
	loadClearMotion.store(true, std::memory_order_relaxed);
}

void SnowDeformation::Reset()
{
	EndLandscapeDraw();
	inZPrepass = false;
	fieldUpdated = false;
}

void SnowDeformation::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	func(This, Pass, RenderFlags);
	globals::features::snowDeformation.BeginLandscapeDraw(Pass, DrawKind::Lighting);
}

void SnowDeformation::Hooks::BSLightingShader_RestoreGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::snowDeformation.EndLandscapeDraw();
	func(This, Pass, RenderFlags);
}

void SnowDeformation::Hooks::BSUtilityShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	func(This, Pass, RenderFlags);
	auto& singleton = globals::features::snowDeformation;
	singleton.BeginLandscapeDraw(Pass, singleton.inZPrepass ? DrawKind::Depth : DrawKind::Shadow);
}

void SnowDeformation::Hooks::BSUtilityShader_RestoreGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::snowDeformation.EndLandscapeDraw();
	func(This, Pass, RenderFlags);
}

void SnowDeformation::Hooks::Main_RenderDepth::thunk(bool a1, bool a2)
{
	auto& singleton = globals::features::snowDeformation;
	singleton.EndLandscapeDraw();
	singleton.prepassRouted.clear();
	singleton.prepassRejected.clear();
	singleton.mainCamera.valid = false;
	singleton.inZPrepass = true;
	func(a1, a2);
	singleton.inZPrepass = false;
	singleton.EndLandscapeDraw();
}

void SnowDeformation::Hooks::Main_Update::thunk()
{
	func();
	globals::features::snowDeformation.GatherStamps();
}

template <class T>
void SnowDeformation::Hooks::Projectile_AddImpact<T>::thunk(RE::Projectile* a_projectile, RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_position, const RE::NiPoint3& a_velocity, RE::hkpCollidable* a_collidable, int32_t a_arg6, uint32_t a_arg7)
{
	auto& singleton = globals::features::snowDeformation;
	if (a_projectile && singleton.gatherEnabled.load(std::memory_order_relaxed)) {
		auto spell = a_projectile->GetProjectileRuntimeData().spell;
		uint8_t element = 0;
		if (ClassifyElement(spell, element) && !a_projectile->GetProjectileRuntimeData().explosion) {
			ImpactEvent event{};
			event.kind = kImpactProjectile;
			event.element = element;
			event.voice = spell->GetSpellType() == RE::MagicSystem::SpellType::kVoicePower;
			event.position = a_position;
			event.direction = a_velocity;
			event.source = a_projectile->GetFormID();
			event.space = SpaceOf(a_projectile);
			singleton.QueueImpact(event);
		}
	}
	func(a_projectile, a_ref, a_position, a_velocity, a_collidable, a_arg6, a_arg7);
}

void SnowDeformation::Hooks::Explosion_Initialize::thunk(RE::Explosion* a_explosion)
{
	func(a_explosion);
	auto& singleton = globals::features::snowDeformation;
	if (!a_explosion || !singleton.gatherEnabled.load(std::memory_order_relaxed))
		return;
	auto object = a_explosion->GetBaseObject();
	auto base = object ? object->As<RE::BGSExplosion>() : nullptr;
	if (!base)
		return;
	const auto& data = a_explosion->GetExplosionRuntimeData();
	uint8_t element = 0;
	RE::MagicItem* magic = data.magicCaster ? data.magicCaster->currentSpell : nullptr;
	if (!ClassifyElement(magic, element) && base->data.damage <= 0.0f && base->data.force <= 0.0f)
		return;
	ImpactEvent event{};
	event.kind = kImpactExplosion;
	event.element = element;
	event.position = a_explosion->GetPosition();
	event.direction = RE::NiPoint3(-data.negativeVelocity.x, -data.negativeVelocity.y, -data.negativeVelocity.z);
	event.source = a_explosion->GetFormID();
	event.space = SpaceOf(a_explosion);
	event.radius = data.radius > 0.0f ? data.radius : base->data.radius;
	singleton.QueueImpact(event);
}

void SnowDeformation::Hooks::BSTempEffectGeometryDecal_Initialize::thunk(RE::BSTempEffectGeometryDecal* a_decal)
{
	func(a_decal);
	globals::features::snowDeformation.RegisterDecal(a_decal);
}

void SnowDeformation::RegisterDecal(RE::BSTempEffectGeometryDecal* a_decal)
{
	if (!a_decal || !a_decal->decal || !loaded || !settings.Enabled || !IsLandscapeGeometry(a_decal->attachedGeometry.get()))
		return;
	std::scoped_lock lock(decalLock);
	if (decalGeometries.size() < 4096)
		decalGeometries[a_decal->decal.get()] = a_decal->decal;
}

RE::BSEventNotifyControl SnowDeformation::SpellCastSink::ProcessEvent(const RE::TESSpellCastEvent* a_event, RE::BSTEventSource<RE::TESSpellCastEvent>*)
{
	auto& singleton = globals::features::snowDeformation;
	if (!a_event || !a_event->object || !singleton.gatherEnabled.load(std::memory_order_relaxed))
		return RE::BSEventNotifyControl::kContinue;
	auto spell = RE::TESForm::LookupByID<RE::SpellItem>(a_event->spell);
	uint8_t element = 0;
	if (!GroundShout(spell) || !ClassifyElement(spell, element))
		return RE::BSEventNotifyControl::kContinue;
	auto ref = a_event->object.get();
	const float yaw = ref->GetAngleZ();
	const float pitch = ref->GetAngleX();
	ImpactEvent event{};
	event.kind = kImpactShout;
	event.element = element;
	event.position = ref->GetPosition() + RE::NiPoint3(std::sin(yaw) * 40.0f, std::cos(yaw) * 40.0f, 60.0f);
	event.direction = RE::NiPoint3(std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch));
	event.source = ref->GetFormID();
	event.space = SpaceOf(ref);
	singleton.QueueImpact(event);
	return RE::BSEventNotifyControl::kContinue;
}

void SnowDeformation::Hooks::Install()
{
	stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	stl::write_vfunc<0x7, BSLightingShader_RestoreGeometry>(RE::VTABLE_BSLightingShader[0]);
	stl::write_vfunc<0x6, BSUtilityShader_SetupGeometry>(RE::VTABLE_BSUtilityShader[0]);
	stl::write_vfunc<0x7, BSUtilityShader_RestoreGeometry>(RE::VTABLE_BSUtilityShader[0]);
	stl::detour_thunk<Main_RenderDepth>(REL::RelocationID(100421, 107139));
	stl::write_vfunc<0xBD, Projectile_AddImpact<RE::MissileProjectile>>(RE::VTABLE_MissileProjectile[0]);
	stl::write_vfunc<0xBD, Projectile_AddImpact<RE::BeamProjectile>>(RE::VTABLE_BeamProjectile[0]);
	stl::write_vfunc<0xBD, Projectile_AddImpact<RE::FlameProjectile>>(RE::VTABLE_FlameProjectile[0]);
	stl::write_vfunc<0xBD, Projectile_AddImpact<RE::ConeProjectile>>(RE::VTABLE_ConeProjectile[0]);
	stl::write_vfunc<0xBD, Projectile_AddImpact<RE::GrenadeProjectile>>(RE::VTABLE_GrenadeProjectile[0]);
	stl::write_vfunc<0xA2, Explosion_Initialize>(RE::VTABLE_Explosion[0]);
	stl::write_vfunc<0x25, BSTempEffectGeometryDecal_Initialize>(RE::VTABLE_BSTempEffectGeometryDecal[0]);

	constexpr std::size_t kTrampolineHeadroom = 3 * 14;
	auto& singleton = globals::features::snowDeformation;
	if (SKSE::GetTrampoline().free_size() >= kTrampolineHeadroom) {
		stl::write_thunk_call<Main_Update>(REL::RelocationID(35565, 36564).address() + Util::VersionedRelocation::Select(0x748, 0xC26, 0xC38));
		singleton.gatherOnUpdate = true;
	} else {
		logger::warn("[SnowDeformation] Trampoline has {} bytes free; gathering footprints on the render thread instead", SKSE::GetTrampoline().free_size());
	}
	logger::info("[SnowDeformation] Installed hooks");
}

void SnowDeformation::PostPostLoad()
{
	if (REL::Module::IsVR()) {
		logger::info("[SnowDeformation] Not supported in VR, hooks not installed");
		return;
	}
	Hooks::Install();
}

void SnowDeformation::DataLoaded()
{
	if (REL::Module::IsVR())
		return;
	if (auto source = RE::ScriptEventSourceHolder::GetSingleton()) {
		source->AddEventSink<RE::TESSpellCastEvent>(&spellCastSink);
		source->AddEventSink<RE::TESEquipEvent>(&equipSink);
	}
}

void SnowDeformation::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	ImGui::Checkbox(T(TKEY("include_npcs"), "Include NPCs and Creatures"), &settings.IncludeNPCs);
	ImGui::Checkbox(T(TKEY("include_objects"), "Include Dropped Items"), &settings.IncludeObjects);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("include_objects_tooltip"), "Weapons, armour, loot, arrows and other items resting on the snow press into it too, and dig themselves out of the snow blanket."));

	{
		Util::DisableGuard disable(!globals::features::snowCover.loaded);
		ImGui::Checkbox(T(TKEY("use_snow_cover"), "Press Into Snow Cover Snow"), &settings.UseSnowCover);
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("use_snow_cover_tooltip"), "Ground that Snow Cover paints white takes prints too, not only vanilla snow textures."));

	ImGui::Checkbox(T(TKEY("displace_shadows"), "Displace Shadows"), &settings.DisplaceShadows);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("displace_shadows_tooltip"), "Also press prints into the sun shadow map, so prints are not shadowed by the flat snow surface above them."));

	{
		Util::DisableGuard blanketDisable(settings.BlanketHeight > 0.0f);
		ImGui::Checkbox(T(TKEY("sink_actors"), "Sink Into Prints"), &settings.SinkActors);
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("sink_actors_tooltip"), "Without a snow blanket, lowers characters standing in prints by the print depth so they do not float above them."));

	ImGui::SliderFloat(T(TKEY("blanket_height"), "Snow Blanket Height"), &settings.BlanketHeight, 0.0f, kMaxBlanketHeight, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("blanket_height_tooltip"), "Raises snowy ground by this many units so footprints become trenches down to the real ground. Ground under roofs and trees stays low. 0 turns it off."));
	ImGui::SliderFloat(T(TKEY("depth"), "Print Depth"), &settings.Depth, 1.0f, 20.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("depth_tooltip"), "How deep a foot presses into the snow, in game units."));
	ImGui::SliderFloat(T(TKEY("lifetime"), "Lifetime"), &settings.Lifetime, 10.0f, 1800.0f, "%.0f s", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("lifetime_tooltip"), "Seconds until a print has filled back in to a tenth of its depth. Prints fill faster while it is snowing."));
	ImGui::SliderFloat(T(TKEY("triangle_size"), "Triangle Size"), &settings.TrianglePixels, 2.0f, 32.0f, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("triangle_size_tooltip"), "On-screen size of the triangles generated over prints, in pixels. Lower is sharper and costs more."));

	if (ImGui::TreeNodeEx(T(TKEY("debug"), "Debug"))) {
		ImGui::Text("%s: %u", T(TKEY("debug_stamps"), "Stamps last frame"), debugShown.stamps);
		ImGui::Text("%s: %u / %u", T(TKEY("debug_tiles"), "Active / updated tiles"), debugShown.activeTiles, debugShown.dispatchedTiles);
		ImGui::Text("%s: %u / %u / %u / %u", T(TKEY("debug_draws"), "Displaced draws (depth / lighting / shadow / decal)"), debugShown.depthDraws, debugShown.lightingDraws, debugShown.shadowDraws, debugShown.decalDraws);
		ImGui::Text("%s: %u / %u", T(TKEY("debug_rejects"), "Draws skipped (layout / camera)"), debugShown.layoutRejects, debugShown.skippedDraws);
		ImGui::Text("%s: %s", T(TKEY("debug_snow_cover"), "Snow Cover gating"), debugShown.snowCover ? T("common.yes", "Yes") : T("common.no", "No"));
		ImGui::Text("%s: %s", T(TKEY("debug_layout"), "Landscape shader layout"),
			lightingLayoutSeen ? T(TKEY("debug_layout_valid"), "Valid") : T(TKEY("debug_layout_unknown"), "Not seen yet"));
		ImGui::Text("%s: %zu", T(TKEY("debug_blocked"), "Landscape pieces recently blocked after a mismatch"), lightingRejected.size());
		ImGui::Text("%s: %u (%.1f)", T(TKEY("debug_sunk"), "Actors sunk into prints (deepest)"), sunkActors, deepestSink);
		ImGui::Text("%s: %s (%u / %d)", T(TKEY("debug_blanket"), "Snow blanket (land texels ready)"), blanketActive ? T("common.yes", "Yes") : T("common.no", "No"), blanketReady,
			kBlanketTexels * kBlanketTexels);
		if (ImGui::Button(T(TKEY("clear_prints"), "Clear Prints")))
			ClearField();
		ImGui::TreePop();
	}
}

void SnowDeformation::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	auto sanitize = [](float& a_value, float a_default, float a_min, float a_max) {
		a_value = std::clamp(std::isfinite(a_value) ? a_value : a_default, a_min, a_max);
	};
	sanitize(settings.BlanketHeight, defaults.BlanketHeight, 0.0f, kMaxBlanketHeight);
	sanitize(settings.Depth, defaults.Depth, 1.0f, 20.0f);
	sanitize(settings.Lifetime, defaults.Lifetime, 10.0f, 1800.0f);
	sanitize(settings.TrianglePixels, defaults.TrianglePixels, 2.0f, 32.0f);
}

void SnowDeformation::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SnowDeformation::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
