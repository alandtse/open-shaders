#include "Game.h"

#include <atomic>
#include <limits>
#include <mutex>

#include "Features/SkySync.h"
#include "Globals.h"
#include "State.h"
#include "StringUtils.h"

#define TDM_API_COMMONLIB
#include "TDM/TrueDirectionalMovementAPI.h"

namespace
{
	TDM_API::IVTDM2* targetLockAPI = nullptr;
	constexpr float kScreenProjectionEpsilon = 1e-5f;

	std::atomic_bool celestialTransitionHandlerAvailable{ false };
	std::atomic_bool timeJumpTransitionRequested{ false };
	std::atomic_bool gameLoadTransitionRequested{ false };
	std::atomic_uint32_t completedCelestialTransitionGeneration{ 0 };

	void ResetModelHandle(RE::ModelDBHandle& a_handle)
	{
		if (!a_handle)
			return;

		if (REL::Module::IsAE()) {
			auto* entry = a_handle.get();
			a_handle = {};
			static REL::Relocation<void (*)(RE::ModelDBHandle::U_Entry*)> release{ REL::ID(15443) };
			release(entry);
		} else {
			using Reset = RE::ModelDBHandle* (*)(RE::ModelDBHandle*, RE::ModelDBHandle::U_Entry*);
			static REL::Relocation<Reset> reset{ REL::ID(25746) };
			reset(&a_handle, nullptr);
		}
	}

	void MarkCelestialTransitionComplete()
	{
		completedCelestialTransitionGeneration.fetch_add(1, std::memory_order_release);
	}

	void RequestCelestialTransition(std::atomic_bool& a_request)
	{
		a_request.store(true, std::memory_order_release);
		if (!celestialTransitionHandlerAvailable.load(std::memory_order_acquire) &&
			a_request.exchange(false, std::memory_order_acq_rel)) {
			MarkCelestialTransitionComplete();
		}
	}
}

namespace Util
{
	void ForceWeather(RE::Sky* a_sky, RE::TESWeather* a_weather, bool a_override)
	{
		if (!a_sky)
			return;

		a_sky->ForceWeather(a_weather, a_override);
		if (a_sky->auroraRoot) {
			if (a_sky->root)
				a_sky->root->DetachChild(a_sky->auroraRoot.get());
			a_sky->auroraRoot.reset();
		}
		ResetModelHandle(a_sky->auroraModel);

		// Defer cloud-pass rebuilding until accumulation; render queues may still hold the current passes.
		if (a_sky->clouds) {
			for (const auto& cloud : a_sky->clouds->clouds) {
				if (cloud) {
					if (auto* property = skyrim_cast<RE::BSSkyShaderProperty*>(cloud->GetGeometryRuntimeData().shaderProperty.get()))
						property->lastRenderPassState = (std::numeric_limits<std::int32_t>::max)();
				}
			}
		}
	}

	void SetCelestialTransitionHandlerAvailable(bool a_available)
	{
		celestialTransitionHandlerAvailable.store(a_available, std::memory_order_release);
		if (a_available)
			return;

		// Bitwise or, not ||: both requests must be drained even when the first one was already pending.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbitwise-instead-of-logical"
		const bool hadPendingTransition = timeJumpTransitionRequested.exchange(false, std::memory_order_acq_rel) |
		                                  gameLoadTransitionRequested.exchange(false, std::memory_order_acq_rel);
#pragma clang diagnostic pop
		if (hadPendingTransition)
			MarkCelestialTransitionComplete();
	}

	void RequestTimeJumpTransition()
	{
		RequestCelestialTransition(timeJumpTransitionRequested);
	}

	void RequestGameLoadTransition()
	{
		RequestCelestialTransition(gameLoadTransitionRequested);
	}

	CelestialTransitionRequest ConsumeCelestialTransitionRequest()
	{
		if (!celestialTransitionHandlerAvailable.load(std::memory_order_acquire))
			return {};

		return {
			.timeJump = timeJumpTransitionRequested.exchange(false, std::memory_order_acq_rel),
			.gameLoad = gameLoadTransitionRequested.exchange(false, std::memory_order_acq_rel),
		};
	}

	void CompleteCelestialTransition()
	{
		MarkCelestialTransitionComplete();
	}

	std::uint32_t GetCompletedCelestialTransitionGeneration()
	{
		return completedCelestialTransitionGeneration.load(std::memory_order_acquire);
	}

	void StoreTransform3x4NoScale(DirectX::XMFLOAT3X4& Dest, const RE::NiTransform& Source)
	{
		//
		// Shove a Matrix3+Point3 directly into a float[3][4] with no modifications
		//
		// Dest[0][#] = Source.m_Rotate.m_pEntry[0][#];
		// Dest[0][3] = Source.m_Translate.x;
		// Dest[1][#] = Source.m_Rotate.m_pEntry[1][#];
		// Dest[1][3] = Source.m_Translate.x;
		// Dest[2][#] = Source.m_Rotate.m_pEntry[2][#];
		// Dest[2][3] = Source.m_Translate.x;
		//
		static_assert(sizeof(RE::NiTransform::rotate) == 3 * 3 * sizeof(float));  // NiMatrix3
		static_assert(sizeof(RE::NiTransform::translate) == 3 * sizeof(float));   // NiPoint3
		static_assert(offsetof(RE::NiTransform, translate) > offsetof(RE::NiTransform, rotate));

		_mm_store_ps(Dest.m[0], _mm_loadu_ps(Source.rotate.entry[0]));
		_mm_store_ps(Dest.m[1], _mm_loadu_ps(Source.rotate.entry[1]));
		_mm_store_ps(Dest.m[2], _mm_loadu_ps(Source.rotate.entry[2]));
		Dest.m[0][3] = Source.translate.x;
		Dest.m[1][3] = Source.translate.y;
		Dest.m[2][3] = Source.translate.z;
	}

