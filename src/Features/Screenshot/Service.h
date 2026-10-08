#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace OS::Capture
{
	struct ContractDescriptor
	{
		std::string name;
		uint32_t major = 1;
		uint32_t minor = 0;
		uint32_t schemaRevision = 1;
	};

	struct ServiceLimits
	{
		std::size_t maximumCommands = 1024;
		std::chrono::steady_clock::duration commandRetention = std::chrono::hours(1);
	};

	/** Versioned command envelopes, process identity and bounded idempotency receipts. */
	class ServiceFoundation
	{
	public:
		using json = nlohmann::json;
		using Handler = std::function<json(const json&)>;
		using ReceiptLookup = std::function<json(std::string_view)>;

		/** Creates process-local identity and bounded command retention. */
		explicit ServiceFoundation(ContractDescriptor a_contract, ServiceLimits a_limits = {});
		/** Validates commands; read-only calls can bypass mutation receipt retention. */
		json Dispatch(const json& a_request, const Handler& a_handler, const ReceiptLookup& a_receiptLookup = {}, bool a_retain = true);
		/** Adds version, identity and timestamp metadata to a response. */
		json MakeEnvelope(const json& a_request, bool a_ok) const;
		/** Returns a structured failure with explicit retry semantics. */
		json MakeError(
			const json& a_request,
			std::string_view a_code,
			std::string_view a_message,
			std::string_view a_phase = "validation",
			bool a_retryable = false,
			std::string_view a_field = {},
			std::string_view a_requestId = {}) const;

		/** Identifies this controller lifetime independently of build metadata. */
		const std::string& SessionId() const noexcept { return sessionId; }

		/** Generates a process-unique request or session identifier. */
		static std::string NewId();
		/** Returns a UTC timestamp with millisecond precision. */
		static std::string TimestampUtc();

	private:
		struct CommandKey
		{
			std::string clientId;
			std::string commandId;

			bool operator==(const CommandKey&) const = default;
		};

		struct CommandKeyHash
		{
			std::size_t operator()(const CommandKey& a_key) const noexcept
			{
				const auto clientHash = std::hash<std::string>{}(a_key.clientId);
				const auto commandHash = std::hash<std::string>{}(a_key.commandId);
				return clientHash ^ (commandHash + 0x9e3779b9u + (clientHash << 6u) + (clientHash >> 2u));
			}
		};

		struct CommandRecord
		{
			std::string canonicalRequest;
			std::string requestId;
			json response = json::object();
			bool completed = false;
			std::chrono::steady_clock::time_point createdAt = std::chrono::steady_clock::now();
		};

		ContractDescriptor contract;
		ServiceLimits limits;
		std::string sessionId;
		mutable std::mutex mutex;
		std::unordered_map<CommandKey, CommandRecord, CommandKeyHash> commands;
		std::deque<CommandKey> commandOrder;

		void TrimLocked(std::chrono::steady_clock::time_point a_now, bool a_reserveSlot = false);
		json InvokeHandler(const json& a_request, const Handler& a_handler) const;
	};
}
