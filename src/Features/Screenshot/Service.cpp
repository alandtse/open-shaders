#include "Features/Screenshot/Service.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <format>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace
{
	std::string StringValueOrEmpty(const nlohmann::json& a_object, std::string_view a_field)
	{
		const auto found = a_object.find(a_field);
		return found != a_object.end() && found->is_string() ? found->get<std::string>() : std::string{};
	}
}

namespace OS::Capture
{
	ServiceFoundation::ServiceFoundation(ContractDescriptor a_contract, ServiceLimits a_limits) :
		contract(std::move(a_contract)),
		limits(a_limits),
		sessionId(NewId())
	{
		if (contract.name.empty())
			throw std::invalid_argument("API contract name cannot be empty");
		limits.maximumCommands = std::max<std::size_t>(limits.maximumCommands, 1);
	}

	ServiceFoundation::json ServiceFoundation::Dispatch(
		const json& a_request,
		const Handler& a_handler,
		const ReceiptLookup& a_receiptLookup)
	{
		if (!a_request.is_object())
			return MakeError(json::object(), "invalid_request", "arguments must be a JSON object");
		bool compatibleContract = false;
		if (a_request.contains("contractMajor")) {
			const auto& value = a_request["contractMajor"];
			if (value.is_number_unsigned())
				compatibleContract = value.get<uint64_t>() == contract.major;
			else if (value.is_number_integer()) {
				const auto signedValue = value.get<int64_t>();
				compatibleContract = signedValue >= 0 && static_cast<uint64_t>(signedValue) == contract.major;
			}
		}
		if (!compatibleContract) {
			return MakeError(
				a_request,
				"unsupported_contract_version",
				std::format("contractMajor must be {}", contract.major),
				"validation",
				false,
				"contractMajor");
		}
		if (!a_request.contains("action") || !a_request["action"].is_string() || a_request["action"].get_ref<const std::string&>().empty())
			return MakeError(a_request, "missing_field", "action is required", "validation", false, "action");
		if (!a_request.contains("clientId") || !a_request["clientId"].is_string())
			return MakeError(a_request, "invalid_field", "clientId must contain 1 to 128 characters", "validation", false, "clientId");
		if (!a_request.contains("commandId") || !a_request["commandId"].is_string())
			return MakeError(a_request, "invalid_field", "commandId must contain 1 to 128 characters", "validation", false, "commandId");

		const auto& clientId = a_request["clientId"].get_ref<const std::string&>();
		const auto& commandId = a_request["commandId"].get_ref<const std::string&>();
		if (clientId.empty() || clientId.size() > 128)
			return MakeError(a_request, "invalid_field", "clientId must contain 1 to 128 characters", "validation", false, "clientId");
		if (commandId.empty() || commandId.size() > 128)
			return MakeError(a_request, "invalid_field", "commandId must contain 1 to 128 characters", "validation", false, "commandId");

		const CommandKey key{ clientId, commandId };
		const std::string canonical = a_request.dump();
		constexpr std::size_t maximumCommandBytes = 16 * 1024;
		if (canonical.size() > maximumCommandBytes)
			return MakeError(a_request, "invalid_request", "command exceeds 16 KiB");
		std::optional<CommandRecord> replay;
		bool conflict = false;
		bool pending = false;
		{
			std::lock_guard lock(mutex);
			TrimLocked(std::chrono::steady_clock::now());
			if (const auto found = commands.find(key); found != commands.end()) {
				conflict = found->second.canonicalRequest != canonical;
				pending = !conflict && !found->second.completed;
				if (!conflict && !pending)
					replay = found->second;
			} else {
				CommandRecord command;
				command.canonicalRequest = canonical;
				commands.emplace(key, std::move(command));
				commandOrder.push_back(key);
			}
		}

		if (conflict)
			return MakeError(a_request, "idempotency_conflict", "clientId and commandId were already used with different arguments");
		if (pending)
			return MakeError(a_request, "command_in_progress", "an identical command is still being processed", "dispatch", true);
		if (replay) {
			json response;
			if (!replay->requestId.empty() && a_receiptLookup) {
				response = MakeEnvelope(a_request, true);
				response["result"] = a_receiptLookup(replay->requestId);
				if (response["result"].is_null())
					return MakeError(a_request, "receipt_expired", "the sequence receipt is no longer retained");
			}
			if (response.is_null() || !response.contains("result") || response["result"].is_null())
				response = replay->response;
			if (response.contains("result") && response["result"].is_object())
				response["result"]["idempotentReplay"] = true;
			else
				response["idempotentReplay"] = true;
			return response;
		}

		json response;
		try {
			response = a_handler(a_request);
		} catch (const json::exception& e) {
			response = MakeError(a_request, "invalid_request", e.what());
		} catch (const std::invalid_argument& e) {
			response = MakeError(a_request, "invalid_request", e.what());
		} catch (const std::exception& e) {
			response = MakeError(a_request, "internal_error", e.what(), "dispatch", true);
		} catch (...) {
			response = MakeError(a_request, "internal_error", "unknown API dispatch error", "dispatch", true);
		}

		std::lock_guard lock(mutex);
		if (auto found = commands.find(key); found != commands.end()) {
			const bool retryableFailure = !response.value("ok", false) && response.contains("error") &&
			                              response["error"].is_object() && response["error"].value("retryable", false);
			if (retryableFailure) {
				commands.erase(found);
				std::erase(commandOrder, key);
				TrimLocked(std::chrono::steady_clock::now());
				return response;
			}
			found->second.response = response;
			found->second.completed = true;
			if (response.value("ok", false) && response.contains("result") && response["result"].is_object())
				found->second.requestId = response["result"].value("requestId", std::string{});
		}
		TrimLocked(std::chrono::steady_clock::now());
		return response;
	}