	float4 TryGetWaterData(float offsetX, float offsetY)
	{
		if (globals::game::shadowState) {
			if (auto tes = globals::game::tes) {
				auto position = GetEyePosition(0);
				position.x += offsetX;
				position.y += offsetY;
				if (auto cell = tes->GetCell(position)) {
					float4 data = float4(1.0f, 1.0f, 1.0f, -FLT_MAX);

					bool extraCellWater = false;

					if (auto extraCellWaterType = cell->extraList.GetByType<RE::ExtraCellWaterType>()) {
						if (auto water = extraCellWaterType->water) {
							{
								data = float4(float(water->data.deepWaterColor.red) + float(water->data.shallowWaterColor.red),
									float(water->data.deepWaterColor.green) + float(water->data.shallowWaterColor.green),
									float(water->data.deepWaterColor.blue) + float(water->data.shallowWaterColor.blue),
									0.0f);

								data.x /= 255.0f;
								data.y /= 255.0f;
								data.z /= 255.0f;

								data.x *= 0.5;
								data.y *= 0.5;
								data.z *= 0.5;
								extraCellWater = true;
							}
						}
					}

					if (!extraCellWater) {
						if (auto worldSpace = tes->GetRuntimeData2().worldSpace) {
							if (auto water = worldSpace->worldWater) {
								data = float4(float(water->data.deepWaterColor.red) + float(water->data.shallowWaterColor.red),
									float(water->data.deepWaterColor.green) + float(water->data.shallowWaterColor.green),
									float(water->data.deepWaterColor.blue) + float(water->data.shallowWaterColor.blue),
									0.0f);

								data.x /= 255.0f;
								data.y /= 255.0f;
								data.z /= 255.0f;

								data.x *= 0.5;
								data.y *= 0.5;
								data.z *= 0.5;
							}
						}
					}

					if (auto sky = globals::game::sky) {
						const auto& color = sky->skyColor[RE::TESWeather::ColorTypes::kWaterMultiplier];
						data.x *= color.red;
						data.y *= color.green;
						data.z *= color.blue;
					}

					data.w = cell->GetExteriorWaterHeight() - position.z;

					return data;
				}
			}
		}
		return float4(1.0f, 1.0f, 1.0f, -FLT_MAX);
	}

