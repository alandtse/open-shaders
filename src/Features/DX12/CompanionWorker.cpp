#include "CompanionWorker.h"

#include <Windows.h>

#include <charconv>
#include <format>
#include <fstream>
#include <cmath>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace DX12
{
	namespace
	{
		constexpr uint64_t kMaximumTransferBytes = 192ull * 1024 * 1024;
		constexpr DWORD kPreparationDeadlineMs = 90000;

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

		bool InvokeCompanion(const CompanionRequest& task, const std::filesystem::path& destination,
			uint64_t expectedGeneration, const std::atomic<uint64_t>& generation, std::stop_token stop)
		{
			const auto executable = std::filesystem::absolute(
				L"Data/SKSE/Plugins/CommunityShaders/DX12Resources/bin/DX12ResourcesPrep.exe");
			const auto data = std::filesystem::absolute(L"Data");
			const auto plugins = PluginsFile();
			std::error_code ec;
			if (!std::filesystem::is_regular_file(executable, ec) ||
				!std::filesystem::is_regular_file(plugins, ec))
				return false;

			std::wstring command = Quote(executable.wstring());
			for (const auto& argument : std::vector<std::wstring>{
					 L"--tile", data.wstring(), plugins.wstring(),
					 std::wstring(task.scopePlugin.begin(), task.scopePlugin.end()),
					 std::format(L"{:X}", task.localScopeID),
					 std::to_wstring(task.key.x), std::to_wstring(task.key.y),
					 std::to_wstring(task.size), task.worldspace ? L"world" : L"cell", destination.wstring() }) {
				command += L' ';
				command += Quote(argument);
			}
			HANDLE job = CreateJobObjectW(nullptr, nullptr);
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
			limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
				if (job)
					CloseHandle(job);
				return false;
			}
			STARTUPINFOW startup{};
			startup.cb = sizeof(startup);
			PROCESS_INFORMATION process{};
			const BOOL launched = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
				CREATE_NO_WINDOW | (task.predictive ? IDLE_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS) | CREATE_SUSPENDED,
				nullptr, nullptr, &startup, &process);
			if (!launched) {
				CloseHandle(job);
				return false;
			}
			const bool assigned = AssignProcessToJobObject(job, process.hProcess) &&
			                      ResumeThread(process.hThread) != DWORD(-1);
			CloseHandle(process.hThread);
			if (!assigned) {
				TerminateJobObject(job, 1);
				CloseHandle(process.hProcess);
				CloseHandle(job);
				return false;
			}
			const uint64_t started = GetTickCount64();
			bool success = false;
			while (!stop.stop_requested() && generation.load(std::memory_order_acquire) == expectedGeneration &&
				   GetTickCount64() - started < kPreparationDeadlineMs) {
				const DWORD wait = WaitForSingleObject(process.hProcess, 50);
				if (wait == WAIT_OBJECT_0) {
					DWORD code = 1;
					success = GetExitCodeProcess(process.hProcess, &code) && code == 0;
					break;
				}
				if (wait != WAIT_TIMEOUT)
					break;
			}
			if (!success)
				TerminateJobObject(job, 1);
			CloseHandle(process.hProcess);
			CloseHandle(job);
			return success;
		}

		std::array<float, 3> Triple(const json& values)
		{
			if (!values.is_array() || values.size() != 3)
				throw std::invalid_argument("Invalid point");
			return { values[0].get<float>(), values[1].get<float>(), values[2].get<float>() };
		}
	}

	CompanionWorker::CompanionWorker() : worker([this](std::stop_token stop) { Run(stop); }) {}

	CompanionWorker::~CompanionWorker()
	{
		worker.request_stop();
		wake.notify_all();
	}

	bool CompanionWorker::Idle() const
	{
		std::scoped_lock lock(mutex);
		return !running && !queued && !completed;
	}

	bool CompanionWorker::Enqueue(CompanionRequest request)
	{
		std::scoped_lock lock(mutex);
		if (running || queued || completed)
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
		generation.fetch_add(1, std::memory_order_acq_rel);
		std::scoped_lock lock(mutex);
		queued.reset();
		completed.reset();
	}

	void CompanionWorker::Run(std::stop_token stop)
	{
		while (!stop.stop_requested()) {
			CompanionRequest task;
			uint64_t epoch = 0;
			{
				std::unique_lock lock(mutex);
				wake.wait(lock, stop, [&] { return queued.has_value(); });
				if (stop.stop_requested())
					break;
				task = std::move(*queued);
				queued.reset();
				running = true;
				epoch = generation.load(std::memory_order_acquire);
			}
			auto result = Prepare(task, epoch, stop);
			{
				std::scoped_lock lock(mutex);
				running = false;
				if (!stop.stop_requested() && epoch == generation.load(std::memory_order_acquire))
					completed = std::move(result);
			}
		}
	}

	CompanionResult CompanionWorker::Prepare(const CompanionRequest& task, uint64_t epoch, std::stop_token stop)
	{
		CompanionResult result;
		result.publication.key = task.key;
		const auto work = std::filesystem::absolute(L"Data/SKSE/Plugins/CommunityShaders/DX12Resources/work");
		std::error_code ec;
		std::filesystem::create_directories(work, ec);
		if (ec)
			return result;
		RemoveStaleSessions(work);
		const auto folder = work / std::format(L"session-{}-{}", GetCurrentProcessId(), GetTickCount64());
		if (!std::filesystem::create_directory(folder, ec) || ec)
			return result;
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
		if (!InvokeCompanion(task, file, epoch, generation, stop))
			return result;
		try {
			std::ifstream stream(file, std::ios::binary | std::ios::ate);
			if (!stream || stream.tellg() <= 0 || uint64_t(stream.tellg()) > kMaximumTransferBytes)
				return result;
			stream.seekg(0);
			const auto root = json::parse(stream);
			if (root.at("version").get<uint32_t>() != 1 ||
				!root.at("shapes").is_array() || root.at("shapes").size() > 384 ||
				!root.at("lights").is_array() || root.at("lights").size() > 65536)
				return result;
			auto& publication = result.publication;
			constexpr uint64_t kMaximumTileBytes = 64ull * 1024 * 1024;
			std::unordered_set<uint64_t> identities;
			publication.eligibleShapes = root.at("eligibleShapes").get<uint32_t>();
			publication.failedShapes = root.at("failedShapes").get<uint32_t>();
			publication.enumerationComplete = root.at("enumerationComplete").get<bool>();
			for (const auto& item : root.at("shapes")) {
				auto shape = std::make_shared<Shape>();
				shape->identity = item.at("identity").get<uint64_t>();
				const auto& positions = item.at("positions");
				if (!positions.is_array() || positions.empty() || positions.size() > 4'000'000)
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
					length > kMaximumTileBytes - publication.geometryBytes ||
					!std::ranges::all_of(shape->positions, [](const float3& p) {
						return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
					}) ||
					!std::ranges::all_of(shape->transform, [](float value) { return std::isfinite(value); }) ||
					!std::ranges::all_of(shape->indices, [&](uint32_t index) { return index < shape->positions.size(); }))
					throw std::invalid_argument("Companion geometry failed validation");
				publication.geometryBytes += length;
				publication.shapes.push_back(std::move(shape));
			}
			for (const auto& item : root.at("lights")) {
				CompanionLight record;
				record.referencePlugin = item.at("referencePlugin").get<std::string>();
				record.referenceLocal = item.at("referenceLocal").get<uint32_t>();
				record.basePlugin = item.at("basePlugin").get<std::string>();
				record.baseLocal = item.at("baseLocal").get<uint32_t>();
				const auto position = Triple(item.at("position"));
				const auto color = Triple(item.at("color"));
				record.value.position = { position[0], position[1], position[2] };
				record.value.color = { color[0], color[1], color[2] };
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
			}
			publication.validated = true;
		} catch (const std::exception&) {
			result.publication.shapes.clear();
			result.publication.validated = false;
			result.publication.geometryBytes = 0;
			result.publication.enumerationComplete = false;
			result.lights.clear();
		}
		return result;
	}
}
