#include "Wind.h"

#include "Trees/TreeWindPatcher.h"

#include <algorithm>

namespace
{
	uint32_t CopyTransientSources(std::span<const OpenWind::WindField::TransientWindSource> a_sources,
		std::span<WindField::TransientWindSource> a_destination)
	{
		uint32_t count = 0;
		for (const auto& source : a_sources) {
			if (source.type > OpenWind::WindField::TransientWindSourceType::DirectionalOvalWave) {
				static bool reported = false;
				if (!reported) {
					logger::warn("Open Wind supplied a transient shape this renderer does not support; that shape is omitted from vegetation.");
					reported = true;
				}
				continue;
			}
			if (count == a_destination.size())
				break;
			a_destination[count++] = {
				source.origin, source.wavefrontDistance, source.direction, source.strength,
				source.maxDistance, source.waveHalfWidth, source.propagationSpeed, source.coneSpreadTangent,
				source.decayTime, source.collisionRadius, source.sideSpreadTangent, source.sideStrength,
				static_cast<WindField::TransientWindSourceType>(source.type), source.verticalScale, source.falloffWidth, 0.0f
			};
		}
		std::fill(a_destination.begin() + count, a_destination.end(), WindField::TransientWindSource{});
		return count;
	}
}

void Wind::PostPostLoad()
{
	const auto module = GetModuleHandleW(L"OpenWind.dll");
	if (!module) {
		logger::info("Open Wind is not installed; using ordinary vegetation wind.");
		return;
	}
	const auto requestRenderer = reinterpret_cast<OpenWind::RequestRendererAPI>(GetProcAddress(module, "GetOpenWindRendererAPI"));
	const auto requestAPI = reinterpret_cast<OpenWind::RequestAPI>(GetProcAddress(module, "GetOpenWindAPI"));
	const auto requestUI = reinterpret_cast<OpenWind::RequestUIAPI>(GetProcAddress(module, "GetOpenWindUIAPI"));
	const auto renderer = requestRenderer ? requestRenderer(OpenWind::kRendererRevision) : nullptr;
	const auto sampler = requestAPI ? requestAPI(OpenWind::kAPIRevision) : nullptr;
	const auto ui = requestUI ? requestUI(OpenWind::kUIRevision) : nullptr;
	OpenWind::RendererHost host;
	host.context = this;
	host.authoring = &GetAuthoringHost();
	if (!renderer || renderer->structSize != sizeof(OpenWind::RendererAPI) ||
		renderer->frameSize != sizeof(OpenWind::RendererFrame) || !renderer->Bind || !renderer->Update ||
		!renderer->CopyFrame || !renderer->DataLoaded || !renderer->ResetScene || !renderer->SetSettings || !renderer->SetControls ||
		!sampler || sampler->structSize < sizeof(OpenWind::API) || sampler->sampleSize != sizeof(OpenWind::WindSample) ||
		!sampler->IsReady || !sampler->SampleWind || !renderer->Bind(&host)) {
		logger::error("Open Wind could not bind: required functions or data layouts do not match.");
		return;
	}
	provider = renderer;
	samplingAPI = sampler;
	providerFrame = std::make_unique<OpenWind::RendererFrame>();
	provider->CopyFrame(providerFrame.get());
	if (ui && ui->structSize >= sizeof(OpenWind::UIAPI) && ui->GetSettings && ui->LoadSettings &&
		ui->ImportSettings && ui->RestoreDefaults && ui->RunAction && ui->Command && ui->GetControls) {
		uiProvider = ui;
		if (pendingProviderSettings.is_object()) {
			const auto text = pendingProviderSettings.dump();
			uiProvider->ImportSettings(text.c_str());
		}
		uint32_t size = 0;
		if (uiProvider->GetControls("windEffects", nullptr, &size) && size) {
			std::string text(size, '\0');
			if (uiProvider->GetControls("windEffects", text.data(), &size) && text.back() == '\0')
				uiControlsDocument = json::parse(text.c_str(), nullptr, false);
		}
		if (!uiControlsDocument.is_object() || !uiControlsDocument.contains("tabs") || !uiControlsDocument["tabs"].is_array()) {
			uiControlsDocument = nullptr;
			logger::error("Open Wind returned an invalid effect control document.");
		}
		RefreshProviderSettings();
	} else {
		logger::error("Open Wind's settings UI is unavailable; wind rendering remains active.");
	}
	CommitProviderSettings();
	TreeWindPatcher::LoadAndInstall();
}