	void RequestTargetLockAPI()
	{
		targetLockAPI = reinterpret_cast<TDM_API::IVTDM2*>(TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V2));
	}

	RE::ActorHandle GetTargetLockTarget()
	{
		return targetLockAPI ? targetLockAPI->GetCurrentTarget() : RE::ActorHandle{};
	}

	RE::ObjectRefHandle GetDialogueTarget()
	{
		const auto* topicManager = globals::game::menuTopicManager;
		if (!topicManager)
			return {};
		return topicManager->speaker ? topicManager->speaker : topicManager->lastSpeaker;
	}

	RE::NiPointer<RE::TESObjectREFR> GetSelectedConsoleReference()
	{
		return RE::Console::GetSelectedRef();
	}

	float GetCameraAspectRatio()
	{
		const auto* graphicsState = globals::game::graphicsState;
		if (!graphicsState || graphicsState->screenWidth <= 0 || graphicsState->screenHeight <= 0)
			return kFallbackCameraAspect;
		const float eyeScale = globals::game::isVR ? 0.5f : 1.0f;
		return eyeScale * static_cast<float>(graphicsState->screenWidth) / static_cast<float>(graphicsState->screenHeight);
	}

	float* GetWorldFOV()
	{
		const auto playerCamera = globals::game::playerCamera;
		return playerCamera && !globals::game::isVR ? &playerCamera->GetRuntimeData2().worldFOV : nullptr;
	}

	// Thanks Ershin!
	RE::NiPoint3 GetCameraPosition()
	{
		auto player = globals::game::player;
		auto playerCamera = globals::game::playerCamera;
		RE::NiPoint3 ret{};
		if (!playerCamera || !player)
			return ret;

		// VR changes both the camera-state layout and indices; mixing runtimes reads the wrong camera.
		const auto isVehicleOrBodyCamera = [&](const auto& runtimeData, RE::CameraState thirdPersonState, RE::CameraState mountState) {
			return playerCamera->currentState == runtimeData.cameraStates[RE::CameraStates::kFirstPerson] ||
			       playerCamera->currentState == runtimeData.cameraStates[thirdPersonState] ||
			       playerCamera->currentState == runtimeData.cameraStates[mountState];
		};
		if (globals::game::isVR ?
				isVehicleOrBodyCamera(*playerCamera->GetVRRuntimeData(), RE::CameraStates::kVRThirdPerson, RE::CameraStates::kVRMount) :
				isVehicleOrBodyCamera(playerCamera->GetRuntimeData(), RE::CameraStates::kThirdPerson, RE::CameraStates::kMount)) {
			RE::NiNode* root = playerCamera->cameraRoot.get();
			if (root) {
				ret.x = root->world.translate.x;
				ret.y = root->world.translate.y;
				ret.z = root->world.translate.z;
			}
		} else if (playerCamera->IsInFreeCameraMode()) {
			auto freeCameraState = static_cast<RE::FreeCameraState*>(playerCamera->currentState.get());
			ret = freeCameraState->translation;
		} else {
			RE::NiPoint3 playerPos = player->GetLookingAtLocation();

			ret.z = playerPos.z;
			ret.x = player->GetPositionX();
			ret.y = player->GetPositionY();
		}

		return ret;
	}

	RE::NiPoint3 GetReferenceFocusPosition(RE::TESObjectREFR* a_ref)
	{
		RE::NiPoint3 targetPosition = a_ref->GetPosition();
		if (a_ref->GetFormType() == RE::FormType::ActorCharacter) {
			auto head = a_ref->GetNodeByName("NPC Head [Head]");
			if (head)
				targetPosition = head->world.translate;
		}
		return targetPosition;
	}

	float GetCameraDistanceToReference(RE::TESObjectREFR* a_ref)
	{
		auto* camera = RE::Main::WorldRootCamera();
		RE::NiPoint3 cameraPosition = camera ? camera->world.translate : GetCameraPosition();
		return cameraPosition.GetDistance(GetReferenceFocusPosition(a_ref));
	}

	bool GetReferenceFocusCoord(RE::TESObjectREFR* a_ref, float2& a_focusCoord)
	{
		auto* camera = RE::Main::WorldRootCamera();
		if (!camera || globals::game::isVR)
			return false;

		float screenX = 0.0f;
		float screenY = 0.0f;
		float screenZ = 0.0f;
		if (!camera->WorldPtToScreenPt3(GetReferenceFocusPosition(a_ref), screenX, screenY, screenZ, kScreenProjectionEpsilon) ||
			!std::isfinite(screenX) || !std::isfinite(screenY) || screenZ <= 0.0f ||
			screenX < 0.0f || screenX > 1.0f || screenY < 0.0f || screenY > 1.0f) {
			return false;
		}

		// Engine projection uses bottom-left coordinates; depth sampling uses top-left.
		a_focusCoord = float2(screenX, 1.0f - screenY);
		return true;
	}

	RE::NiPoint3 GetAverageEyePosition()
	{
		auto shadowState = globals::game::shadowState;
		if (!globals::game::isVR)
			return shadowState->GetRuntimeData().posAdjust.getEye();
		return (shadowState->GetVRRuntimeData().posAdjust.getEye(0) + shadowState->GetVRRuntimeData().posAdjust.getEye(1)) * 0.5f;
	}

	RE::NiPoint3 GetEyePosition(int eyeIndex)
	{
		auto shadowState = globals::game::shadowState;
		if (!globals::game::isVR)
			return shadowState->GetRuntimeData().posAdjust.getEye();
		return shadowState->GetVRRuntimeData().posAdjust.getEye(eyeIndex);
	}

	RE::BSGraphics::ViewData GetCameraData(int eyeIndex)
	{
		auto shadowState = globals::game::shadowState;
		if (!globals::game::isVR) {
			return shadowState->GetRuntimeData().cameraData.getEye();
		}
		return shadowState->GetVRRuntimeData().cameraData.getEye(eyeIndex);
	}

	float4 GetCameraData()
	{
		const float cameraNear = *globals::game::cameraNear;
		const float cameraFar = *globals::game::cameraFar;

		float4 cameraData{};
		cameraData.x = cameraFar;
		cameraData.y = cameraNear;
		cameraData.z = cameraFar - cameraNear;
		cameraData.w = cameraFar * cameraNear;

		return cameraData;
	}

	bool GetTemporal()
	{
		auto* imageSpaceManager = globals::game::imageSpaceManager;
		if (!imageSpaceManager)
			return false;
		// VR keeps its own ISTemporalAA instance at a different offset.
		GET_INSTANCE_MEMBER_VRPTR(BSImagespaceShaderISTemporalAA, imageSpaceManager);
		return BSImagespaceShaderISTemporalAA && BSImagespaceShaderISTemporalAA->taaEnabled;
	}

	void SetTemporal(bool enabled)
	{
		auto* imageSpaceManager = globals::game::imageSpaceManager;
		if (!imageSpaceManager)
			return;
		GET_INSTANCE_MEMBER_VRPTR(BSImagespaceShaderISTemporalAA, imageSpaceManager);
		if (BSImagespaceShaderISTemporalAA)
			BSImagespaceShaderISTemporalAA->taaEnabled = enabled;
	}

	void DisableVanillaTAA()
	{
		if (auto* setting = RE::GetINISetting("bUseTAA:Display"))
			setting->data.b = false;
	}

	float GetVerticalFOVRad()
	{
		static float& cameraFOVDeg = (*(float*)(REL::RelocationID(513786, 388785).address()));  // FOV degrees
		float hFOVRad = DirectX::XMConvertToRadians(cameraFOVDeg);
		float unitHalfWidth = tan(hFOVRad / 2);                                                                // This is same as camera frustum RL
		float unitHalfHeight = unitHalfWidth / (globals::state->screenSize.x / globals::state->screenSize.y);  // frustum TB
		float vFOVRad = 2.0f * atan(unitHalfHeight);
		return vFOVRad;
	}

	float2 ConvertToDynamic(float2 a_size, bool a_ignoreLock)
	{
		auto viewport = globals::game::graphicsState;
		auto& runtimeData = viewport->GetRuntimeData();

		if (runtimeData.dynamicResolutionLock && !a_ignoreLock)
			return a_size;

		return float2(
			a_size.x * runtimeData.dynamicResolutionWidthRatio,
			a_size.y * runtimeData.dynamicResolutionHeightRatio);
	}

	DispatchCount GetScreenDispatchCount(bool a_dynamic)
	{
		float2 resolution = globals::state->screenSize;

		// Feature passes use the scaled render area even when the vanilla dynamic-resolution lock is set.
		if (a_dynamic)
			resolution = ConvertToDynamic(resolution, true);

		uint dispatchX = (uint)std::ceil(resolution.x / 8.0f);
		uint dispatchY = (uint)std::ceil(resolution.y / 8.0f);

		return { dispatchX, dispatchY };
	}

	bool IsDynamicResolution()
	{
		const static auto address = REL::RelocationID{ 508794, 380760 }.address();
		bool* bDynamicResolution = reinterpret_cast<bool*>(address);
		return *bDynamicResolution;
	}

	std::string FormatTESForm(const RE::TESForm* form)
	{
		if (!form) {
			return "nullptr";
		}

		// Get name and editor ID
		const char* rawName = form->GetName();
		const char* rawEditorID = form->GetFormEditorID();

		std::string name;
		std::string editorID = rawEditorID ? rawEditorID : "Unknown";

		// Check if name exists and is not just whitespace
		if (rawName && strlen(rawName) > 0) {
			std::string tempName(rawName);
			// Check if name is only whitespace
			bool isOnlyWhitespace = std::all_of(tempName.begin(), tempName.end(),
				[](unsigned char c) { return std::isspace(c); });

			if (!isOnlyWhitespace) {
				name = tempName;
			}
		}
		// Format the FormID part once
		std::string formIDStr = " - 0x" + std::format("{:08X}", form->GetFormID());

		// If no valid name, use editor ID as name and don't show it twice
		if (name.empty()) {
			return editorID + formIDStr;
		} else {
			return name + " " + editorID + formIDStr;
		}
	}
	std::string FormatWeather(const RE::TESWeather* weather)
	{
		if (!weather) {
			return "nullptr";
		}

		std::string baseFormat = FormatTESForm(weather);

		// Get all flag names for this weather using magic_enum
		std::vector<std::string> flagNames;
		uint32_t flags = weather->data.flags.underlying();

		if (flags == 0) {
			flagNames.push_back("None");
		} else {
			// Use magic_enum to iterate through all weather flags
			for (auto flagValue : magic_enum::enum_values<RE::TESWeather::WeatherDataFlag>()) {
				if (flagValue != RE::TESWeather::WeatherDataFlag::kNone &&
					weather->data.flags.any(flagValue)) {
					// Convert enum name to human-readable format
					std::string flagName = std::string(magic_enum::enum_name(flagValue));

					// Remove 'k' prefix and convert to readable format
					if (flagName.starts_with("k")) {
						flagName = flagName.substr(1);
					}

					// Convert specific cases to more readable names
					if (flagName == "PermAurora") {
						flagName = "Aurora";
					} else if (flagName == "AuroraFollowsSun") {
						flagName = "Aurora Sun";
					}

					flagNames.push_back(flagName);
				}
			}

			// Check for any unknown flags (flags not covered by the enum)
			uint32_t knownFlags = 0;
			for (auto flagValue : magic_enum::enum_values<RE::TESWeather::WeatherDataFlag>()) {
				if (flagValue != RE::TESWeather::WeatherDataFlag::kNone) {
					knownFlags |= static_cast<uint32_t>(flagValue);
				}
			}

			uint32_t unknownFlags = flags & ~knownFlags;
			if (unknownFlags != 0) {
				flagNames.push_back("Unknown(" + std::to_string(unknownFlags) + ")");
			}
		}

		// Join flag names with commas
		std::string flagsStr;
		for (size_t i = 0; i < flagNames.size(); ++i) {
			if (i > 0) {
				flagsStr += ", ";
			}
			flagsStr += flagNames[i];
		}

		return baseFormat + " [" + flagsStr + "]";
	}

	bool FrameChecker::IsNewFrame()
	{
		return IsNewFrame(globals::state->frameCount);
	}

	bool GraphSampleGate::ShouldSample()
	{
		return ShouldSample(globals::state->frameCount);
	}

	RE::BGSTextureSet* GetSeasonalSwap(RE::BGSTextureSet* textureSet)
	{
		if (textureSet == nullptr) {
			return nullptr;
		}

		if (textureSet->pad12C > 0) {
			if (auto* form = RE::TESForm::LookupByID<RE::BGSTextureSet>(textureSet->pad12C)) {
				return form;
			}
		}

		return textureSet;
	}

	bool IsInterior()
	{
		auto tes = RE::TES::GetSingleton();
		if (tes && !tes->interiorCell) {
			if (auto worldSpace = tes->GetRuntimeData2().worldSpace) {
				if (!worldSpace->flags.any(RE::TESWorldSpace::Flag::kNoSky, RE::TESWorldSpace::Flag::kFixedDimensions)) {
					return false;
				}
			}
		}
		return true;
	}

	std::string_view GetCurrentWorldspaceOrCellEditorID()
	{
		auto* tes = globals::game::tes;
		if (!tes)
			return {};
		const char* name = nullptr;
		if (tes->interiorCell)
			name = tes->interiorCell->GetFormEditorID();
		else if (auto* worldspace = tes->GetRuntimeData2().worldSpace)
			name = worldspace->GetFormEditorID();
		return name ? name : "";
	}

	bool IsObjectLODTree(const char* name, const RE::BSShaderProperty* property)
	{
		if (!name)
			return false;
		std::string_view geometryName(name);
		constexpr std::string_view treePassThrough = "treepassthru";
		if (geometryName.size() >= treePassThrough.size() && IEquals(geometryName.substr(0, treePassThrough.size()), treePassThrough))
			return true;
		if (!property || !property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided))
			return false;
		constexpr std::string_view largeRef = "-LargeRef";
		if (geometryName.size() > largeRef.size() && IEquals(geometryName.substr(geometryName.size() - largeRef.size()), largeRef))
			geometryName.remove_suffix(largeRef.size());
		for (const std::string_view category : { "obj"sv, "objHD"sv, "objpassthru"sv })
			if (IEquals(geometryName, category))
				return true;
		return false;
	}

	bool IsHarvestableFlora(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref)
			return false;
		auto base = a_ref->GetObjectReference();
		if (!base)
			return false;
		switch (base->GetFormType()) {
		case RE::FormType::Flora:
			return true;
		case RE::FormType::Tree:
			return static_cast<RE::TESObjectTREE*>(base)->produceItem != nullptr;
		default:
			return false;
		}
	}

	RE::NiBound MergeBounds(const RE::NiBound& a_lhs, const RE::NiBound& a_rhs)
	{
		const RE::NiPoint3 delta = a_rhs.center - a_lhs.center;
		const float distance = delta.Length();
		if (distance + a_rhs.radius <= a_lhs.radius)
			return a_lhs;
		if (distance + a_lhs.radius <= a_rhs.radius)
			return a_rhs;
		RE::NiBound combined;
		combined.radius = 0.5f * (distance + a_lhs.radius + a_rhs.radius);
		combined.center = a_lhs.center + delta * ((combined.radius - a_lhs.radius) / distance);
		return combined;
	}

	float GetFractionalGameMonth(const RE::Calendar& calendar)
	{
		const auto month = std::min(calendar.GetMonth(), static_cast<uint32_t>(std::size(RE::Calendar::DAYS_IN_MONTH) - 1));
		return static_cast<float>(month) + (calendar.GetDay() - 1.0f + calendar.GetHour() / EnvironmentControls::kHoursPerDay) / RE::Calendar::DAYS_IN_MONTH[month];
	}

	float GetPrecipitationIntensity(const RE::TESWeather* weather)
	{
		if (auto* precipitation = weather ? weather->precipitationData : nullptr) {
			using DataID = RE::BGSShaderParticleGeometryData::DataID;
			const float intensity = precipitation->GetSettingValue(DataID::kParticleDensity).f * precipitation->GetSettingValue(DataID::kGravityVelocity).f;
			return std::isfinite(intensity) ? intensity : 0.0f;
		}
		return 0.0f;
	}

	bool IsArchitecturalModel(const RE::TESModel* model)
	{
		if (!model || !model->GetModel())
			return false;
		auto path = ToLowerAscii(model->GetModel());
		std::replace(path.begin(), path.end(), '/', '\\');
		return path.find("architecture\\") != std::string::npos || path.find("dungeons\\") != std::string::npos;
	}

	bool IsPlacedFireCandidate(RE::TESBoundObject* a_base)
	{
		if (!a_base)
			return false;
		switch (a_base->GetFormType()) {
		case RE::FormType::Static:
		case RE::FormType::MovableStatic:
		case RE::FormType::Light:
		case RE::FormType::Activator:
		case RE::FormType::TalkingActivator:
		case RE::FormType::Furniture:
			return true;
		default:
			return false;
		}
	}

	bool IsFireGeometry(RE::BSGeometry* a_geometry)
	{
		auto& data = a_geometry->GetGeometryRuntimeData();
		auto* property = data.shaderProperty.get();
		auto* alpha = data.alphaProperty.get();
		if (!property || !alpha || property->GetRTTI() != globals::rtti::BSEffectShaderPropertyRTTI.get())
			return false;
		if (alpha->GetDestBlendMode() != RE::NiAlphaProperty::AlphaFunction::kOne)
			return false;
		auto* material = static_cast<RE::BSEffectShaderMaterial*>(property->material);
		if (!material)
			return false;

		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		if (property->flags.any(Flag::kSoftEffect))
			return !material->greyscaleTexturePath.empty() && property->flags.all(Flag::kGrayscaleToPaletteColor, Flag::kGrayscaleToPaletteAlpha);

		auto* particles = a_geometry->AsParticlesGeom();
		if (!particles || netimmerse_cast<RE::BSStripParticleSystem*>(a_geometry) || material->sourceTexturePath.empty())
			return false;
		auto* particleData = particles->GetParticlesRuntimeData().particleData.get();
		return particleData && particleData->GetParticlesRuntimeData().subTextureOffsetsCount != 0;
	}

	void CollectFireGeometry(RE::NiAVObject* a_object, bool a_hidden, bool& a_hasFire, std::vector<RE::NiBound>& a_samples)
	{
		if (!a_object)
			return;
		const bool hidden = a_hidden || a_object->GetAppCulled();
		if (auto* geometry = a_object->AsGeometry()) {
			if (!IsFireGeometry(geometry))
				return;
			a_hasFire = true;
			if (!hidden && geometry->worldBound.radius > 0.0f)
				a_samples.push_back(geometry->worldBound);
			return;
		}
		if (auto* node = a_object->AsNode()) {
			for (auto& child : node->GetChildren())
				CollectFireGeometry(child.get(), hidden, a_hasFire, a_samples);
		}
	}

	void WorldToCell(const RE::NiPoint2& worldPos, int32_t& x, int32_t& y)
	{
		x = static_cast<int32_t>(floor(worldPos.x / 4096.0f));
		y = static_cast<int32_t>(floor(worldPos.y / 4096.0f));
	}

	void WorldToCell(const RE::NiPoint3& worldPos, int32_t& x, int32_t& y)
	{
		WorldToCell(RE::NiPoint2(worldPos.x, worldPos.y), x, y);
	}
	std::uintptr_t MainUpdateCallSite()
	{
		const std::uintptr_t aeOffset = REL::Module::IsAtLeast(REL::Version(1, 7, 99, 0)) ? 0xC38 : 0xC26;
		return REL::RelocationID(35565, 36564).address() + REL::Relocate<std::uintptr_t>(0x748, aeOffset, 0x7EE);
	}
}  // namespace Util

