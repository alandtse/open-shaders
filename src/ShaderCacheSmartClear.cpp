// Scene-scoped ("smart") shader cache clear: bounded capture, scoped eviction, and
// the compilation-set bookkeeping it depends on. Split out of ShaderCache.cpp to keep
// that file from growing further; TrackActiveShader's capture write and the rest of
// ShaderCache's lifecycle stay there since they predate this feature.

#include "ShaderCache.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <unordered_set>

#include "Deferred.h"
#include "Feature.h"
#include "Globals.h"
#include "State.h"
#include "Util.h"

namespace SIE
{
	template <typename ShaderType, typename MutexType>
	void ReleaseShader(ShaderType& shaders,
		MutexType& mutex, RE::BSShader::Type type, uint32_t descriptor)
	{
		std::lock_guard<MutexType> lockGuard(mutex);

		if (static_cast<size_t>(type) < shaders.size()) {
			auto& shaderMap = shaders[static_cast<size_t>(type)];
			auto shaderIt = shaderMap.find(descriptor);
			if (shaderIt != shaderMap.end()) {
				auto& shaderPtr = shaderIt->second;
				if (shaderPtr && shaderPtr->shader) {
					shaderPtr->shader->Release();
				}
				shaderMap.erase(shaderIt);
			}
		}
	}

	void ShaderCache::EvictShader(const std::string& a_key, RE::BSShader::Type a_type, uint32_t a_descriptor,
		ShaderClass a_shaderClass, const std::wstring& a_diskPath, bool a_deleteDiskBlob)
	{
		// Remove shader key from shaderMap. Scoped and released before compilationMutex is
		// taken below - never nest these two mutexes (see CompilationSet::Add, which holds
		// compilationMutex while calling GetCompletedShader, which takes mapMutex; the reverse
		// nesting here would be a genuine AB-BA deadlock against a concurrent Add()).
		{
			std::unique_lock lockM{ mapMutex };
			shaderMap.erase(a_key);
		}

		// Handle vertex, pixel, and compute shaders (each will lock)
		switch (a_shaderClass) {
		case SIE::ShaderClass::Vertex:
			ReleaseShader(vertexShaders, vertexShadersMutex, a_type, a_descriptor);
			break;
		case SIE::ShaderClass::Pixel:
			ReleaseShader(pixelShaders, pixelShadersMutex, a_type, a_descriptor);
			break;
		case SIE::ShaderClass::Compute:
			ReleaseShader(computeShaders, computeShadersMutex, a_type, a_descriptor);
			break;
		default:
			logger::warn("Unexpected shader class: {}", static_cast<int>(a_shaderClass));
			break;
		}

		// Delete the associated file
		if (a_deleteDiskBlob) {
			const auto& filePathString = Util::WStringToString(a_diskPath);
			std::scoped_lock lockD{ compilationSet.compilationMutex };
			std::error_code ec;  // Use the error_code overload to avoid exceptions for non-critical errors like the file not existing.
			if (const bool removed = std::filesystem::remove(a_diskPath, ec); ec) {
				logger::warn("Error while trying to delete {}: {}", filePathString, ec.message());
			} else if (removed) {
				logger::debug("Deleted {}", filePathString);
			}  // If !removed and no error, the file didn't exist, which is fine.
		}

		logger::debug("Marking recompile for shader: {}", a_key);
	}

	bool ShaderCache::IsTrackingActiveShaders() const
	{
		return globals::state->IsDeveloperMode() ||
		       activeShaderCaptureFramesRemaining.load(std::memory_order_relaxed) > 0;
	}

	void ShaderCache::BeginActiveShaderCapture()
	{
		if (activeShaderCaptureStage != ActiveShaderCaptureStage::Idle)
			return;  // ignore re-trigger while a capture is already in progress

		activeShaderCaptureMenuWasVisible = false;
		clearedThisCaptureCycle.clear();
		StartActiveShaderCaptureWindow(ActiveShaderCaptureStage::FirstWindow);
	}

	void ShaderCache::StartActiveShaderCaptureWindow(ActiveShaderCaptureStage a_stage)
	{
		activeShaderCaptureStage = a_stage;
		activeShaderCaptureThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
		activeShaderCaptureDeadline = std::chrono::steady_clock::now() + kActiveShaderCaptureTimeout;
		{
			std::lock_guard lock(activeShadersMutex);
			capturedShaders.clear();
		}
		activeShaderCaptureFramesRemaining.store(kActiveShaderCaptureFrames, std::memory_order_relaxed);
	}

	bool ShaderCache::IsCapturingActiveShaders() const
	{
		return activeShaderCaptureStage == ActiveShaderCaptureStage::FirstWindow ||
		       activeShaderCaptureStage == ActiveShaderCaptureStage::SecondWindow;
	}

	uint32_t ShaderCache::GetActiveShaderCaptureFramesRemaining() const
	{
		return activeShaderCaptureFramesRemaining.load(std::memory_order_relaxed);
	}

	bool ShaderCache::IsAwaitingMenuCloseCapture() const
	{
		return activeShaderCaptureStage == ActiveShaderCaptureStage::AwaitingMenuClose;
	}