void Wind::DataLoaded()
{
	if (provider)
		provider->DataLoaded();
}

void Wind::OnSceneTransitionReset(bool a_opening)
{
	if (provider)
		provider->ResetScene(a_opening);
	ResetSpringHistory();
}

void Wind::ResetSpringHistory()
{
	grassState.springInitialized.fill(false);
	grassState.springFieldAvailable.fill(false);
	treeState.springInitialized.fill(false);
	treeState.springFieldAvailable.fill(false);
	treeState.hasUpdated = false;
}

void Wind::ApplyProviderFrame(const OpenWind::RendererFrame& a_frame)
{
	if (providerResetId != a_frame.resetId) {
		providerResetId = a_frame.resetId;
		ResetSpringHistory();
	}
	windFieldTuning.gustScale = a_frame.tuning.gustScale;
	windFieldTuning.frontAspectRatio = a_frame.tuning.frontAspectRatio;
	windFieldTuning.gustAdvectionBaseSpeed = a_frame.tuning.gustAdvectionBaseSpeed;
	windFieldTuning.gustAdvectionMultiplier = a_frame.tuning.gustAdvectionMultiplier;
	windFieldTuning.detailScaleRatio = a_frame.tuning.detailScaleRatio;
	windFieldTuning.detailCrosswindScaleRatio = a_frame.tuning.detailCrosswindScaleRatio;
	windFieldTuning.turbulenceStrength = a_frame.tuning.turbulenceStrength;
	windFieldTuning.turbulenceSkew = a_frame.tuning.turbulenceSkew;
	windFieldTuning.contrastLow = a_frame.tuning.contrastLow;
	windFieldTuning.contrastHigh = a_frame.tuning.contrastHigh;
	windFieldTuning.gustAmplitude = a_frame.tuning.gustAmplitude;
	windFieldTuning.broadGustSeed = a_frame.tuning.broadGustSeed;
	windFieldTuning.turbulentGustSeed = a_frame.tuning.turbulentGustSeed;
	windFieldTuning.gradientSeedMix = a_frame.tuning.gradientSeedMix;
	windFieldTuning.pcgMultiplier = a_frame.tuning.pcgMultiplier;
	windFieldTuning.pcgIncrement = a_frame.tuning.pcgIncrement;
	windFieldTuning.distortionStrength = a_frame.tuning.distortionStrength;
	windFieldTuning.distortionScale = a_frame.tuning.distortionScale;
	windFieldTuning.distortionSpeed = a_frame.tuning.distortionSpeed;
	windFieldCurrent = { a_frame.current.direction, a_frame.current.travelDistance, a_frame.current.crosswind, a_frame.current.speed };
	previousWindFieldCurrent = { a_frame.previous.direction, a_frame.previous.travelDistance, a_frame.previous.crosswind, a_frame.previous.speed };
	windFieldTransition = { a_frame.transition.direction, a_frame.transition.travelDistance, a_frame.transition.crosswind, a_frame.transition.speed };
	previousWindFieldTransition = { a_frame.previousTransition.direction, a_frame.previousTransition.travelDistance, a_frame.previousTransition.crosswind, a_frame.previousTransition.speed };
	ambientWindVelocity = a_frame.ambientVelocity;
	windFieldSelectedVelocity = a_frame.selectedVelocity;
	previousWindFieldSelectedVelocity = a_frame.previousSelectedVelocity;
	windFieldFrameTime = a_frame.frameTime;
	windFieldAmbientSpeed = a_frame.ambientSpeed;
	windFieldAdvectionSpeed = a_frame.advectionSpeed;
	windFieldGustTravelDistance = a_frame.gustTravelDistance;
	previousWindFieldGustTravelDistance = a_frame.previousGustTravelDistance;
	windFieldTravelDelta = a_frame.travelDelta;
	windFieldTransitionBlend = a_frame.transitionBlend;
	previousWindFieldTransitionBlend = a_frame.previousTransitionBlend;
	windFieldSelectedSpeed = a_frame.selectedSpeed;
	windFieldTransitionActive = a_frame.initialized != 0 && a_frame.transitionBlend < 1.0f;
	activeTransientWindImpulseCount = CopyTransientSources(
		std::span{ a_frame.transients }.first(a_frame.transientCount), transientWindImpulses);
	previousActiveTransientWindImpulseCount = CopyTransientSources(
		std::span{ a_frame.previousTransients }.first(a_frame.previousTransientCount), previousTransientWindImpulses);
}

