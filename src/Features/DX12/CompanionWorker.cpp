#include "CompanionWorker.h"
#include "SystemMemoryBudget.h"
#include "Utils/PerfUtils.h"

#include <Windows.h>

#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace DX12
{
	struct CompanionPriority
	{
		std::mutex mutex;
		HANDLE process = nullptr;
		DWORD base = IDLE_PRIORITY_CLASS;
		DWORD applied = 0;
		bool loading = false, destination = false;

		~CompanionPriority()
		{
			if (process)
				CloseHandle(process);
		}

		void Apply()
		{
			const DWORD desired = loading && destination ? HIGH_PRIORITY_CLASS : base;
			if (!process || desired == applied)
				return;
			if (SetPriorityClass(process, desired)) {
				applied = desired;
				logger::debug("[DX12Resources] Companion process priority {}", desired);
			} else {
				logger::warn("[DX12Resources] Could not set companion process priority: {}", GetLastError());
				if (applied == HIGH_PRIORITY_CLASS && desired != HIGH_PRIORITY_CLASS)
					TerminateProcess(process, 1);
			}
		}

		void SetLoading(bool active)
		{
			std::scoped_lock lock(mutex);
			loading = active;
			Apply();
		}

		void Attach(HANDLE handle, DWORD priority, bool isDestination)
		{
			std::scoped_lock lock(mutex);
			if (process)
				CloseHandle(process);
			process = nullptr;
			applied = 0;
			base = priority;
			destination = isDestination;
			if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &process, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
				logger::warn("[DX12Resources] Could not duplicate companion process handle: {}", GetLastError());
				// Never elevate a process whose priority cannot be revoked.
				SetPriorityClass(handle, priority);
				return;
			}
			Apply();
		}
	};

	namespace
	{
		constexpr DWORD kPreparationDeadlineMs = 90000;
		constexpr DWORD kStartupDeadlineMs = 10 * 60 * 1000;
		constexpr DWORD kStartupProgressIntervalMs = 10000;
		constexpr DWORD kCancellationGraceMs = 2000;

		DWORD RequestPriority(const CompanionRequest& task)
		{
			return task.warmup || task.preloadLastSave ? ABOVE_NORMAL_PRIORITY_CLASS :
			       task.predictive ? IDLE_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS;
		}

		std::wstring Quote(std::wstring_view value)
		{
			std::wstring result = L"\"";
			size_t slashCount = 0;
			for (const wchar_t ch : value) {
				if (ch == L'\\') {
					++slashCount;
					continue;
				}
				if (ch == L'"') {
					result.append(slashCount * 2 + 1, L'\\');
					result.push_back(ch);
					slashCount = 0;
					continue;
				}
				result.append(slashCount, L'\\');
				slashCount = 0;
				result.push_back(ch);
			}
			result.append(slashCount * 2, L'\\');
			result.push_back(L'"');
			return result;
		}

		std::filesystem::path PluginsFile()
		{
			const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
			if (length < 2 || length > 32768)
				return {};
			std::wstring folder(length, L'\0');
			if (GetEnvironmentVariableW(L"LOCALAPPDATA", folder.data(), length) != length - 1)
				return {};
			folder.resize(length - 1);
			return std::filesystem::path(folder) / L"Skyrim Special Edition" / L"Plugins.txt";
		}

		void RemoveStaleSessions(const std::filesystem::path& work)
		{
			std::error_code ec;
			for (const auto& entry : std::filesystem::directory_iterator(work, ec)) {
				if (ec)
					break;
				if (!entry.is_directory(ec) || entry.is_symlink(ec))
					continue;
				const auto name = entry.path().filename().string();
				if (!name.starts_with("session-"))
					continue;
				const auto suffix = name.substr(8);
				uint32_t pid = 0;
				const auto [end, error] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), pid);
				if (error != std::errc{} || end == suffix.data() || *end != '-')
					continue;
				const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
				bool exited = false;
				if (process) {
					exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
					CloseHandle(process);
				} else {
					// ERROR_INVALID_PARAMETER means the owner PID no longer exists.
					exited = GetLastError() == ERROR_INVALID_PARAMETER;
				}
				if (exited)
					std::filesystem::remove_all(entry.path(), ec);
				ec.clear();
			}
		}

		// This service belongs exclusively to the companion worker thread. It survives
		// consecutive tiles and retains Mutagen's initialized load order and archive handles.
		struct CompanionProcess
		{
			HANDLE job = nullptr;
			HANDLE process = nullptr;
			std::filesystem::path root;

			~CompanionProcess() { Close(); }

			void Close()
			{
				if (job)
					TerminateJobObject(job, 1);
				if (process) {
					WaitForSingleObject(process, 100);
					CloseHandle(process);
					process = nullptr;
				}
				if (job) {
					CloseHandle(job);
					job = nullptr;
				}
				std::error_code ignored;
				if (!root.empty())
					std::filesystem::remove_all(root, ignored);
				root.clear();
			}

			bool Start(const std::filesystem::path& executable, const std::filesystem::path& data,
				const std::filesystem::path& plugins, const std::filesystem::path& work, DWORD priority)
			{
				if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
					return true;
				Close();
				root = work / std::format(L"session-{}-{}-service", GetCurrentProcessId(), GetTickCount64());
				std::error_code ec;
				if (!std::filesystem::create_directory(root, ec) || ec)
					return false;
				job = CreateJobObjectW(nullptr, nullptr);
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
				limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
				if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
					Close();
					return false;
				}
				std::wstring command = Quote(executable.wstring()) + L" --serve " +
				                       Quote(data.wstring()) + L" " + Quote(plugins.wstring()) + L" " + Quote(root.wstring());
				STARTUPINFOW startup{};
				startup.cb = sizeof(startup);
				PROCESS_INFORMATION started{};
				const BOOL launched = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
					CREATE_NO_WINDOW | priority | CREATE_SUSPENDED,
					nullptr, nullptr, &startup, &started);
				if (!launched) {
					Close();
					return false;
				}
				process = started.hProcess;
				const bool assigned = AssignProcessToJobObject(job, process) != FALSE;
				const bool resumed = assigned && ResumeThread(started.hThread) != DWORD(-1);
				CloseHandle(started.hThread);
				if (!assigned || !resumed) {
					// A child not assigned to the job survives Close() unless terminated directly.
					TerminateProcess(process, 1);
					Close();
					return false;
				}
				return true;
			}
		};

		CompanionProcess& CurrentCompanionProcess()
		{
			static thread_local CompanionProcess service;
			return service;
		}

		bool InvokeCompanion(const CompanionRequest& task, const std::filesystem::path& destination,
			std::stop_token stop, const std::atomic<uint64_t>& generation, uint64_t epoch, uint64_t requestID,
			CompanionPriority& priorityState, std::string& failure)
		{
			const auto executable = std::filesystem::absolute(
				L"Data/SKSE/Plugins/CommunityShaders/DX12Resources/bin/DX12ResourcesPrep.exe");
			const auto data = std::filesystem::absolute(L"Data");
			const auto plugins = PluginsFile();
			std::error_code ec;
			if (!std::filesystem::is_regular_file(executable, ec)) {
				failure = "Missing DX12ResourcesPrep.exe in DX12Resources/bin (AIO installation required)";
				return false;
			}
			if (!std::filesystem::is_regular_file(plugins, ec)) {
				failure = "Skyrim SE plugins.txt is unavailable in LocalAppData";
				return false;
			}
			// Host-owned per-session mailbox; no persistent geometry output or diagnostics.
			auto& service = CurrentCompanionProcess();
			const auto work = destination.parent_path().parent_path();
			const bool startup = task.warmup || task.preloadLastSave;
			const auto priority = RequestPriority(task);
			if (!service.Start(executable, data, plugins, work, priority)) {
				failure = "Could not launch persistent .NET companion process";
				return false;
			}
			priorityState.Attach(service.process, priority, task.loading);
			const auto requestTmp = service.root / L"request.tmp";
			const auto request = service.root / L"request.json";
			const auto reply = service.root / L"reply.json";
			const auto cancelFile = service.root / std::format(L"cancel-{}", requestID);
			const json message = {
				{ "requestId", requestID },
				{ "scopePlugin", task.scopePlugin },
				{ "localScopeID", task.localScopeID },
				{ "x", task.key.x },
				{ "y", task.key.y },
				{ "size", task.size },
				{ "worldspace", task.worldspace },
				{ "warmup", task.warmup },
				{ "preloadLastSave", task.preloadLastSave },
				{ "consolidate", task.consolidate }
			};
			{
				std::ofstream stream(requestTmp, std::ios::binary | std::ios::trunc);
				if (!stream) {
					failure = "Cannot open companion request mailbox";
					service.Close();
					return false;
				}
				stream << message.dump();
				stream.flush();
				if (!stream) {
					failure = "Cannot write companion request mailbox";
					service.Close();
					return false;
				}
			}
			std::filesystem::rename(requestTmp, request, ec);
			if (ec) {
				failure = "Cannot publish companion request mailbox";
				service.Close();
				return false;
			}
			const auto started = Util::GetQpcTicks();
			const auto frequency = Util::GetQpcFrequency();
			const auto deadline = Util::QpcDeadlineAfterMs(started, frequency, startup ? kStartupDeadlineMs : kPreparationDeadlineMs);
			auto nextProgress = Util::QpcDeadlineAfterMs(started, frequency, kStartupProgressIntervalMs);
			const char* stage = task.preloadLastSave ? "last-save cell" : "NIF manifests";
			if (startup)
				logger::info("[DX12Resources] Menu {} preparation started (above-normal process priority)", stage);
			std::optional<int64_t> cancellationDeadline;
			while (!stop.stop_requested() && Util::QpcDeadlinePending(Util::GetQpcTicks(), deadline)) {
				if (generation.load(std::memory_order_acquire) != epoch && !cancellationDeadline) {
					std::ofstream cancel(cancelFile, std::ios::binary | std::ios::trunc);
					if (!cancel) {
						service.Close();
						return false;
					}
					cancellationDeadline = Util::QpcDeadlineAfterMs(Util::GetQpcTicks(), frequency, kCancellationGraceMs);
				}
				if (cancellationDeadline && !Util::QpcDeadlinePending(Util::GetQpcTicks(), *cancellationDeadline))
					break;
				if (startup && !Util::QpcDeadlinePending(Util::GetQpcTicks(), nextProgress)) {
					const auto now = Util::GetQpcTicks();
					logger::info("[DX12Resources] Waiting for {} companion reply ({:.1f} s)", stage,
						double(now - started) / double(frequency));
					nextProgress = Util::QpcDeadlineAfterMs(now, frequency, kStartupProgressIntervalMs);
				}
				if (std::filesystem::is_regular_file(reply, ec)) {
					if (cancellationDeadline) {
						std::filesystem::remove(reply, ec);
						if (ec) {
							service.Close();
							return false;
						}
						std::filesystem::remove(cancelFile, ec);
						if (ec)
							service.Close();
						return false;
					}
					std::filesystem::rename(reply, destination, ec);
					if (!ec)
						return true;
					break;
				}
				if (WaitForSingleObject(service.process, 25) != WAIT_TIMEOUT)
					break;
			}
			if (generation.load(std::memory_order_acquire) != epoch) {
				service.Close();
				return false;
			}
			// Catch a startup-error reply even when the .NET service exits just
			// after finishing its atomic reply and before the host polls again.
			if (std::filesystem::is_regular_file(reply, ec)) {
				ec.clear();
				std::filesystem::rename(reply, destination, ec);
				if (!ec)
					return true;
			}
			if (stop.stop_requested())
				failure = "Companion request canceled by scene change";
			else if (!Util::QpcDeadlinePending(Util::GetQpcTicks(), deadline))
				failure = "Companion preparation timed out (load order or NIF traversal too slow)";
			else
				failure = "Companion exited before returning a tile";
			// A stopped or timed-out request kills only the companion job, not Skyrim.
			service.Close();
			return false;
		}

		std::array<float, 3> Triple(const json& values)
		{
			if (!values.is_array() || values.size() != 3)
				throw std::invalid_argument("Invalid point");
			return { values[0].get<float>(), values[1].get<float>(), values[2].get<float>() };
		}
	}

	CompanionWorker::CompanionWorker() : priorityState(std::make_unique<CompanionPriority>()), worker([this](std::stop_token stop) { Run(stop); }) {}

	CompanionWorker::~CompanionWorker()
	{
		Stop();
	}

	void CompanionWorker::Stop()
	{
		priorityState->SetLoading(false);
		worker.request_stop();
		wake.notify_all();
	}

	bool CompanionWorker::Idle() const
	{
		std::scoped_lock lock(mutex);
		return !running && !queued && !completed && !loadingQueued && !loadingTarget;
	}

	bool CompanionWorker::Enqueue(CompanionRequest request)
	{
		std::scoped_lock lock(mutex);
		if (running || queued || completed || loadingQueued || loadingTarget || worker.get_stop_token().stop_requested())
			return false;
		queued = std::move(request);
		wake.notify_one();
		return true;
	}

	std::optional<CompanionResult> CompanionWorker::Take()
	{
		std::scoped_lock lock(mutex);
		return std::exchange(completed, std::nullopt);
	}

	void CompanionWorker::Cancel()
	{
		std::scoped_lock lock(mutex);
		generation.fetch_add(1, std::memory_order_acq_rel);
		queued.reset();
		completed.reset();
		loadingQueued.reset();
		loadingTarget.reset();
		loadingResults.clear();
		priorityState->SetLoading(false);
		wake.notify_all();
	}

	void CompanionWorker::CancelStartup()
	{
		std::scoped_lock lock(mutex);
		if (!runningStartup && !(queued && (queued->warmup || queued->preloadLastSave)))
			return;
		generation.fetch_add(1, std::memory_order_acq_rel);
		queued.reset();
		wake.notify_all();
	}

	bool CompanionWorker::BeginLoading(CompanionRequest request, double x, double y, std::shared_ptr<const Grid> existing)
	{
		const auto keys = Neighborhood(request.key.scope, request.size, x, y);
		if (!keys || worker.get_stop_token().stop_requested())
			return false;
		request.key = keys->front();
		request.loading = true;
		std::scoped_lock lock(mutex);
		if (loadingTarget && loadingTarget->key == request.key && loadingTarget->size == request.size &&
			loadingTarget->consolidate == request.consolidate)
			return false;
		generation.fetch_add(1, std::memory_order_acq_rel);
		queued.reset();
		completed.reset();
		loadingResults.clear();
		loadingTarget = request;
		loadingQueued = LoadingBatch{ std::move(request), x, y, std::move(existing) };
		priorityState->SetLoading(true);
		wake.notify_all();
		return true;
	}

	void CompanionWorker::EndLoading()
	{
		std::scoped_lock lock(mutex);
		priorityState->SetLoading(false);
		if (!loadingTarget)
			return;
		loadingTarget.reset();
		loadingQueued.reset();
		generation.fetch_add(1, std::memory_order_acq_rel);
		wake.notify_all();
	}

	std::vector<std::unique_ptr<CompanionResult>> CompanionWorker::TakeLoadingResults()
	{
		std::scoped_lock lock(mutex);
		return std::exchange(loadingResults, {});
	}

	void CompanionWorker::RunLoading(const LoadingBatch& batch, uint64_t epoch, std::stop_token stop)
	{
		const auto active = Neighborhood(batch.request.key.scope, batch.request.size, batch.x, batch.y);
		const auto predictive = PredictiveNeighborhood(batch.request.key.scope, batch.request.size, batch.x, batch.y);
		if (!active || !predictive)
			return;
		const auto& existing = batch.existing;
		const bool reuse = existing && existing->size == batch.request.size;
		if (reuse && existing->wholeCell && existing->requested.front().scope == batch.request.key.scope)
			return;
		const auto ready = [&](SubcellKey key) {
			if (!reuse || existing->wholeCell)
				return false;
			const auto contains = [&](const auto& tiles) {
				return std::ranges::any_of(tiles, [&](const auto& tile) {
					return tile && tile->key == key && (tile->state == Tile::State::Ready || tile->state == Tile::State::ReadyPartial ||
						tile->state == Tile::State::Empty || tile->state == Tile::State::EmptyPartial);
				});
			};
			return contains(existing->resident) || contains(existing->predictiveResident);
		};
		for (size_t i = 0; i < active->size() + predictive->size(); ++i) {
			if (stop.stop_requested() || generation.load(std::memory_order_acquire) != epoch)
				break;
			auto task = batch.request;
			task.predictive = i >= active->size();
			task.corner = i >= active->size() + kPredictiveSideCount;
			task.key = task.predictive ? (*predictive)[i - active->size()] : (*active)[i];
			task.consolidate = task.consolidate && i == 0;
			if (ready(task.key))
				continue;
			const auto started = Util::GetQpcTicks();
			auto result = std::make_unique<CompanionResult>(Prepare(task, epoch, stop));
			result->predictive = task.predictive;
			result->corner = task.corner;
			result->preparationMs = double(Util::GetQpcTicks() - started) * 1000.0 / double(Util::GetQpcFrequency());
			if (!result->publication.validated)
				continue;
			const bool wholeCell = result->publication.wholeCell;
			{
				std::scoped_lock lock(mutex);
				if (stop.stop_requested() || generation.load(std::memory_order_acquire) != epoch)
					break;
				loadingResults.push_back(std::move(result));
			}
			if (wholeCell)
				break;
		}
	}

	void CompanionWorker::Run(std::stop_token stop)
	{
		uint64_t observedGeneration = generation.load(std::memory_order_acquire);
		while (!stop.stop_requested()) {
			CompanionRequest task;
			std::optional<LoadingBatch> batch;
			uint64_t epoch = 0;
			{
				std::unique_lock lock(mutex);
				wake.wait(lock, stop, [&] {
					return queued.has_value() || loadingQueued.has_value() || generation.load(std::memory_order_acquire) != observedGeneration;
				});
				if (stop.stop_requested())
					break;
				if (generation.load(std::memory_order_acquire) != observedGeneration) {
					observedGeneration = generation.load(std::memory_order_acquire);
					// A canceled tile invalidates its publication, not the load-order manifest cache.
					continue;
				}
				if (loadingQueued) {
					batch = std::exchange(loadingQueued, std::nullopt);
					runningLoading = true;
				} else {
					task = std::move(*queued);
					queued.reset();
				}
				running = true;
				runningStartup = !batch && (task.warmup || task.preloadLastSave);
				epoch = generation.load(std::memory_order_acquire);
			}
			if (batch) {
				try {
					RunLoading(*batch, epoch, stop);
				} catch (const std::exception& error) {
					logger::warn("[DX12Resources] Destination preparation failed: {}", error.what());
				} catch (...) {
					logger::warn("[DX12Resources] Destination preparation failed");
				}
				std::scoped_lock lock(mutex);
				running = runningLoading = false;
				continue;
			}
			const auto started = Util::GetQpcTicks();
			auto result = Prepare(task, epoch, stop);
			result.predictive = task.predictive;
			result.corner = task.corner;
			result.preparationMs = double(Util::GetQpcTicks() - started) * 1000.0 / double(Util::GetQpcFrequency());
			{
				std::scoped_lock lock(mutex);
				if (!stop.stop_requested() && epoch == generation.load(std::memory_order_acquire))
					completed = std::move(result);
			}
			if (task.preloadLastSave && !stop.stop_requested() && epoch == generation.load(std::memory_order_acquire)) {
				CompanionRequest warmup;
				warmup.warmup = true;
				const auto warmed = Prepare(warmup, epoch, stop);
				if (epoch == generation.load(std::memory_order_acquire)) {
					if (!warmed.failure.empty())
						logger::warn("[DX12Resources] Manifest warmup failed: {}", warmed.failure);
					else
						logger::info("[DX12Resources] NIF manifest warmup completed");
				}
			}
			{
				std::scoped_lock lock(mutex);
				running = runningStartup = false;
			}
		}
	}

	CompanionResult CompanionWorker::Prepare(const CompanionRequest& task, uint64_t epoch, std::stop_token stop)
	{
		CompanionResult result;
		result.startup = task.warmup || task.preloadLastSave;
		result.preload = task.preloadLastSave;
		result.size = task.size;
		result.publication.key = task.key;
		const auto work = std::filesystem::absolute(L"Data/SKSE/Plugins/CommunityShaders/DX12Resources/work");
		std::error_code ec;
		std::filesystem::create_directories(work, ec);
		if (ec) {
			result.failure = "Cannot create companion temporary work directory";
			return result;
		}
		RemoveStaleSessions(work);
		const auto folder = work / std::format(L"session-{}-{}", GetCurrentProcessId(), GetTickCount64());
		if (!std::filesystem::create_directory(folder, ec) || ec) {
			result.failure = "Cannot create companion temporary session";
			return result;
		}
		struct Cleanup
		{
			std::filesystem::path folder, parent;
			~Cleanup()
			{
				std::error_code ignored;
				std::filesystem::remove_all(folder, ignored);
				ignored.clear();
				std::filesystem::remove(parent, ignored);  // Only when empty.
			}
		} cleanup{ folder, work };
		const auto file = folder / L"tile.json";
		static thread_local uint64_t nextRequestID = 0;
		const uint64_t requestID = ++nextRequestID;
		if (!InvokeCompanion(task, file, stop, generation, epoch, requestID, *priorityState, result.failure) ||
			generation.load(std::memory_order_acquire) != epoch || stop.stop_requested())
			return result;
		try {
			std::ifstream stream(file, std::ios::binary | std::ios::ate);
			if (!stream || stream.tellg() <= 0 || !HasAvailableSystemMemory(static_cast<uint64_t>(stream.tellg()))) {
				result.failure = "Companion reply missing, empty, or insufficient system memory";
				return result;
			}
			stream.seekg(0);
			const auto root = json::parse(stream);
			const uint64_t replyID = root.value("requestId", uint64_t{ 0 });
			if (replyID != requestID) {
				// Initialization failures precede request consumption and cannot certify a tile.
				const auto failure = root.value("failure", std::string{});
				result.failure = replyID == 0 && !failure.empty() ? failure.substr(0, 320) : "Companion reply request ID mismatch";
				CurrentCompanionProcess().Close();
				return result;
			}
			result.residencyReason = root.value("residencyReason", std::string{}).substr(0, 320);
			if (const auto entry = root.find("failure"); entry != root.end() && entry->is_string()) {
				result.failure = entry->get<std::string>().substr(0, 320);
				if (!result.failure.empty())
					return result;
			}
			if (root.at("version").get<uint32_t>() != 1 ||
				!root.at("shapes").is_array() ||
				!root.at("lights").is_array() || root.at("lights").size() > kMaxPreparedLights) {
				result.failure = "Companion reply schema/shape/light-count invalid";
				return result;
			}
			auto& publication = result.publication;
			std::unordered_set<uint64_t> identities;
			publication.eligibleShapes = root.at("eligibleShapes").get<uint32_t>();
			publication.failedShapes = root.at("failedShapes").get<uint32_t>();
			publication.enumerationComplete = root.at("enumerationComplete").get<bool>();
			publication.wholeCell = root.value("wholeCell", false);
			publication.spatialBoundsComplete = root.value("spatialBoundsComplete", false);
			if (publication.wholeCell) {
				result.scopePlugin = root.at("scopePlugin").get<std::string>();
				result.scopeLocal = root.at("scopeLocal").get<uint32_t>();
				const auto x = root.at("centerX").get<int64_t>();
				const auto y = root.at("centerY").get<int64_t>();
				if (result.scopePlugin.empty() || result.scopePlugin.size() > 128 || !result.scopeLocal ||
					x < -1000000 || x > 1000000 || y < -1000000 || y > 1000000 ||
					(!task.preloadLastSave && (!task.consolidate || task.worldspace ||
					 result.scopePlugin != task.scopePlugin || result.scopeLocal != task.localScopeID ||
					 x != task.key.x || y != task.key.y)))
					throw std::invalid_argument("Invalid whole-cell scope");
				publication.key.x = x;
				publication.key.y = y;
			}
			// The companion reports only bounded reason counts; never write per-mesh logs.
			if (const auto reasons = root.find("rejectionReasons");
				reasons != root.end() && reasons->is_object()) {
				for (auto entry = reasons->begin();
					entry != reasons->end() && publication.rejectionReasons.size() < 12; ++entry) {
					if (!entry.value().is_number_unsigned())
						continue;
					const auto count = entry.value().get<uint32_t>();
					if (count)
						publication.rejectionReasons.emplace_back(entry.key().substr(0, 80), count);
				}
			}
			// Optional NIF provenance for later runtime geometry supplementation. These
			// are identifiers, not authorization to load arbitrary paths or trust triangles.
			if (const auto nifSources = root.find("nifSources");
				nifSources != root.end() && nifSources->is_array()) {
				for (const auto& item : *nifSources) {
					NifSourceDescriptor source;
					source.referencePlugin = item.at("referencePlugin").get<std::string>();
					source.referenceLocal = item.at("referenceLocal").get<uint32_t>();
					source.nifPath = item.at("nifPath").get<std::string>();
					if (source.referencePlugin.size() > 128 || source.nifPath.size() > 260 ||
						!source.nifPath.starts_with("meshes\\"))
						continue;
					publication.nifSources.push_back(std::move(source));
				}
			}
			if (const auto missing = root.find("missingShapes");
				missing != root.end() && missing->is_array()) {
				for (const auto& item : *missing) {
					MissingNifShapeDescriptor source;
					source.referencePlugin = item.at("referencePlugin").get<std::string>();
					source.referenceLocal = item.at("referenceLocal").get<uint32_t>();
					source.nifPath = item.at("nifPath").get<std::string>();
					source.blockIndex = item.at("blockIndex").get<int32_t>();
					source.reason = item.at("reason").get<std::string>();
					if (source.referencePlugin.size() > 128 || source.nifPath.size() > 260 ||
						source.reason.size() > 80 || source.blockIndex < -1 ||
						!source.nifPath.starts_with("meshes\\"))
						continue;
					publication.missingShapes.push_back(std::move(source));
				}
			}
			for (const auto& item : root.at("shapes")) {
				auto shape = std::make_shared<Shape>();
				shape->identity = item.at("identity").get<uint64_t>();
				const auto& positions = item.at("positions");
				if (!positions.is_array() || positions.empty() || positions.size() > UINT32_MAX)
					throw std::invalid_argument("Invalid vertex count");
				shape->positions.reserve(positions.size());
				for (const auto& entry : positions) {
					const auto p = Triple(entry);
					shape->positions.push_back({ p[0], p[1], p[2] });
				}
				shape->indices = item.at("indices").get<std::vector<uint32_t>>();
				const auto& transform = item.at("transform");
				if (!transform.is_array() || transform.size() != shape->transform.size())
					throw std::invalid_argument("Invalid transform");
				for (size_t i = 0; i < shape->transform.size(); ++i)
					shape->transform[i] = transform[i].get<float>();
				const uint64_t length = uint64_t(shape->positions.size()) * sizeof(float3) +
				                        uint64_t(shape->indices.size()) * sizeof(uint32_t);
				if (!shape->identity || !identities.insert(shape->identity).second ||
					shape->indices.empty() || shape->indices.size() % 3 ||
					length > UINT64_MAX - publication.geometryBytes || !HasAvailableSystemMemory(length) ||
					!std::ranges::all_of(shape->positions, [](const float3& p) {
						return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
					}) ||
					!std::ranges::all_of(shape->transform, [](float value) { return std::isfinite(value); }) || !std::ranges::all_of(shape->indices, [&](uint32_t index) { return index < shape->positions.size(); }))
					throw std::invalid_argument("Companion geometry failed validation");
				publication.geometryBytes += length;
				publication.shapes.push_back(std::move(shape));
			}
			for (const auto& item : root.at("lights")) {
				try {
					CompanionLight record;
					record.referencePlugin = item.at("referencePlugin").get<std::string>();
					record.referenceLocal = item.at("referenceLocal").get<uint32_t>();
					record.basePlugin = item.at("basePlugin").get<std::string>();
					record.baseLocal = item.at("baseLocal").get<uint32_t>();
					const auto position = Triple(item.at("position"));
					const auto color = Triple(item.at("color"));
					record.value.position = float3{ position[0], position[1], position[2] };
					record.value.color = float3{ color[0], color[1], color[2] };
					record.value.radius = item.at("radius").get<float>();
					record.value.fade = item.at("fade").get<float>();
					record.value.emitterRadius = item.at("emitterRadius").get<float>();
					record.value.falloffExponent = item.at("falloffExponent").get<float>();
					record.value.flags = item.at("flags").get<uint32_t>();
					const auto& light = record.value;
					if (!std::isfinite(light.position.x) || !std::isfinite(light.position.y) || !std::isfinite(light.position.z) ||
						!std::isfinite(light.color.x) || !std::isfinite(light.color.y) || !std::isfinite(light.color.z) ||
						!std::isfinite(light.radius) || !std::isfinite(light.fade) ||
						!std::isfinite(light.emitterRadius) || !std::isfinite(light.falloffExponent))
						throw std::invalid_argument("Companion light failed validation");
					result.lights.push_back(std::move(record));
				} catch (const std::exception&) {
					// A malformed light must not discard independently validated geometry.
					++publication.rejectedLights;
				}
			}
			publication.validated = true;
			if (!publication.enumerationComplete || publication.failedShapes ||
				publication.eligibleShapes != publication.shapes.size())
				result.failure = std::format("Partial coverage: {} eligible shapes, {} extracted, {} unsupported; complete enumeration: {}",
					publication.eligibleShapes, publication.shapes.size(), publication.failedShapes,
					publication.enumerationComplete ? "yes" : "no");
			if (publication.rejectedLights) {
				if (publication.rejectionReasons.size() < 12)
					publication.rejectionReasons.emplace_back("Invalid companion light", publication.rejectedLights);
				const auto lightFailure = std::format("{} invalid companion light(s) skipped", publication.rejectedLights);
				result.failure = result.failure.empty() ? lightFailure : result.failure + "; " + lightFailure;
			}
		} catch (const std::exception& error) {
			result.failure = std::format("Companion payload rejected: {}", std::string(error.what()).substr(0, 240));
			result.publication.shapes.clear();
			result.publication.validated = false;
			result.publication.geometryBytes = 0;
			result.publication.enumerationComplete = false;
			result.lights.clear();
		}
		return result;
	}
}
