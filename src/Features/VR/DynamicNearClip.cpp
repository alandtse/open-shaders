#include "DynamicNearClip.h"

#include "Features/ScreenSpaceGI.h"
#include "Features/VR.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "NearClipProjection.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/PerfUtils.h"

namespace
{
	constexpr float kMaximumSampleAge = 0.2f;
	constexpr float kCameraGap = 0.5f;
	constexpr float kLogInterval = 2.0f;

	struct PrepareWorldCamera
	{
		static void thunk(float fov)
		{
			func(fov);
			globals::features::vr.dynamicNearClip.BeforeCameraUpdate();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct RenderWorldDepth
	{
		static void thunk(bool a1, bool a2)
		{
			auto& nearClip = globals::features::vr.dynamicNearClip;
			nearClip.BeginWorldDepth();
			func(a1, a2);
			nearClip.FinishWorldDepth();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct CopyWorldDepth
	{
		static void thunk(ID3D11DeviceContext* context, ID3D11Resource* destination, ID3D11Resource* source)
		{
			globals::features::vr.dynamicNearClip.FinishWorldDepth();
			context->CopyResource(destination, source);
		}
	};

	bool ApproximatelyEqual(float a, float b)
	{
		return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= std::max(0.0001f, std::abs(b) * 0.001f);
	}

}

void VRDynamicNearClip::Install()
{
	if (!globals::game::isVR || REL::Module::get().version() != SKSE::RUNTIME_VR_1_4_15)
		return;
	const auto call = REL::Offset(0x5B9F38).address();
	if (!REL::verify_code(call, REL::make_pattern<"E8 93 78 D6 00">())) {
		Fail("Camera preparation call changed; dynamic near clip was not installed");
		return;
	}
	stl::write_thunk_call<PrepareWorldCamera>(call);
	hookReady = true;
	logger::info("VR dynamic near clip: installed before eye frustum construction and world culling");
	const auto depthCopy = REL::Offset(0x13235BD).address();
	if (!REL::verify_code(depthCopy, REL::make_pattern<"41 FF 91 78 01 00 00">()) ||
		!REL::verify_code(REL::Offset(0x1349770).address(), REL::make_pattern<"33 C0 48 89 05 DF CA 18 02 89 05 D5 CA 18 02 48 89 05 DA 9A 39 02 C3">())) {
		logger::warn("VR dynamic near clip: world depth signatures changed; prepass capture unavailable");
		return;
	}
	if (!REL::safe_fill(depthCopy + 5, REL::NOP, 2, REL::make_pattern<"00 00">())) {
		logger::warn("VR dynamic near clip: world depth patch changed; prepass capture unavailable");
		return;
	}
	SKSE::GetTrampoline().write_call<5>(depthCopy, CopyWorldDepth::thunk);
	stl::write_thunk_call<RenderWorldDepth>(REL::Offset(0x5B961E).address());
}

void VRDynamicNearClip::Fail(const char* reason)
{
	if (!failed)
		logger::warn("VR dynamic near clip: {}", reason);
	failed = true;
	status = reason;
}

void VRDynamicNearClip::WaitForCamera(const char* reason)
{
	RestoreCamera();
	status = reason;
	const auto now = Util::GetNowSecs();
	if (now - lastLog >= kLogInterval) {
		logger::info("VR dynamic near clip: {}; source near L/R={:.6g}/{:.6g}, far L/R={:.6g}/{:.6g}, minimum={:.6g}, ratio={:.6g}",
			reason, inputNear[0], inputNear[1], inputFar[0], inputFar[1], inputMinimumNear, inputFarNearRatio);
		lastLog = now;
	}
}

void VRDynamicNearClip::RestoreCamera()
{
	if (!controlledCamera)
		return;
	auto& camera = controlledCamera->GetVRRuntimeData();
	if (camera.viewFrustumArray && camera.viewFrustumBuffer) {
		for (uint32_t eye = 0; eye < std::min(camera.unk1C8, 2u); ++eye) {
			if (ApproximatelyEqual(camera.viewFrustumArray[eye].fNear, appliedNear))
				camera.viewFrustumArray[eye].fNear = savedNear[eye];
		}
		if (ApproximatelyEqual(camera.viewFrustumBuffer->fNear, appliedNear))
			camera.viewFrustumBuffer->fNear = savedBufferNear;
	}
	auto& projection = controlledCamera->GetRuntimeData2();
	projection.minNearPlaneDist = savedMinimum;
	projection.maxFarNearRatio = savedRatio;
	controlledCamera.reset();
	appliedNear = 0.0f;
	for (auto& readback : readbacks)
		readback.pending = false;
	lastSample = {};
	ssgiReleaseResetPending = false;
	updateFrame = UINT32_MAX;
}

void VRDynamicNearClip::ReadDepth(double now)
{
	for (auto& readback : readbacks) {
		if (!readback.pending)
			continue;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		const auto result = globals::d3d::context->Map(readback.buffer.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
		if (result == DXGI_ERROR_WAS_STILL_DRAWING)
			continue;
		readback.pending = false;
		if (FAILED(result)) {
			Fail("Depth readback failed; restoring the engine near plane");
			return;
		}
		std::array<EyeDepth, 2> sample;
		std::memcpy(sample.data(), mapped.pData, sizeof(sample));
		globals::d3d::context->Unmap(readback.buffer.get(), 0);
		if (readback.serial <= acceptedSerial || now - readback.captured > kMaximumSampleAge)
			continue;
		acceptedSerial = readback.serial;
		for (uint32_t eye = 0; eye < 2; ++eye) {
			absoluteNearest[eye] = sample[eye].validSamples && std::isfinite(sample[eye].absoluteNearest) && sample[eye].absoluteNearest > 0.0f ? sample[eye].absoluteNearest : std::numeric_limits<float>::infinity();
			nearest[eye] = sample[eye].validSamples && std::isfinite(sample[eye].relevantNearest) && sample[eye].relevantNearest > 0.0f ? sample[eye].relevantNearest : std::numeric_limits<float>::infinity();
		}
		lastSample = readback.captured;
	}
}

void VRDynamicNearClip::BeforeCameraUpdate()
{
	settings.ClampNearClipSettings();
	UpdateVanillaFogOverride();
	const bool menuScene = globals::state->IsFullScreenMenuOpen() || globals::state->IsMainOrLoadingMenuOpen(globals::game::ui);
	if (!hookReady || !resourcesReady || failed || !globals::features::vr.loaded || !settings.DynamicNearClip || menuScene) {
		RestoreCamera();
		if (!failed)
			status = menuScene ? "Menu: engine near plane" : "Inactive";
		return;
	}
	auto* camera = RE::Main::WorldRootCamera();
	if (!camera) {
		inputNear = {};
		inputFar = {};
		WaitForCamera("Waiting for the world camera");
		return;
	}
	auto& cameraData = camera->GetVRRuntimeData();
	if (cameraData.unk1C8 != 2 || !cameraData.viewFrustumArray || !cameraData.viewFrustumBuffer) {
		inputNear = {};
		inputFar = {};
		WaitForCamera("Waiting for both eye frustums");
		return;
	}
	const auto& projection = camera->GetRuntimeData2();
	inputMinimumNear = projection.minNearPlaneDist;
	inputFarNearRatio = projection.maxFarNearRatio;
	for (uint32_t eye = 0; eye < 2; ++eye) {
		inputNear[eye] = cameraData.viewFrustumArray[eye].fNear;
		inputFar[eye] = cameraData.viewFrustumArray[eye].fFar;
	}
	// Camera preparation also runs while scene/menu cameras are being initialized.
	if (!std::isfinite(projection.minNearPlaneDist) || projection.minNearPlaneDist < 0.0f ||
		!std::isfinite(projection.maxFarNearRatio) || projection.maxFarNearRatio <= 0.0f) {
		WaitForCamera("Waiting for valid camera projection limits");
		return;
	}
	for (uint32_t eye = 0; eye < 2; ++eye) {
		const auto& frustum = cameraData.viewFrustumArray[eye];
		if (frustum.bOrtho || !std::isfinite(frustum.fNear) || frustum.fNear <= 0.0f ||
			!std::isfinite(frustum.fFar) || frustum.fFar <= std::max(frustum.fNear, settings.NormalNearClip)) {
			WaitForCamera("Waiting for a valid perspective camera frustum");
			return;
		}
	}
	const bool fixedNear = ApproximatelyEqual(settings.MinimumNearClip, settings.NormalNearClip);
	const auto& sourceNear = controlledCamera ? savedNear : inputNear;
	if (fixedNear && ApproximatelyEqual(sourceNear[0], settings.NormalNearClip) && ApproximatelyEqual(sourceNear[1], settings.NormalNearClip)) {
		RestoreCamera();
		targetNear = settings.NormalNearClip;
		status = "Requested near matches engine: no override";
		return;
	}
	const auto now = Util::GetNowSecs();
	if (controlledCamera.get() != camera) {
		RestoreCamera();
		controlledCamera.reset(camera);
		for (uint32_t eye = 0; eye < 2; ++eye)
			savedNear[eye] = cameraData.viewFrustumArray[eye].fNear;
		savedBufferNear = cameraData.viewFrustumBuffer->fNear;
		savedMinimum = projection.minNearPlaneDist;
		savedRatio = projection.maxFarNearRatio;
		controller.Reset(settings);
		lastUpdate = now;
		appliedNear = 0.0f;
		logger::info("VR dynamic near clip: camera ready; source near L/R={:.6g}/{:.6g}, far L/R={:.6g}/{:.6g}, starting near={:.6g}",
			inputNear[0], inputNear[1], inputFar[0], inputFar[1], controller.current);
	}
	if (updateFrame != globals::state->frameCount) {
		float elapsed = static_cast<float>(now - lastUpdate);
		if (elapsed > kCameraGap) {
			controller.Reset(settings);
			ssgiReleaseResetPending = false;
			lastSample = {};
			for (auto& readback : readbacks)
				readback.pending = false;
		}
		ReadDepth(now);
		if (failed) {
			RestoreCamera();
			return;
		}
		bool resetSSGIHistory = false;
		const float age = static_cast<float>(now - lastSample);
		const bool fresh = lastSample > 0.0 && age <= kMaximumSampleAge;
		const float distance = std::min(nearest[0], nearest[1]);
		targetNear = std::isfinite(distance) ? std::clamp(distance * settings.NearDistanceScale, settings.MinimumNearClip, settings.NormalNearClip) : settings.NormalNearClip;
		const float previousNear = controller.current;
		controller.Update(targetNear, elapsed, fresh, settings);
		if (controller.current < previousNear * 0.9f) {
			resetSSGIHistory = true;
			ssgiReleaseResetPending = true;
		} else if (ssgiReleaseResetPending && targetNear > previousNear * (1.0f + VRNearClipController::kHysteresis)) {
			resetSSGIHistory = true;
			ssgiReleaseResetPending = false;
		}
		if (resetSSGIHistory && globals::features::screenSpaceGI.loaded)
			globals::features::screenSpaceGI.QueueHistoryReset();
		lastUpdate = now;
		updateFrame = globals::state->frameCount;
		status = fresh ? "Active" : "Waiting for depth (holding near plane)";
	}
	for (uint32_t eye = 0; eye < 2; ++eye) {
		// If another controller already moved fNear off our applied value, capture it here
		// so RestoreCamera() does not overwrite that change with a stale saved value.
		if (appliedNear > 0.0f && !ApproximatelyEqual(cameraData.viewFrustumArray[eye].fNear, appliedNear))
			savedNear[eye] = cameraData.viewFrustumArray[eye].fNear;
		cameraData.viewFrustumArray[eye].fNear = controller.current;
	}
	camera->GetRuntimeData2().minNearPlaneDist = std::min(savedMinimum, settings.MinimumNearClip);
	appliedNear = controller.current;
	if (globals::state && globals::state->IsDeveloperMode() && now - lastLog >= kLogInterval) {
		logger::debug("VR dynamic near clip: near={:.4f}, target={:.4f}, relevant L/R={:.3f}/{:.3f}, absolute L/R={:.3f}/{:.3f}, {}",
			appliedNear, targetNear, nearest[0], nearest[1], absoluteNearest[0], absoluteNearest[1], status);
		lastLog = now;
	}
}

void VRDynamicNearClip::UpdateVanillaFogOverride()
{
	auto* fogEnabled = RE::GetINISetting("bFogEnabled:Weather");
	if (!fogEnabled)
		return;
	if (settings.DynamicNearClip) {
		if (!vanillaFogBeforeOverride)
			vanillaFogBeforeOverride = fogEnabled->GetBool();
		fogEnabled->SetBool(false);
	} else if (vanillaFogBeforeOverride) {
		fogEnabled->SetBool(*vanillaFogBeforeOverride);
		vanillaFogBeforeOverride.reset();
	}
}

bool VRDynamicNearClip::CheckProjection(const float4& cameraData)
{
	const auto& camera = controlledCamera->GetVRRuntimeData();
	if (camera.unk1C8 != 2 || !camera.viewFrustumArray)
		return false;
	observedEngineNear = cameraData.y;
	bool depthConsistent = std::isfinite(cameraData.x) && std::isfinite(cameraData.y) &&
	                       cameraData.y > 0.0f && cameraData.x > cameraData.y;
	bool requestMatches = ApproximatelyEqual(observedEngineNear, appliedNear);
	bool cacheMatches = true;
	std::array<float, 2> jitteredNear{};
	std::array<float4, 2> projectionZ{};
	for (uint32_t eye = 0; eye < 2; ++eye) {
		const auto view = Util::GetCameraData(eye);
		const auto& projection = view.projMatrixUnjittered;
		projectionZ[eye] = { projection(2, 2), projection(3, 2), projection(2, 3), projection(3, 3) };
		observedNear[eye] = VRNearClipMath::PerspectiveNear(projection(2, 2), projection(3, 2), projection(2, 3), projection(3, 3));
		jitteredNear[eye] = VRNearClipMath::PerspectiveNear(view.projMat(2, 2), view.projMat(3, 2), view.projMat(2, 3), view.projMat(3, 3));
		depthConsistent &= ApproximatelyEqual(observedNear[eye], observedEngineNear) && ApproximatelyEqual(jitteredNear[eye], observedEngineNear);
		requestMatches &= ApproximatelyEqual(observedNear[eye], appliedNear) && ApproximatelyEqual(camera.viewFrustumArray[eye].fNear, appliedNear);
		// The GPU upload cache is transposed and can describe a different camera or preparation.
		const auto& cached = globals::game::frameBufferCached.GetCameraProjUnjittered(eye);
		cachedNear[eye] = VRNearClipMath::PerspectiveNear(cached._33, cached._34, cached._43, cached._44);
		cacheMatches &= ApproximatelyEqual(cachedNear[eye], observedNear[eye]);
	}
	if (!depthConsistent || !requestMatches || !cacheMatches) {
		status = !depthConsistent ? "Waiting for matching eye depth parameters (retrying)" :
		         !requestMatches  ? "Engine near differs; adapting from rendered depth" :
		                            "Active (cached frame projection differs)";
		const auto now = Util::GetNowSecs();
		if (globals::state && globals::state->IsDeveloperMode() && now - lastProjectionLog >= kLogInterval) {
			logger::warn("VR dynamic near clip: {}; requested={:.6g}, engine={:.6g}, far={:.6g}, frustum L/R={:.6g}/{:.6g}, projection L/R={:.6g}/{:.6g}, jittered L/R={:.6g}/{:.6g}, cached L/R={:.6g}/{:.6g}; native Z/W L=[{:.6g},{:.6g},{:.6g},{:.6g}], R=[{:.6g},{:.6g},{:.6g},{:.6g}]",
				status, appliedNear, observedEngineNear, cameraData.x, camera.viewFrustumArray[0].fNear, camera.viewFrustumArray[1].fNear,
				observedNear[0], observedNear[1], jitteredNear[0], jitteredNear[1], cachedNear[0], cachedNear[1],
				projectionZ[0].x, projectionZ[0].y, projectionZ[0].z, projectionZ[0].w,
				projectionZ[1].x, projectionZ[1].y, projectionZ[1].z, projectionZ[1].w);
			lastProjectionLog = now;
		}
	}
	return depthConsistent;
}

void VRDynamicNearClip::SetupResources()
{
	resourcesReady = false;
	if (!globals::game::isVR || !hookReady)
		return;
	try {
		probeShader = nullptr;
		probeShader.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\VR\\DynamicNearClipCS.hlsl", {}, "cs_5_0")));
		if (!probeShader) {
			Fail("Depth probe shader unavailable; using the engine near plane");
			return;
		}
		auto device = globals::d3d::device;
		probeResult = nullptr;
		probeUAV = nullptr;
		auto desc = StructuredBufferDesc<EyeDepth>(uint64_t{ 2 }, true, false);
		DX::ThrowIfFailed(device->CreateBuffer(&desc, nullptr, probeResult.put()));
		Util::SetResourceName(probeResult.get(), "VR::NearClipProbe");
		D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
		view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		view.Buffer.NumElements = 2;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(probeResult.get(), &view, probeUAV.put()));
		Util::SetResourceName(probeUAV.get(), "VR::NearClipProbe UAV");
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		for (uint32_t index = 0; index < readbacks.size(); ++index) {
			readbacks[index] = {};
			DX::ThrowIfFailed(device->CreateBuffer(&desc, nullptr, readbacks[index].buffer.put()));
			Util::SetResourceName(readbacks[index].buffer.get(), "VR::NearClipReadback%u", index);
		}
		probeConstants = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ProbeConstants>(false), "VR::NearClipConstants");
		resourcesReady = true;
		failed = false;
		lastSample = {};
		lastUpdate = {};
		status = "Waiting for camera";
	} catch (const std::exception& error) {
		logger::warn("VR dynamic near clip resource error: {}", error.what());
		Fail("Depth probe resources unavailable; using the engine near plane");
	}
}

void VRDynamicNearClip::ClearShaderCache()
{
	SetupResources();
}

void VRDynamicNearClip::BeginWorldDepth()
{
	if (collectingWorldDepth || !controlledCamera || !resourcesReady || failed ||
		!settings.DynamicNearClip || controlledCamera.get() != RE::Main::WorldRootCamera())
		return;
	collectingWorldDepth = true;
}

void VRDynamicNearClip::FinishWorldDepth()
{
	if (!collectingWorldDepth)
		return;
	auto* depth = Util::GetCurrentSceneDepthSRV(false);
	if (depth)
		CaptureDepth(controlledCamera.get(), depth);
	collectingWorldDepth = false;
}

void VRDynamicNearClip::CaptureDepth(const RE::NiCamera* camera, ID3D11ShaderResourceView* prepassDepth, ID3D11ShaderResourceView* terrainDepth)
{
	if (!controlledCamera || camera != controlledCamera.get() || !resourcesReady || failed ||
		(!globals::state->inWorld && !collectingWorldDepth) || captureFrame == globals::state->frameCount)
		return;
	const auto cameraData = Util::GetCameraData();
	if (!CheckProjection(cameraData))
		return;
	auto slot = std::find_if(readbacks.begin(), readbacks.end(), [](const Readback& readback) { return !readback.pending; });
	if (slot == readbacks.end())
		return;
	auto* depth = prepassDepth ? prepassDepth : Util::GetCurrentSceneDepthSRV(false);
	if (!depth)
		return;
	winrt::com_ptr<ID3D11Resource> resource;
	depth->GetResource(resource.put());
	auto texture = resource.try_as<ID3D11Texture2D>();
	if (!texture)
		return;
	D3D11_TEXTURE2D_DESC desc{};
	texture->GetDesc(&desc);
	const auto resolution = Util::ConvertToDynamic(globals::state->screenSize, true);
	if (!std::isfinite(resolution.x) || !std::isfinite(resolution.y) || desc.SampleDesc.Count != 1 ||
		resolution.x < 4 || resolution.y < 2 || resolution.x > desc.Width || resolution.y > desc.Height)
		return;
	const ProbeConstants constants{ cameraData, static_cast<uint32_t>(resolution.x) & ~1u, static_cast<uint32_t>(resolution.y), terrainDepth ? 1u : 0u };
	probeConstants->Update(constants);
	auto* context = globals::d3d::context;
	winrt::com_ptr<ID3D11ComputeShader> savedShader;
	winrt::com_ptr<ID3D11Buffer> savedConstants;
	winrt::com_ptr<ID3D11ShaderResourceView> savedDepth;
	winrt::com_ptr<ID3D11ShaderResourceView> savedTerrainDepth;
	winrt::com_ptr<ID3D11UnorderedAccessView> savedUAV;
	context->CSGetShader(savedShader.put(), nullptr, nullptr);
	context->CSGetConstantBuffers(0, 1, savedConstants.put());
	context->CSGetShaderResources(0, 1, savedDepth.put());
	context->CSGetShaderResources(1, 1, savedTerrainDepth.put());
	context->CSGetUnorderedAccessViews(0, 1, savedUAV.put());
	std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> savedTargets{};
	winrt::com_ptr<ID3D11DepthStencilView> savedDSV;
	if (prepassDepth) {
		context->OMGetRenderTargets(static_cast<UINT>(savedTargets.size()), savedTargets.data(), savedDSV.put());
		context->OMSetRenderTargets(0, nullptr, nullptr);
	}
	{
		CS_GPU_PASS("VR::NearClipProbe");
		auto* buffer = probeConstants->CB();
		auto* uav = probeUAV.get();
		context->CSSetShader(probeShader.get(), nullptr, 0);
		context->CSSetConstantBuffers(0, 1, &buffer);
		context->CSSetShaderResources(0, 1, &depth);
		context->CSSetShaderResources(1, 1, &terrainDepth);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->Dispatch(2, 1, 1);
		uav = nullptr;
		depth = nullptr;
		terrainDepth = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, 1, &depth);
		context->CSSetShaderResources(1, 1, &terrainDepth);
		context->CopyResource(slot->buffer.get(), probeResult.get());
	}
	auto* buffer = savedConstants.get();
	auto* uav = savedUAV.get();
	depth = savedDepth.get();
	terrainDepth = savedTerrainDepth.get();
	context->CSSetShader(savedShader.get(), nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &buffer);
	context->CSSetShaderResources(0, 1, &depth);
	context->CSSetShaderResources(1, 1, &terrainDepth);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	if (prepassDepth) {
		context->OMSetRenderTargets(static_cast<UINT>(savedTargets.size()), savedTargets.data(), savedDSV.get());
		for (auto* target : savedTargets) {
			if (target)
				target->Release();
		}
	}
	slot->pending = true;
	slot->captured = Util::GetNowSecs();
	slot->serial = ++nextSerial;
	captureFrame = globals::state->frameCount;
}

void VRDynamicNearClip::DrawValues()
{
	if (!globals::state || !globals::state->IsDeveloperMode())
		return;
	ImGui::TextUnformatted(status);
	if (controlledCamera)
		ImGui::Text("Near %.4f | target %.4f", appliedNear, targetNear);
	else
		ImGui::TextUnformatted("Near override inactive: using the engine camera");
	ImGui::Text("Source near L/R: %.6g / %.6g", inputNear[0], inputNear[1]);
	ImGui::Text("Source far L/R: %.6g / %.6g", inputFar[0], inputFar[1]);
	ImGui::Text("Source minimum %.6g | far/near limit %.6g", inputMinimumNear, inputFarNearRatio);
	ImGui::Text("Last projection near L/R: %.4f / %.4f", observedNear[0], observedNear[1]);
	ImGui::Text("Last engine near: %.4f | cached L/R: %.4f / %.4f", observedEngineNear, cachedNear[0], cachedNear[1]);
	for (uint32_t eye = 0; eye < 2; ++eye) {
		if (std::isfinite(nearest[eye]))
			ImGui::Text("Nearest %s: relevant %.3f | absolute %.3f", eye == 0 ? "L" : "R", nearest[eye], absoluteNearest[eye]);
		else
			ImGui::Text("Nearest %s: no valid geometry", eye == 0 ? "L" : "R");
	}
	if (lastSample > 0.0)
		ImGui::Text("Depth age: %.0f ms", (Util::GetNowSecs() - lastSample) * 1000.0);
}

void VRDynamicNearClip::DrawSettings()
{
	if (ImGui::CollapsingHeader(T("feature.vr.near_clip.header", "Dynamic Near Clip (Experimental)"))) {
		if (ImGui::Checkbox(T("feature.vr.near_clip.enable", "Dynamic near clip"), &settings.DynamicNearClip))
			UpdateVanillaFogOverride();
		ImGui::TextWrapped("%s", T("feature.vr.near_clip.fog_warning", "Enabling this option disables vanilla fog. Please enable Exponential Height Fog instead."));
		ImGui::SliderFloat(T("feature.vr.near_clip.normal", "Normal near clip"), &settings.NormalNearClip, 0.1f, 30.0f, "%.2f");
		ImGui::SliderFloat(T("feature.vr.near_clip.minimum", "Minimum near clip"), &settings.MinimumNearClip, 0.01f, settings.NormalNearClip, "%.3f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat(T("feature.vr.near_clip.scale", "Near distance scale"), &settings.NearDistanceScale, 0.05f, 1.0f, "%.2f");
		ImGui::SliderFloat(T("feature.vr.near_clip.restore", "Restore speed (per second)"), &settings.RestoreSpeed, 0.01f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		ImGui::TextWrapped("%s", T("feature.vr.near_clip.help", "Distances use Skyrim units. Both eyes share the smaller required near distance. The central depth probe cannot see transparent surfaces or geometry already clipped between samples."));
		if (globals::state && globals::state->IsDeveloperMode()) {
			ImGui::Checkbox(T("feature.vr.near_clip.readout", "Show near clip headset readout"), &settings.DynamicNearClipReadout);
			DrawValues();
		}
	}
	settings.ClampNearClipSettings();
}

void VRDynamicNearClip::DrawReadout()
{
	if (!globals::state || !globals::state->IsDeveloperMode() || !settings.DynamicNearClipReadout)
		return;
	const float margin = ImGui::GetFontSize();
	ImGui::SetNextWindowPos({ ImGui::GetIO().DisplaySize.x - margin, margin }, ImGuiCond_Always, { 1.0f, 0.0f });
	if (ImGui::Begin("Dynamic near clip###VRNearClipReadout", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings))
		DrawValues();
	ImGui::End();
}