namespace Util::EnvironmentControls
{
	namespace
	{
		std::recursive_mutex environmentMutex;
		std::atomic<RE::TESWeather*> lockedWeather{ nullptr };
		std::atomic_bool weatherLockAvailable{ false };
		bool timePaused = false;
		float savedTimeScale = kDefaultTimeScale;
		bool timeRunningForMenu = false;
		bool restorePauseAfterMenu = false;
		constexpr float kHourLockTolerance = 0.001f;
		constexpr float kWeatherTransitionMidpoint = 0.5f;
		std::optional<RE::TESWeather*> scrubPreviousWeather;

		struct Preview
		{
			RE::TESWeather* weather = nullptr;
			std::optional<float> hour;
			RE::TESWeather* previousWeather = nullptr;
			bool previousTimePaused = false;
			float previousTimeScale = kDefaultTimeScale;
			float previousSavedTimeScale = kDefaultTimeScale;
		};
		std::optional<Preview> preview;

		RE::TESWeather* GetWeatherForTimeChange()
		{
			if (auto* weather = GetLockedWeather())
				return weather;
			auto* sky = globals::game::sky;
			return sky ? sky->currentWeather : nullptr;
		}

		void ApplyWeatherLock(RE::TESWeather* weather)
		{
			const auto* previous = lockedWeather.exchange(weather, std::memory_order_acq_rel);
			if (!weather && previous)
				if (auto* sky = globals::game::sky)
					sky->ReleaseWeatherOverride();
		}