	void ShaderCache::TickActiveShaderCapture(bool a_menuVisible)
	{
		if (activeShaderCaptureStage == ActiveShaderCaptureStage::Idle)
			return;

		// A future hook re-plumbing that moves BeginActiveShaderCapture() or this tick off the
		// render thread would silently break the capture write's thread-identity gate.
		assert(std::this_thread::get_id() == activeShaderCaptureThread.load(std::memory_order_relaxed));

		switch (activeShaderCaptureStage) {
		case ActiveShaderCaptureStage::Idle:
			return;

		case ActiveShaderCaptureStage::FirstWindow:
		case ActiveShaderCaptureStage::SecondWindow:
			{
				uint32_t remaining = 0;
				if (auto prev = activeShaderCaptureFramesRemaining.load(std::memory_order_relaxed); prev > 0)
					remaining = activeShaderCaptureFramesRemaining.fetch_sub(1, std::memory_order_relaxed) - 1;
				const bool expired = remaining == 0 || std::chrono::steady_clock::now() >= activeShaderCaptureDeadline;
				if (!expired)
					return;

				activeShaderCaptureFramesRemaining.store(0, std::memory_order_relaxed);  // stop tracking before evicting
				const bool wasSecondWindow = activeShaderCaptureStage == ActiveShaderCaptureStage::SecondWindow;
				// Deferred/feature clearing isn't scoped by the capture, so only do it once per
				// cycle - on the second window it would just recompile the same features again
				// for nothing.
				ClearActive(/*a_clearFeatures=*/!wasSecondWindow);

				if (wasSecondWindow) {
					activeShaderCaptureStage = ActiveShaderCaptureStage::Idle;
				} else if (!a_menuVisible) {
					StartActiveShaderCaptureWindow(ActiveShaderCaptureStage::SecondWindow);
				} else {
					activeShaderCaptureStage = ActiveShaderCaptureStage::AwaitingMenuClose;
					activeShaderCaptureMenuWasVisible = a_menuVisible;
				}
				return;
			}

		case ActiveShaderCaptureStage::AwaitingMenuClose:
			if (activeShaderCaptureMenuWasVisible && !a_menuVisible)
				StartActiveShaderCaptureWindow(ActiveShaderCaptureStage::SecondWindow);
			activeShaderCaptureMenuWasVisible = a_menuVisible;
			return;
		}
	}

	size_t ShaderCache::ClearActive(bool a_clearFeatures)
	{
		std::vector<ActiveShaderInfo> entries;
		{
			std::lock_guard lock(activeShadersMutex);
			entries.reserve(capturedShaders.size());
			for (auto& [key, info] : capturedShaders)
				entries.push_back(std::move(info));
			capturedShaders.clear();
		}

		LARGE_INTEGER start, end, freq;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&start);

		std::unordered_set<size_t> taskIds;
		taskIds.reserve(entries.size());
		size_t evictedCount = 0;
		for (const auto& entry : entries) {
			// Already evicted earlier in this same click's capture cycle (e.g. still on
			// screen across both windows) - clearing it again would just force a second,
			// pointless recompile of a shader that may have already finished the first one.
			if (clearedThisCaptureCycle.contains(entry.key))
				continue;
			// A shader still Pending is actively compiling on a pool thread. Evicting it here
			// would let a subsequent Get*Shader miss enqueue a second, duplicate compile for
			// the same descriptor while the original is still running - two threads racing to
			// D3DWriteBlobToFile the same disk path. Leave it; it isn't broken, just in flight.
			if (GetShaderStatus(entry.key) == ShaderCompilationTask::Status::Pending)
				continue;
			EvictShader(entry.key, entry.shaderType, entry.descriptor, entry.shaderClass, entry.diskPath, IsDiskCache());
			taskIds.insert(ShaderCompilationTask::MakeId(entry.shaderClass, entry.shaderType, entry.descriptor));
			clearedThisCaptureCycle.insert(entry.key);
			++evictedCount;
		}
		// Must run after every EvictShader() above: Add() refuses to re-enqueue a task still
		// present in processedTasks, independent of whether shaderMap still has it (ShaderCache.h).
		compilationSet.Forget(taskIds);

		if (a_clearFeatures) {
			globals::deferred->ClearShaderCache();
			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded)
					feature->ClearShaderCacheScoped();
			}
		}

		QueryPerformanceCounter(&end);
		lastScopedClearCount = evictedCount;
		lastScopedClearMs = static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
		logger::info("Smart shader cache clear: evicted {} shader(s) in {:.2f} ms", lastScopedClearCount, lastScopedClearMs);

		return lastScopedClearCount;
	}

	void CompilationSet::Forget(const std::unordered_set<size_t>& a_taskIds)
	{
		if (a_taskIds.empty())
			return;

		auto pred = [&a_taskIds](const ShaderCompilationTask& task) {
			return a_taskIds.contains(task.GetId());
		};

		std::scoped_lock lock(compilationMutex);
		// availableTasks is deliberately left untouched: a queued-but-unstarted task is work
		// we still want done, and by the time this runs the disk blob has already been deleted
		// (ShaderCache::ClearActive), so it will compile fresh rather than reload stale data.
		std::erase_if(processedTasks, pred);
		std::erase_if(tasksInProgress, pred);
	}
}
