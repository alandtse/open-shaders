#include "Features/Upscaling/antilag2sdk.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <string>
#include <vector>

namespace
{
	struct Driver final : AMD::AntiLag2DX12::IAmdExtAntiLagApi
	{
		HRESULT stateResult = S_OK;
		HRESULT delayResult = S_OK;
		unsigned states = 0;
		unsigned delays = 0;
		unsigned releases = 0;
		bool enabled = false;
		unsigned maxFPS = 0;
		std::vector<std::string>* events = nullptr;

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
		ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
		ULONG STDMETHODCALLTYPE Release() override
		{
			++releases;
			if (events)
				events->emplace_back("release");
			return 0;
		}
		HRESULT UpdateAntiLagState(VOID* value) override
		{
			if (!value) {
				++delays;
				return delayResult;
			}
			++states;
			const auto& data = *static_cast<AMD::AntiLag2DX12::APIData_v1*>(value);
			CHECK(data.uiSize == sizeof(data));
			CHECK(data.uiVersion == 1);
			CHECK(data.sControlStr == nullptr);
			CHECK(data.uiControlStrLength == 0);
			CHECK(data.maxFPS == 0);
			if (SUCCEEDED(stateResult)) {
				enabled = data.eMode == 1;
				maxFPS = data.maxFPS;
			}
			return stateResult;
		}
	};

	CS::AntiLag2::ContextPtr SupportedContext(Driver& driver)
	{
		auto context = std::make_unique<AMD::AntiLag2DX12::Context>();
		context->m_pAntiLagAPI = &driver;
		return context;
	}
}

TEST_CASE("Anti-Lag rejects a missing driver context", "[antilag2]")
{
	AMD::AntiLag2DX12::Context empty{};
	CHECK(CS::AntiLag2::Update(nullptr, true) == E_NOINTERFACE);
	CHECK(CS::AntiLag2::Update(&empty, true) == E_NOINTERFACE);
}

TEST_CASE("Anti-Lag retries rejected state changes without caching them", "[antilag2]")
{
	Driver driver;
	auto context = SupportedContext(driver);
	driver.stateResult = E_FAIL;
	for (unsigned attempt = 1; attempt <= 2; ++attempt) {
		CHECK(CS::AntiLag2::Update(context.get(), true) == E_FAIL);
		CHECK(driver.states == attempt);
		CHECK(driver.delays == 0);
		CHECK_FALSE(context->m_enabled);
		CHECK(context->m_maxFPS == 0);
	}
	driver.stateResult = S_OK;
	REQUIRE(CS::AntiLag2::Update(context.get(), true) == S_OK);
	CHECK(driver.states == 3);
	CHECK(driver.delays == 1);
	CHECK(driver.enabled);
	CHECK(context->m_enabled);
	CHECK(CS::AntiLag2::Update(context.get(), true) == S_OK);
	CHECK(driver.states == 3);
	CHECK(driver.delays == 2);
	driver.stateResult = E_FAIL;
	CHECK(CS::AntiLag2::Update(context.get(), false) == E_FAIL);
	CHECK(context->m_enabled);
	driver.stateResult = S_OK;
	CHECK(CS::AntiLag2::Update(context.get(), false) == S_OK);
	CHECK_FALSE(context->m_enabled);
	CHECK_FALSE(driver.enabled);
	AMD::AntiLag2DX12::DeInitialize(context.get());
	CHECK(driver.releases == 1);
}

TEST_CASE("Anti-Lag preserves accepted state on delay failure and normalizes S_FALSE", "[antilag2]")
{
	Driver driver;
	auto context = SupportedContext(driver);
	driver.delayResult = E_FAIL;
	CHECK(CS::AntiLag2::Update(context.get(), true) == E_FAIL);
	CHECK(context->m_enabled);
	CHECK(context->m_maxFPS == 0);
	CHECK(driver.states == 1);
	driver.delayResult = S_FALSE;
	CHECK(CS::AntiLag2::Update(context.get(), true) == S_OK);
	CHECK(driver.states == 1);
	CHECK(driver.delays == 2);
	AMD::AntiLag2DX12::DeInitialize(context.get());
}