		void PauseTimeInternal()
		{
			if (timePaused)
				return;
			if (auto* calendar = globals::game::calendar; calendar && calendar->timeScale) {
				savedTimeScale = calendar->timeScale->value;
				calendar->timeScale->value = 0.0f;
				timePaused = true;
			}
		}

		void ResumeTimeInternal()
		{
			if (!timePaused)
				return;
			if (auto* calendar = globals::game::calendar; calendar && calendar->timeScale) {
				calendar->timeScale->value = savedTimeScale;
				timePaused = false;
			}
		}

		void RestorePreviewTime(const Preview& previous)
		{
			savedTimeScale = previous.previousSavedTimeScale;
			timePaused = previous.previousTimePaused && !timeRunningForMenu;
			if (timeRunningForMenu)
				restorePauseAfterMenu = previous.previousTimePaused;
			if (auto* calendar = globals::game::calendar; calendar && calendar->timeScale) {
				calendar->timeScale->value = timeRunningForMenu && previous.previousTimeScale <= 0.0f ?
				                                 (savedTimeScale > 0.0f ? savedTimeScale : kDefaultTimeScale) :
				                                 previous.previousTimeScale;
			}
		}
	}

	void SetWeatherLockAvailable()
	{
		weatherLockAvailable.store(true, std::memory_order_release);
	}

