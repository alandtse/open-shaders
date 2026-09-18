#include "Diagnostics.h"

#include "Globals.h"
#include "Menu.h"
#include "Utils/D3D.h"
#include <DirectXTex.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <limits>

namespace NR
{
	void Diagnostics::DumpTexture(const char* stage, ID3D11Resource* resource, uint32_t frame)
	{
		if (!resource || !stage || !globals::d3d::device || !globals::d3d::context)
			return;
		winrt::com_ptr<ID3D11Texture2D> source;
		if (FAILED(resource->QueryInterface(source.put())))
			return;
		D3D11_TEXTURE2D_DESC desc{};
		source->GetDesc(&desc);
		logger::info("[NRDiag/v2] dump {} resource=0x{:X} {}x{} format={} array={} samples={} misc=0x{:X}", stage,
			reinterpret_cast<uintptr_t>(resource), desc.Width, desc.Height, static_cast<uint32_t>(desc.Format), desc.ArraySize,
			desc.SampleDesc.Count, desc.MiscFlags);
		D3D11_TEXTURE2D_DESC stagingDesc = desc;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		stagingDesc.MiscFlags = 0;
		stagingDesc.ArraySize = 1;
		stagingDesc.SampleDesc.Count = 1;
		stagingDesc.SampleDesc.Quality = 0;
		winrt::com_ptr<ID3D11Texture2D> staging;
		if (FAILED(globals::d3d::device->CreateTexture2D(&stagingDesc, nullptr, staging.put())))
			return;
		globals::d3d::context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, source.get(), 0, nullptr);
		globals::d3d::context->Flush();
		DirectX::ScratchImage image;
		if (FAILED(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, staging.get(), image)))
			return;
		DirectX::ScratchImage statistics;
		if (SUCCEEDED(DirectX::Convert(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DXGI_FORMAT_R32G32B32A32_FLOAT,
				DirectX::TEX_FILTER_DEFAULT, 0.0f, statistics))) {
			const auto* pixels = reinterpret_cast<const float*>(statistics.GetPixels());
			const size_t pixelCount = static_cast<size_t>(statistics.GetPixelsSize() / (sizeof(float) * 4));
			if (pixels && pixelCount) {
				float minimum = std::numeric_limits<float>::max(), maximum = 0.0f, average = 0.0f;
				for (size_t i = 0; i < pixelCount; ++i) {
					const float luminance = std::max(0.0f, pixels[i * 4 + 0] * 0.2126f + pixels[i * 4 + 1] * 0.7152f + pixels[i * 4 + 2] * 0.0722f);
					minimum = std::min(minimum, luminance);
					maximum = std::max(maximum, luminance);
					average += luminance;
				}
				logger::info("[NRDiag/v2] stats {} pixels={} avgY={} minY={} maxY={}", stage, pixelCount, average / pixelCount, minimum, maximum);
			}
		}
		std::error_code ec;
		const auto directory = std::filesystem::path("Data/SKSE/Plugins/CommunityShaders/Captures");
		std::filesystem::create_directories(directory, ec);
		const auto path = directory / std::format("NR_{:02}_{}_f{}.dds", frame % 100, stage, frame);
		if (FAILED(DirectX::SaveToDDSFile(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DirectX::DDS_FLAGS_NONE, path.c_str())))
			logger::warn("[NRDiag/v2] failed to save {}", path.string());
		else
			logger::info("[NRDiag/v2] wrote {}", path.string());
	}

	void Diagnostics::CaptureStage(const char* stage, ID3D11Resource* resource, uint32_t frame)
	{
		if (CaptureActive(frame))
			DumpTexture(stage, resource, frame);
	}

	void Diagnostics::CaptureView(const char* stage, ID3D11ShaderResourceView* view, uint32_t frame)
	{
		if (!CaptureActive(frame) || !view)
			return;
		winrt::com_ptr<ID3D11Resource> resource;
		view->GetResource(resource.put());
		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
		view->GetDesc(&viewDesc);
		D3D11_TEXTURE2D_DESC resourceDesc{};
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (resource && SUCCEEDED(resource->QueryInterface(texture.put())))
			texture->GetDesc(&resourceDesc);
		logger::info("[NRDiag/v2] view {} resourceFormat={} viewFormat={} dimension={}", stage, static_cast<uint32_t>(resourceDesc.Format),
			static_cast<uint32_t>(viewDesc.Format), static_cast<uint32_t>(viewDesc.ViewDimension));
		CaptureStage(stage, resource.get(), frame);
	}

	const char* Diagnostics::Name(Outcome outcome)
	{
		switch (outcome) {
		case Outcome::NoHook:
			return "NO HOOK";
		case Outcome::Disabled:
			return "DISABLED";
		case Outcome::NoWorld:
			return "NO WORLD";
		case Outcome::Paused:
			return "PAUSED / MENU";
		case Outcome::FailedLatch:
			return "FAILURE LATCH";
		case Outcome::Error:
			return "ERROR";
		case Outcome::Bypassed:
			return "WRITEBACK BYPASSED";
		case Outcome::Applied:
			return "COPY QUEUED";
		default:
			return "UNKNOWN";
		}
	}

	char Diagnostics::Code(Outcome outcome)
	{
		constexpr char codes[] = "NDWPLEAB";
		return codes[static_cast<size_t>(outcome)];
	}

	Diagnostics::Frame& Diagnostics::BeginHook(uint32_t frame, uint32_t target)
	{
		if (current.number != frame) {
			current = {};
			current.number = frame;
		}
		current.options = options.load();
		++current.calls;
		current.target = target;
		return current;
	}

	void Diagnostics::Stage(uint32_t frame, bool finishedPost, uintptr_t main)
	{
		if (current.number != frame)
			return;
		if (finishedPost)
			current.afterPost = true;
		else
			current.afterUpscale = true;
		current.mainChanged |= current.source && current.source != main;
	}

	void Diagnostics::LogFrame(const Frame& frame)
	{
		logger::info(
			"[NRDiag/v2] frame={} outcome={} enabled={} world={} paused={} calls={} duplicates={} target={} "
			"size={}x{} sourceFormat={} proxyFormat={} source=0x{:X} eyes={} eval=0x{:X} copyQueued=0x{:X} created=0x{:X} "
			"resetL=0x{:X} resetR=0x{:X} ngxL=0x{:X} ngxR=0x{:X} recreated={} upscale={} post={} mainChanged={} fence={}/{}",
			frame.number, Name(frame.outcome), frame.enabled, frame.world, frame.paused, frame.calls, frame.duplicates, frame.target,
			frame.width, frame.height, frame.format, frame.proxyFormat, frame.source, frame.eyeCount, frame.evaluated, frame.copied, frame.created,
			frame.reset[0], frame.reset[1], frame.result[0], frame.result[1], frame.recreated, frame.afterUpscale, frame.afterPost, frame.mainChanged,
			frame.completedFence, frame.submittedFence);
		logger::info(
			"[NRDiag/v2] parameters frame={} conversion={} exposureMode={} compositeMode={} visualMode={} manualExposure={} diffStrength={} split={} "
			"intensity={} localTone={} localStructure={} skinStructure={}",
			frame.number, frame.conversion, frame.exposureMode,
			frame.compositeMode, frame.visualMode, frame.manualExposure, frame.differenceStrength, frame.splitPosition, frame.intensity, frame.localTone,
			frame.localStructure, frame.skinStructure);
	}

	void Diagnostics::OpenTrace()
	{
		traceFile.close();
		traceFile.clear();
		try {
			const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
			tracePath = (std::filesystem::temp_directory_path() / std::format("OpenShaders-NR-{}-{}.txt", GetCurrentProcessId(), stamp)).string();
			traceFile.open(tracePath);
			if (!traceFile)
				throw std::runtime_error("Cannot open diagnostic trace");
			traceFile << std::setprecision(9);
			traceFile << "NRDiag/v2 CPU scheduling and camera trace. Thresholds: distance>256, directionDot<0.5, projectionDelta>0.1.\n"
						 "Options: 1=ignorePosition,2=ignoreCameraCuts,4=forceReset,8=zeroMotion,16=zeroJitter,32=serializeGPU,64=bypassWriteback,128=bypassEvaluation,256=copyInput,512=interopRoundTrip.\n"
						 "Reset bits: 1=request,2=first,4=gap,8=position,16=direction,32=projection,64=creation.\n";
			logger::info("[NRDiag/v2] trace file: {}", tracePath);
		} catch (const std::exception& error) {
			tracePath = std::format("Trace file failed: {}", error.what());
			logger::error("[NRDiag/v2] {}", tracePath);
		}
	}

	void Diagnostics::WriteCameraTrace(const Frame& frame)
	{
		if (!traceFile)
			return;
		traceFile << "frame=" << frame.number << " options=" << frame.options << " outcome=" << Name(frame.outcome)
				  << " eval=" << frame.evaluated << " copy=" << frame.copied << " created=" << frame.created
				  << " recreated=" << frame.recreated << " size=" << frame.width << 'x' << frame.height
				  << " sourceFormat=" << frame.format << " proxyFormat=" << frame.proxyFormat
				  << " calls=" << frame.calls << " duplicates=" << frame.duplicates << " upscale=" << frame.afterUpscale
				  << " post=" << frame.afterPost << " mainChanged=" << frame.mainChanged
				  << " fence=" << frame.completedFence << '/' << frame.submittedFence << '\n';
		for (uint32_t i = 0; i < frame.eyeCount; ++i) {
			const auto& c = frame.camera[i];
			traceFile << "eye=" << i << " detected=" << c.detected << " reset=" << frame.reset[i] << " ngx=" << frame.result[i]
					  << " distance=" << c.distance << " directionDot=" << c.directionDot << " projectionDelta=" << c.projectionDelta
					  << " jitter=" << c.jitterX << ',' << c.jitterY << " dtMs=" << c.frameTimeMs;
			const std::array<const char*, 4> names{ " position=", " previous=", " enginePrevious=", " viewTranslation=" };
			const std::array<std::array<float, 3>, 4> vectors{ c.position, c.previous, c.enginePrevious, c.viewTranslation };
			for (size_t j = 0; j < vectors.size(); ++j)
				traceFile << names[j] << vectors[j][0] << ',' << vectors[j][1] << ',' << vectors[j][2];
			traceFile << '\n';
		}
	}

	void Diagnostics::EndFrame(uint32_t frame, bool enabled, bool world, bool paused)
	{
		if (current.number != frame) {
			current = {};
			current.number = frame;
		}
		current.enabled = enabled;
		current.world = world;
		current.paused = paused;
		std::scoped_lock lock(mutex);
		if (startSuite.exchange(false)) {
			if (!suite)
				savedOptions = options.load();
			suite = true;
			suiteStep = suiteFrames = 0;
			options = kSuiteOptions[0];
			traceRemaining = 0;
			OpenTrace();
			traceFile << "TEST Baseline\n";
			return;
		}
		if (stopSuite.exchange(false) && suite) {
			options = savedOptions;
			suite = false;
			traceFile << "SUITE STOPPED\n";
			traceFile.flush();
		}
		if (startTrace.exchange(false))
			OpenTrace();
		if (markFlicker.exchange(false)) {
			logger::info("[NRDiag/v2] FLICKER MARKER frame={} options={}", frame, current.options);
			traceFile << "FLICKER MARKER frame=" << frame << " options=" << current.options << '\n';
			traceFile.flush();
		}
		if (clearRequested.exchange(false))
			next = count = framesSinceSummary = 0;
		history[next] = current;
		next = (next + 1) % kHistorySize;
		count = std::min(count + 1, kHistorySize);
		if (auto remaining = traceRemaining.load(); remaining != 0 && enabled && world && !paused && !globals::menu->IsEnabled) {
			WriteCameraTrace(current);
			LogFrame(current);
			if (traceRemaining.fetch_sub(1) == 1)
				logger::info("[NRDiag/v2] bounded trace complete");
			if (traceRemaining.load() == 0)
				traceFile.flush();
		}
		if (suite && enabled && world && !paused && !globals::menu->IsEnabled) {
			WriteCameraTrace(current);
			if (suiteFrames % kHistorySize == 0)
				traceFile.flush();
			if (++suiteFrames >= kSuiteFrames) {
				suiteFrames = 0;
				if (++suiteStep == kSuiteOptions.size()) {
					options = savedOptions;
					suite = false;
					traceFile << "SUITE COMPLETE; restored options=" << savedOptions << '\n';
					logger::info("[NRDiag/v2] suite complete: {}", tracePath);
				} else {
					options = kSuiteOptions[suiteStep];
					traceFile << "TEST " << kSuiteNames[suiteStep] << '\n';
				}
				traceFile.flush();
			}
		}
		if (enabled && world && !paused && current.options != 0)
			LogFrame(current);
		if (enabled && ++framesSinceSummary >= kHistorySize) {
			std::string outcomes;
			uint32_t resets = 0, recreations = 0, duplicates = 0;
			for (size_t i = 0; i < count; ++i) {
				const auto& item = history[(next + kHistorySize - count + i) % kHistorySize];
				outcomes += Code(item.outcome);
				resets += (item.reset[0] || item.reset[1]) ? 1 : 0;
				recreations += item.recreated ? 1 : 0;
				duplicates += item.duplicates;
			}
			logger::info(
				"[NRDiag/v2] through={} frames={} resets={} recreations={} duplicateCalls={} "
				"history={} (A=copyQueued,N=noHook,D=disabled,W=noWorld,P=paused,L=failureLatch,E=error)",
				frame, count, resets, recreations, duplicates, outcomes);
			LogFrame(current);
			framesSinceSummary = 0;
		}
	}

	void Diagnostics::DrawSettings()
	{
		std::scoped_lock lock(mutex);
		ImGui::Separator();
		ImGui::TextWrapped("Session-only isolation tests. Camera-cut overrides preserve loading, frame-gap and resource resets. Option changes reset history once.");
		ImGui::BeginDisabled(suite);
		uint32_t selected = options.load();
		auto toggle = [&](const char* name, uint32_t bit) {
			bool value = (selected & bit) != 0;
			if (ImGui::Checkbox(name, &value)) {
				selected = value ? selected | bit : selected & ~bit;
				options = selected;
			}
		};
		toggle("Ignore camera-position resets", IgnorePosition);
		toggle("Ignore all camera-cut resets", IgnoreCameraCuts);
		toggle("Reset NR every frame", ForceReset);
		toggle("Zero NR motion vectors (stationary test)", ZeroMotion);
		toggle("Zero NR jitter parameter", ZeroJitter);
		toggle("Serialize GPU (slow diagnostic)", SerializeGPU);
		toggle("Bypass NR writeback (keep evaluating)", BypassWriteback);
		toggle("Bypass NR evaluation entirely", BypassEvaluation);
		toggle("Copy NR input directly to output", CopyInputToOutput);
		toggle("DX11 -> DX12 -> DX11 round trip only", InteropRoundTrip);
		toggle("Bypass NR mask", BypassMask);
		toggle("Force NR mask = 0", ForceMaskZero);
		toggle("Force NR mask = 1", ForceMaskOne);
		toggle("Visualize NR mask", VisualizeMask);
		toggle("Visualize skin mask (if supplied)", VisualizeSkinMask);
		toggle("Visualize auto mask (if supplied)", VisualizeAutoMask);
		toggle("Disable local tone", DisableTone);
		toggle("Disable local structure", DisableStructure);
		toggle("Disable skin processing", DisableSkin);
		toggle("Disable exposure adaptation", DisableExposure);
		toggle("Disable color transform", DisableColorTransform);
		uint32_t conversion = conversionMode.load(), exposure = exposureMode.load(), composition = compositeMode.load(), view = visualMode.load();
		float exposureValue = manualExposure.load(), differenceValue = differenceStrength.load(), split = splitPosition.load();
		if (ImGui::Combo("Color conversion", reinterpret_cast<int*>(&conversion), "Raw / none\0Linear -> sRGB\0sRGB -> Linear\0Linear -> Gamma 2.2\0Gamma 2.2 -> Linear\0Production\0"))
			conversionMode = std::min(conversion, 5u);
		if (ImGui::Combo("Exposure mode", reinterpret_cast<int*>(&exposure), "Production\0Ignore\0Force 1.0\0Game exposure\0Manual\0De-expose/re-expose\0Pass only\0Do not pass\0"))
			exposureMode = std::min(exposure, 7u);
		if (ImGui::SliderFloat("Manual exposure", &exposureValue, 0.01f, 16.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp))
			manualExposure = exposureValue;
		if (ImGui::Combo("Composition mode", reinterpret_cast<int*>(&composition), "Production\0Raw replacement\0Masked lerp\050% masked lerp\0Preserve luminance\0Preserve ratio\0Residual\0Ratio\0"))
			compositeMode = std::min(composition, 7u);
		if (ImGui::Combo("Debug view", reinterpret_cast<int*>(&view), "None\0NR input\0NR output\0Difference\0Ratio\0Original\0Post-composite\0Luminance difference\0Chroma difference\0Mask\0Exposure\0Split original / NR output\0Split original / composite\0Split NR input / output\0Split pre / post\0"))
			visualMode = std::min(view, 14u);
		if (ImGui::SliderFloat("Difference strength", &differenceValue, 1.0f, 16.0f, "%.0fx", ImGuiSliderFlags_AlwaysClamp))
			differenceStrength = differenceValue;
		if (ImGui::SliderFloat("Split position", &split, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
			splitPosition = split;
		if (ImGui::Button("Restore Diagnostic Defaults"))
			options = 0;
		if (ImGui::Button("Run All NR Tests"))
			startSuite = true;
		ImGui::TextWrapped("8 tests, 600 world frames each. Repeat standing still, turning and walking; watch the overlay. The sequence pauses while this menu is open. Tests 4-8 retain Ignore Position to isolate other causes.");
		ImGui::EndDisabled();
		if (suite && ImGui::Button("Stop Tests and Restore"))
			stopSuite = true;
		if (ImGui::Button("Mark Flicker in Trace"))
			markFlicker = true;
		if (ImGui::Button("Capture NR stages (lossless DDS)"))
			RequestCapture();
		if (!tracePath.empty()) {
			ImGui::TextWrapped("Trace: %s", tracePath.c_str());
			if (ImGui::Button("Copy Trace Path"))
				ImGui::SetClipboardText(tracePath.c_str());
		}
		ImGui::Checkbox("Show NR Diagnostics", &showOverlay);
		ImGui::BeginDisabled(suite);
		if (ImGui::Button("Record 300 NR Frames")) {
			startTrace = true;
			logger::info("[NRDiag/v2] recording requested; reset bits: 1=request,2=first,4=gap,8=position,16=direction,32=projection,64=creation");
			traceRemaining = kTraceFrames;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Clear NR Counters"))
			clearRequested = true;
		ImGui::TextWrapped("Scheduling diagnostics are CPU observations, not proof of GPU pixels. Traces use [NRDiag/v2] in CommunityShaders.log.");
	}

	void Diagnostics::DrawOverlay(bool enabled, const std::string& status)
	{
		if (!enabled || !showOverlay || !ImGui::GetCurrentContext())
			return;
		std::array<Frame, kHistorySize> snapshot;
		size_t snapshotNext, snapshotCount;
		std::string suiteStatus;
		{
			std::scoped_lock lock(mutex);
			suiteStatus = suite ? std::format("Test {}/8: {} ({}/600)", suiteStep + 1, kSuiteNames[suiteStep], suiteFrames) : "Manual testing / sequence idle";
			snapshot = history;
			snapshotNext = next;
			snapshotCount = count;
		}
		if (!snapshotCount)
			return;
		const auto& latest = snapshot[(snapshotNext + kHistorySize - 1) % kHistorySize];
		std::array<uint32_t, static_cast<size_t>(Outcome::Count)> outcomes{};
		uint32_t resets = 0, recreations = 0, duplicates = 0;
		std::string sequence;
		for (size_t i = 0; i < snapshotCount; ++i) {
			const auto& item = snapshot[(snapshotNext + kHistorySize - snapshotCount + i) % kHistorySize];
			++outcomes[static_cast<size_t>(item.outcome)];
			resets += (item.reset[0] || item.reset[1]) ? 1 : 0;
			recreations += item.recreated ? 1 : 0;
			duplicates += item.duplicates;
			sequence += Code(item.outcome);
			if ((i + 1) % (kHistorySize / 2) == 0)
				sequence += '\n';
		}
		const auto* viewport = ImGui::GetMainViewport();
		const auto padding = ImGui::GetStyle().WindowPadding;
		ImGui::SetNextWindowPos({ viewport->WorkPos.x + padding.x, viewport->WorkPos.y + viewport->WorkSize.y - padding.y }, ImGuiCond_FirstUseEver, { 0, 1 });
		auto flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
		if (!globals::menu->IsEnabled)
			flags |= ImGuiWindowFlags_NoInputs;
		if (ImGui::Begin("NR Diagnostics v2###NeuralRenderingDiagnostics", nullptr, flags)) {
			ImGui::TextUnformatted(suiteStatus.c_str());
			ImGui::Text("Options: %u | bypass frames %u", latest.options, outcomes[size_t(Outcome::Bypassed)]);
			for (uint32_t i = 0; i < latest.eyeCount; ++i)
				ImGui::Text("Eye %u: distance %.2f / 256 | dot %.3f | projection %.4f | detected 0x%X", i, latest.camera[i].distance, latest.camera[i].directionDot, latest.camera[i].projectionDelta, latest.camera[i].detected);
			const auto& palette = globals::menu->GetTheme().StatusPalette;
			ImGui::TextColored(latest.outcome == Outcome::Applied ? palette.SuccessColor : palette.Warning,
				"Frame %u: %s", latest.number, Name(latest.outcome));
			if (latest.outcome == Outcome::FailedLatch || latest.outcome == Outcome::Error)
				ImGui::TextWrapped("%s", status.c_str());
			ImGui::Text("Last %zu: applied %u | no hook %u | disabled %u | no world %u | paused %u | failures %u",
				snapshotCount, outcomes[size_t(Outcome::Applied)], outcomes[size_t(Outcome::NoHook)], outcomes[size_t(Outcome::Disabled)],
				outcomes[size_t(Outcome::NoWorld)], outcomes[size_t(Outcome::Paused)], outcomes[size_t(Outcome::FailedLatch)] + outcomes[size_t(Outcome::Error)]);
			ImGui::Text("Reset frames %u | recreations %u | duplicate calls %u", resets, recreations, duplicates);
			ImGui::Text("%ux%u | source/proxy DXGI %u/%u | eyes %u | eval 0x%X | copy queued 0x%X", latest.width, latest.height, latest.format, latest.proxyFormat, latest.eyeCount, latest.evaluated, latest.copied);
			ImGui::Text("Reset L/R 0x%X/0x%X | NGX L/R 0x%X/0x%X", latest.reset[0], latest.reset[1], latest.result[0], latest.result[1]);
			ImGui::Text("After upscale/post %u/%u | target %u | main changed %u", latest.afterUpscale, latest.afterPost, latest.target, latest.mainChanged);
			ImGui::TextUnformatted(sequence.c_str());
			ImGui::TextUnformatted("A=copy queued N=no hook D=disabled W=no world P=paused L=latched E=error B=bypassed");
			ImGui::Text("Trace frames remaining: %u (controls in Upscaling)", traceRemaining.load());
		}
		ImGui::End();
	}
}