TEST_CASE("Anti-Lag clears a cached FPS cap through a checked state change", "[antilag2]")
{
	Driver driver;
	auto context = SupportedContext(driver);
	context->m_enabled = true;
	context->m_maxFPS = 120;
	driver.stateResult = E_FAIL;
	CHECK(CS::AntiLag2::Update(context.get(), true) == E_FAIL);
	CHECK(context->m_maxFPS == 120);
	driver.stateResult = S_OK;
	CHECK(CS::AntiLag2::Update(context.get(), true) == S_OK);
	CHECK(context->m_maxFPS == 0);
	CHECK(driver.maxFPS == 0);
	AMD::AntiLag2DX12::DeInitialize(context.get());
}

TEST_CASE("Anti-Lag initializes before installing optional input interception", "[antilag2]")
{
	const auto result = GENERATE(S_OK, S_FALSE, E_FAIL);
	const auto partial = GENERATE(false, true);
	const auto hookSucceeds = GENERATE(false, true);
	Driver driver;
	CS::AntiLag2::ContextPtr context;
	unsigned initializationCalls = 0;
	CHECK(CS::AntiLag2::InitializeContext(context, nullptr,
			  [&](AMD::AntiLag2DX12::Context* candidate, ID3D12Device* device) {
				  ++initializationCalls;
				  CHECK(device == nullptr);
				  CHECK_FALSE(candidate->m_enabled);
				  CHECK(candidate->m_maxFPS == 0);
				  if (result == S_OK || partial)
					  candidate->m_pAntiLagAPI = &driver;
				  return result;
			  }) == result);
	CHECK(initializationCalls == 1);
	CHECK(bool(context) == (result == S_OK));
	CHECK(driver.releases == unsigned(result != S_OK && partial));
	unsigned hookCalls = 0;
	const bool installed = CS::AntiLag2::InstallInputHook(context, [&] {
		++hookCalls;
		return hookSucceeds;
	});
	CHECK(hookCalls == unsigned(result == S_OK));
	CHECK(installed == (result == S_OK && hookSucceeds));
	CHECK(bool(context) == installed);
	CHECK(driver.releases == unsigned((result != S_OK && partial) || (result == S_OK && !hookSucceeds)));
	if (context) {
		CHECK(CS::AntiLag2::Shutdown(context, [] { return true; }, [] {}));
		CHECK_FALSE(context);
		CHECK(driver.releases == 1);
	}
}

TEST_CASE("Anti-Lag shutdown does no work without an eligible context", "[antilag2]")
{
	CS::AntiLag2::ContextPtr context;
	unsigned callbacks = 0;
	CHECK(CS::AntiLag2::Shutdown(context, [&] { ++callbacks; return true; }, [&] { ++callbacks; }));
	CHECK(callbacks == 0);
}

TEST_CASE("Anti-Lag drains before disabling the handshake and releasing its interface", "[antilag2]")
{
	Driver driver;
	std::vector<std::string> events;
	driver.events = &events;
	auto context = SupportedContext(driver);
	CHECK(CS::AntiLag2::Shutdown(context, [&] { events.emplace_back("drain"); return true; }, [&] { events.emplace_back("disable"); }));
	CHECK(events == std::vector<std::string>{ "drain", "disable", "release" });
	CHECK_FALSE(context);
	CHECK(driver.releases == 1);
}

TEST_CASE("Anti-Lag retains the context and interface when presentation cannot drain", "[antilag2]")
{
	Driver driver;
	auto context = SupportedContext(driver);
	auto* retained = context.get();
	unsigned disabled = 0;
	CHECK_FALSE(CS::AntiLag2::Shutdown(context, [] { return false; }, [&] { ++disabled; }));
	CHECK_FALSE(context);
	CHECK(disabled == 0);
	CHECK(driver.releases == 0);
	CHECK(retained->m_pAntiLagAPI == &driver);
	AMD::AntiLag2DX12::DeInitialize(retained);
	delete retained;
}