	bool IsWeatherLockAvailable()
	{
		return weatherLockAvailable.load(std::memory_order_acquire);
	}

	RE::TESWeather* GetLockedWeather()
	{
		return lockedWeather.load(std::memory_order_acquire);
	}

	void MaintainLocks()
	{
		std::scoped_lock lock(environmentMutex);
		if (auto* weather = GetLockedWeather()) {
			if (auto* sky = globals::game::sky) {
				const bool releasePending = sky->flags.any(RE::Sky::Flags::kReleaseWeatherOverride);
				if (releasePending || sky->currentWeather != weather || sky->overrideWeather != weather) {
					sky->flags.reset(RE::Sky::Flags::kReleaseWeatherOverride);
					Util::ForceWeather(sky, weather, true);
				}
			}
		}
		if (!preview || !preview->hour || timeRunningForMenu)
			return;
		if (auto* calendar = globals::game::calendar; calendar && calendar->gameHour && calendar->timeScale) {
			calendar->timeScale->value = 0.0f;
			timePaused = true;
			if (std::abs(calendar->gameHour->value - *preview->hour) > kHourLockTolerance) {
				calendar->gameHour->value = *preview->hour;
				RequestTimeJumpTransition();
			}
		}
	}

	void SetLockedWeather(RE::TESWeather* weather)
	{
		std::scoped_lock lock(environmentMutex);
		EndGameHourScrub();
		StopPreview();
		ApplyWeatherLock(weather);
		MaintainLocks();
	}