void Wind::Reset()
{
	if (!provider)
		return;
	CommitProviderSettings();
	if (provider->Update(providerFrame.get()) &&
		providerFrame->transientCount <= providerFrame->transients.size() &&
		providerFrame->previousTransientCount <= providerFrame->previousTransients.size())
		ApplyProviderFrame(*providerFrame);
}

void Wind::CommitProviderSettings()
{
	if (!provider || !providerFrame)
		return;
	auto updated = providerFrame->settings;
	updated.enableTrunkBend = settings.enableTrunkBend;
	updated.overrideTrunkWindIntensity = settings.overrideTrunkWindIntensity;
	updated.trunkWindIntensityOverride = settings.trunkWindIntensityOverride;
	updated.trunkWindBendSensitivity = settings.trunkWindBendSensitivity;
	updated.treeLeafBaseWindFlutterGain = settings.treeLeafBaseWindFlutterGain;
	updated.treeWindGustScale = settings.treeWindGustScale;
	updated.treeWindGustSoftLimit = settings.treeWindGustSoftLimit;
	updated.treeWindSpringFrequency = settings.treeWindSpringFrequency;
	updated.treeWindSpringDamping = settings.treeWindSpringDamping;
	updated.treeTransientSpringFrequency = settings.treeTransientSpringFrequency;
	updated.treeTransientSpringDamping = settings.treeTransientSpringDamping;
	updated.windFieldGustScale = settings.windFieldGustScale;
	updated.windFieldGustCrosswindScale = settings.windFieldGustCrosswindScale;
	updated.windFieldGustAmplitude = settings.windFieldGustAmplitude;
	updated.windFieldGustAdvectionMultiplier = settings.windFieldGustAdvectionMultiplier;
	updated.windFieldGustAdvectionResponse = settings.windFieldGustAdvectionResponse;
	updated.windFieldGustCoverage = settings.windFieldGustCoverage;
	updated.windFieldGustEdgeSoftness = settings.windFieldGustEdgeSoftness;
	updated.enableWindFieldGustDistortion = settings.enableWindFieldGustDistortion;
	updated.windFieldGustDistortionStrength = settings.windFieldGustDistortionStrength;
	updated.windFieldGustDistortionScale = settings.windFieldGustDistortionScale;
	updated.windFieldGustDistortionSpeed = settings.windFieldGustDistortionSpeed;
	updated.windFieldDirectionTransitionDuration = settings.windFieldDirectionTransitionDuration;
	updated.processMidRangeTransients = settings.processMidRangeTransients;
	updated.processFarRangeTransients = settings.processFarRangeTransients;
	updated.grassTransientBendStrength = settings.grassTransientBendStrength;
	updated.grassTransientFlutterHalfLife = settings.grassTransientFlutterHalfLife;
	updated.grassTransientFlutterStrength = settings.grassTransientFlutterStrength;
	updated.grassTransientFlutterFrequency = settings.grassTransientFlutterFrequency;
	updated.enableAmbientGrassWind = settings.enableAmbientGrassWind;
	updated.enableGrassWindSpring = settings.enableGrassWindSpring;
	updated.enableGrassWindSpringBend = settings.enableGrassWindSpringBend;
	updated.grassWindResponse = settings.grassWindResponse;
	updated.grassWindSensitivity = settings.grassWindSensitivity;
	updated.grassWindMaximumTilt = settings.grassWindMaximumTilt;
	updated.grassWindCompressionToBend = settings.grassWindCompressionToBend;
	updated.grassWindSpringFrequency = settings.grassWindSpringFrequency;
	updated.grassWindSpringDamping = settings.grassWindSpringDamping;
	updated.grassWindFlutterStrength = settings.grassWindFlutterStrength;
	updated.grassWindFlutterFrequency = settings.grassWindFlutterFrequency;
	updated.grassWindFlutterGustInfluence = settings.grassWindFlutterGustInfluence;
	updated.grassWindFlutterWaveScale = settings.grassWindFlutterWaveScale;
	updated.grassWindFlutterAmplitudeResponse = settings.grassWindFlutterAmplitudeResponse;
	for (size_t index = 0; index < settings.grassWindSpringQuality.size(); ++index) {
		updated.grassWindSpringQuality[index].textureSize = settings.grassWindSpringQuality[index].textureSize;
		updated.grassWindSpringQuality[index].maxDistance = settings.grassWindSpringQuality[index].maxDistance;
	}
	provider->SetSettings(&updated, sizeof(updated));
	OpenWind::WindRuntimeState controls;
	controls.visualizeWindField = runtimeState.visualizeWindField;
	controls.windFieldUseRealSpeed = runtimeState.windFieldUseRealSpeed;
	controls.windFieldUseRealDirection = runtimeState.windFieldUseRealDirection;
	controls.windFieldOverrideSpeed = runtimeState.windFieldOverrideSpeed;
	controls.windFieldPendingDirectionDegrees = runtimeState.windFieldPendingDirectionDegrees;
	controls.windFieldAppliedDirectionDegrees = runtimeState.windFieldAppliedDirectionDegrees;
	controls.windFieldDebugView = static_cast<OpenWind::WindFieldDebugView>(runtimeState.windFieldDebugView);
	controls.treeWindTest = { runtimeState.treeWindTest.enabled, runtimeState.treeWindTest.speed,
		runtimeState.treeWindTest.gustScale, runtimeState.treeWindTest.gustAmplitude, runtimeState.treeWindTest.gustAdvectionMultiplier };
	provider->SetControls(&controls, sizeof(controls));
}