	ServiceFoundation::json ServiceFoundation::MakeEnvelope(const json& a_request, bool a_ok) const
	{
		json response = {
			{ "ok", a_ok },
			{ "contract", {
							  { "name", contract.name },
							  { "major", contract.major },
							  { "minor", contract.minor },
							  { "schemaRevision", contract.schemaRevision },
						  } },
			{ "command", {
							 { "action", StringValueOrEmpty(a_request, "action") },
							 { "clientId", StringValueOrEmpty(a_request, "clientId") },
							 { "commandId", StringValueOrEmpty(a_request, "commandId") },
						 } },
			{ "timestampUtc", TimestampUtc() },
		};
		response["server"] = { { "sessionId", sessionId } };
		return response;
	}

	ServiceFoundation::json ServiceFoundation::MakeError(
		const json& a_request,
		std::string_view a_code,
		std::string_view a_message,
		std::string_view a_phase,
		bool a_retryable,
		std::string_view a_field,
		std::string_view a_requestId) const
	{
		auto response = MakeEnvelope(a_request, false);
		response["error"] = {
			{ "code", a_code },
			{ "message", a_message },
			{ "phase", a_phase },
			{ "retryable", a_retryable },
			{ "field", a_field.empty() ? json(nullptr) : json(a_field) },
			{ "requestId", a_requestId.empty() ? json(nullptr) : json(a_requestId) },
			{ "details", json::object() },
		};
		return response;
	}

	void ServiceFoundation::TrimLocked(std::chrono::steady_clock::time_point a_now)
	{
		for (auto position = commandOrder.begin(); position != commandOrder.end();) {
			const auto found = commands.find(*position);
			if (found == commands.end() || (found->second.completed && a_now - found->second.createdAt >= limits.commandRetention)) {
				if (found != commands.end())
					commands.erase(found);
				position = commandOrder.erase(position);
			} else {
				++position;
			}
		}
		while (commandOrder.size() > limits.maximumCommands) {
			const auto found = commands.find(commandOrder.front());
			if (found != commands.end() && !found->second.completed)
				break;
			commands.erase(commandOrder.front());
			commandOrder.pop_front();
		}
	}

	std::string ServiceFoundation::NewId()
	{
		static std::atomic_uint64_t sequence{ 1 };
		thread_local std::mt19937_64 random([] {
			std::random_device source;
			std::seed_seq seed{ source(), source(), source(), source() };
			return std::mt19937_64(seed);
		}());
		const auto count = sequence.fetch_add(1, std::memory_order_relaxed);
		const auto ticks = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
		std::string value = std::format("{:016x}{:016x}", random() ^ ticks, random() ^ count);
		value.insert(8, "-");
		value.insert(13, "-");
		value.insert(18, "-");
		value.insert(23, "-");
		return value;
	}

	std::string ServiceFoundation::TimestampUtc()
	{
		const auto now = std::chrono::system_clock::now();
		const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
		const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
		const std::time_t value = std::chrono::system_clock::to_time_t(now);
		std::tm utc{};
#ifdef _WIN32
		gmtime_s(&utc, &value);
#else
		gmtime_r(&value, &utc);
#endif
		std::ostringstream stream;
		stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << millis << 'Z';
		return stream.str();
	}

}