	void ChangeWeather(RE::TESWeather* weather, bool instant)
	{
		std::scoped_lock lock(environmentMutex);
		auto* sky = globals::game::sky;
		if (!sky || !weather)
			return;
		EndGameHourScrub();
		const bool wasLocked = GetLockedWeather() != nullptr;
		StopPreview();
		if (wasLocked) {
			ApplyWeatherLock(weather);
			MaintainLocks();
		} else if (instant) {
			Util::ForceWeather(sky, weather, false);
		} else {
			sky->SetWeather(weather, true, false);
		}
	}

	void ResetWeather()
	{
		std::scoped_lock lock(environmentMutex);
		SetLockedWeather(nullptr);
		if (auto* sky = globals::game::sky)
			sky->ResetWeather();
	}

	void RefreshWeather(RE::TESWeather* weather)
	{
		std::scoped_lock lock(environmentMutex);
		if (auto* sky = globals::game::sky; sky && weather && sky->currentWeather == weather) {
			Util::ForceWeather(sky, weather, true);
			if (!GetLockedWeather())
				sky->ReleaseWeatherOverride();
			else
				MaintainLocks();
		}
	}

	void BeginGameHourScrub()
	{
		std::scoped_lock lock(environmentMutex);
		auto* calendar = globals::game::calendar;
		if (scrubPreviousWeather || timeRunningForMenu || !calendar || !calendar->gameHour)
			return;
		auto* weather = GetWeatherForTimeChange();
		if (auto* sky = globals::game::sky; sky && !GetLockedWeather() && sky->lastWeather &&
											(!weather || sky->currentWeatherPct <= kWeatherTransitionMidpoint))
			weather = sky->lastWeather;
		if (!weather || !globals::game::sky)
			return;
		scrubPreviousWeather = preview && preview->weather ? preview->previousWeather : GetLockedWeather();
		// Keep the preview weather through takeover, restoring its original lock when the slider releases.
		if (preview)
			preview->previousWeather = weather;
		StopPreview();
		ApplyWeatherLock(weather);
		MaintainLocks();
	}

	void EndGameHourScrub()
	{
		std::scoped_lock lock(environmentMutex);
		if (!scrubPreviousWeather)
			return;
		auto* previousWeather = *scrubPreviousWeather;
		scrubPreviousWeather.reset();
		ApplyWeatherLock(previousWeather);
		MaintainLocks();
	}

	bool SetGameHour(float hour, bool synchronize)
	{
		std::scoped_lock lock(environmentMutex);
		auto* calendar = globals::game::calendar;
		if (!calendar || !calendar->gameHour || !std::isfinite(hour) || hour < 0.0f || hour >= kHoursPerDay)
			return false;
		StopPreview();
		// Backward clock edits otherwise look like a midnight wrap and expire the weather override.
		if (auto* sky = globals::game::sky; sky && GetLockedWeather())
			sky->lastWeatherUpdate = hour;
		calendar->gameHour->value = hour;
		if (synchronize)
			RequestTimeJumpTransition();
		return true;
	}

	void SetTimeScale(float timeScale)
	{
		std::scoped_lock lock(environmentMutex);
		auto* calendar = globals::game::calendar;
		if (!calendar || !calendar->timeScale || !std::isfinite(timeScale) || timeScale < 0.0f)
			return;
		StopPreview();
		if (IsTimePaused())
			savedTimeScale = timeScale;
		else
			calendar->timeScale->value = timeScale;
	}

	bool IsTimePaused()
	{
		std::scoped_lock lock(environmentMutex);
		if (auto* calendar = globals::game::calendar; calendar && calendar->timeScale && calendar->timeScale->value > 0.0f)
			timePaused = false;
		return timePaused;
	}

	float GetSavedTimeScale()
	{
		std::scoped_lock lock(environmentMutex);
		return savedTimeScale;
	}