bool Wind::ReadProviderSettings(json& a_document) const
{
	uint32_t size = 0;
	if (!uiProvider || !uiProvider->GetSettings(nullptr, &size) || !size)
		return false;
	std::string text(size, '\0');
	if (!uiProvider->GetSettings(text.data(), &size) || text.back() != '\0')
		return false;
	a_document = json::parse(text.c_str(), nullptr, false);
	return a_document.is_object();
}

void Wind::RefreshProviderSettings()
{
	json document;
	if (ReadProviderSettings(document))
		uiSettingsDocument = std::move(document);
	if (provider && providerFrame)
		provider->CopyFrame(providerFrame.get());
}

bool Wind::RunProviderAction(const json& a_action)
{
	if (!uiProvider)
		return false;
	const auto text = a_action.dump();
	return uiProvider->RunAction(text.c_str());
}

json Wind::RunProviderCommand(const json& a_request) const
{
	json response{ { "success", false }, { "error", "Open Wind authoring is unavailable." } };
	if (!uiProvider)
		return response;
	const auto request = a_request.dump();
	const auto receive = [](void* context, const char* text) noexcept {
		try {
			*static_cast<json*>(context) = json::parse(text, nullptr, false);
		} catch (...) {
		}
	};
	if (!uiProvider->Command(request.c_str(), &response, receive) || !response.is_object())
		return { { "success", false }, { "error", "Open Wind did not return a valid authoring response." } };
	return response;
}

bool Wind::SamplePublishedWind(std::span<const float3> a_worldPositions,
	std::span<PublishedWindSample> a_samples) const noexcept
{
	const auto* sampler = samplingAPI.load();
	if (!sampler || a_worldPositions.size() != a_samples.size() || a_samples.size() > sampler->maximumBatchSize)
		return false;
	try {
		thread_local std::vector<OpenWind::Vector3> positions;
		thread_local std::vector<OpenWind::WindSample> samples;
		positions.resize(a_worldPositions.size());
		samples.resize(a_samples.size());
		for (size_t index = 0; index < positions.size(); ++index)
			positions[index] = { a_worldPositions[index].x, a_worldPositions[index].y, a_worldPositions[index].z };
		if (!sampler->SampleWind(positions.data(), samples.data(), static_cast<uint32_t>(samples.size())))
			return false;
		const auto vector = [](const OpenWind::Vector3& value) { return float3{ value.x, value.y, value.z }; };
		for (size_t index = 0; index < samples.size(); ++index) {
			const auto& sample = samples[index];
			a_samples[index] = { vector(sample.baseVelocity), vector(sample.gustVelocity),
				vector(sample.transientVelocity), vector(sample.finalVelocity), vector(sample.windExcludingHavokImpulses),
				sample.ambientGust, sample.transientIntensity, sample.frameId };
		}
		return true;
	} catch (...) {
		return false;
	}
}
