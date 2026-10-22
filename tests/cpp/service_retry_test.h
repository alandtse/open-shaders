#pragma once

#include <stdexcept>

template <class Service, class Check>
void CheckRetryableCommands(Service& service, Check check)
{
	using json = typename Service::json;
	const json command{ { "contractMajor", 1 }, { "clientId", "retry-test" }, { "commandId", "retry" }, { "action", "start" } };
	int attempts = 0;
	auto handler = [&](const json& args) {
		++attempts;
		if (attempts < 3)
			return service.MakeError(args, "busy", "source is occupied", "dispatch", true);
		auto result = service.MakeEnvelope(args, true);
		result["result"] = { { "requestId", "accepted" } };
		return result;
	};
	check(service.Dispatch(command, handler)["error"]["retryable"] == true, "First rejection must be retryable");
	check(service.Dispatch(command, handler)["error"]["retryable"] == true, "Repeated rejection must remain retryable");
	check(service.Dispatch(command, handler)["ok"] == true && attempts == 3, "Same-ID retry did not execute after recovery");
	check(service.Dispatch(command, handler)["result"]["idempotentReplay"] == true && attempts == 3, "Accepted capture was executed twice");

	auto thrown = command;
	thrown["commandId"] = "exception";
	int nestedCalls = 0;
	auto fails = [&](const json& args) -> json {
		auto pending = service.Dispatch(args, [&](const json&) { ++nestedCalls; return json::object(); });
		check(pending["error"]["code"] == "command_in_progress", "Concurrent command lost its reservation");
		throw std::runtime_error("temporary storage failure");
	};
	check(service.Dispatch(thrown, fails)["error"]["retryable"] == true, "Transient exception must be retryable");
	check(nestedCalls == 0, "Pending command executed concurrently");
	check(service.Dispatch(thrown, handler)["ok"] == true && attempts == 4, "Exception poisoned subsequent retry");

	auto rejected = command;
	rejected["commandId"] = "permanent";
	int rejectedCalls = 0;
	auto invalid = [&](const json& args) {
		++rejectedCalls;
		return service.MakeError(args, "invalid_request", "invalid settings");
	};
	service.Dispatch(rejected, invalid);
	const auto replay = service.Dispatch(rejected, invalid);
	check(replay["error"]["retryable"] == false && rejectedCalls == 1, "Permanent rejection was retried");
	auto conflict = command;
	conflict["changed"] = true;
	check(service.Dispatch(conflict, handler)["error"]["code"] == "idempotency_conflict", "Accepted command lost argument protection");
}