	void PauseTime()
	{
		std::scoped_lock lock(environmentMutex);
		StopPreview();
		if (timeRunningForMenu)
			restorePauseAfterMenu = true;
		else
			PauseTimeInternal();
	}

	void ResumeTime()
	{
		std::scoped_lock lock(environmentMutex);
		StopPreview();
		restorePauseAfterMenu = false;
		ResumeTimeInternal();
	}

	void ResetTimeScale()
	{
		std::scoped_lock lock(environmentMutex);
		SetTimeScale(kDefaultTimeScale);
	}

	void SetTimeRunningForMenu(bool needsRunningTime)
	{
		std::scoped_lock lock(environmentMutex);
		auto* calendar = globals::game::calendar;
		if (!calendar || !calendar->timeScale || timeRunningForMenu == needsRunningTime)
			return;
		timeRunningForMenu = needsRunningTime;
		if (needsRunningTime) {
			EndGameHourScrub();
			restorePauseAfterMenu = IsTimePaused();
			if (calendar->timeScale->value == 0.0f) {
				if (savedTimeScale <= 0.0f)
					savedTimeScale = kDefaultTimeScale;
				ResumeTimeInternal();
				if (calendar->timeScale->value == 0.0f)
					calendar->timeScale->value = savedTimeScale;
			}
		} else {
			if (restorePauseAfterMenu)
				PauseTimeInternal();
			restorePauseAfterMenu = false;
			MaintainLocks();
		}
	}

	bool StartPreview(RE::TESWeather* weather, std::optional<float> hour)
	{
		std::scoped_lock lock(environmentMutex);
		auto* calendar = globals::game::calendar;
		if (!weather && hour)
			weather = GetWeatherForTimeChange();
		if ((!weather && !hour) || timeRunningForMenu ||
			(weather && !globals::game::sky) ||
			(hour && (!std::isfinite(*hour) || *hour < 0.0f || *hour >= kHoursPerDay ||
						 !calendar || !calendar->gameHour || !calendar->timeScale)))
			return false;
		EndGameHourScrub();
		if (!preview)
			preview.emplace();
		if (weather && !preview->weather)
			preview->previousWeather = GetLockedWeather();
		if (hour && !preview->hour) {
			preview->previousTimePaused = IsTimePaused();
			preview->previousTimeScale = calendar->timeScale->value;
			preview->previousSavedTimeScale = savedTimeScale;
		} else if (!hour && preview->hour) {
			RestorePreviewTime(*preview);
		}
		preview->weather = weather;
		preview->hour = hour;
		if (weather)
			ApplyWeatherLock(weather);
		if (hour)
			PauseTimeInternal();
		MaintainLocks();
		return true;
	}

	void StopPreview()
	{
		std::scoped_lock lock(environmentMutex);
		if (!preview)
			return;
		const auto previous = *preview;
		preview.reset();
		if (previous.weather)
			ApplyWeatherLock(previous.previousWeather);
		if (previous.hour)
			RestorePreviewTime(previous);
		MaintainLocks();
	}

	bool IsPreviewActive()
	{
		std::scoped_lock lock(environmentMutex);
		return preview.has_value();
	}
}

float Util::GetSunVisibility()
{
	const auto sky = globals::game::sky;
	const auto sun = sky ? sky->sun : nullptr;
	if (!sun || !sun->root || !sun->sunBaseNode || !sun->sunBase)
		return 0.0f;
	if (sun->root->GetFlags().any(RE::NiAVObject::Flag::kHidden) || sun->sunBaseNode->GetFlags().any(RE::NiAVObject::Flag::kHidden))
		return 0.0f;

	const auto prop = skyrim_cast<RE::BSSkyShaderProperty*>(sun->sunBase->GetGeometryRuntimeData().shaderProperty.get());
	if (!prop)
		return 0.0f;

	const float alpha = prop->kBlendColor.alpha;
	return alpha > 0.0f ? std::min(alpha, 1.0f) : 0.0f;
}

RE::NiPoint3 Util::GetSunDirection()
{
	if (auto direction = globals::features::skySync.GetCelestialDirection(SkySync::Caster::Sun))
		return *direction;
	const auto sky = globals::game::sky;
	if (!sky || !sky->root || !sky->sun || !sky->sun->root)
		return { 0.0f, 0.0f, 1.0f };
	auto direction = sky->root->world.rotate * sky->sun->root->local.translate;
	if (direction.Unitize() <= FLT_EPSILON)
		return { 0.0f, 0.0f, 1.0f };
	return direction;
}

RE::NiPoint3 Util::GetMoonDirection(const RE::Moon* moon)
{
	const auto sky = globals::game::sky;
	if (!sky || !sky->root || !moon || !moon->root)
		return { 0.0f, 0.0f, 1.0f };
	if (moon == sky->masser || moon == sky->secunda) {
		const auto caster = moon == sky->masser ? SkySync::Caster::Masser : SkySync::Caster::Secunda;
		if (auto direction = globals::features::skySync.GetCelestialDirection(caster))
			return *direction;
	}
	auto direction = sky->root->world.rotate * Moon::GetFacingAxis(moon->root->local.rotate);
	if (direction.Unitize() <= FLT_EPSILON)
		return { 0.0f, 0.0f, 1.0f };
	return direction;
}
