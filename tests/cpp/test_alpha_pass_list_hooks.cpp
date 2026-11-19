#include "EngineFixes/AlphaPassListHooks.h"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace AlphaPassList;

	struct NativeImage
	{
		std::uintptr_t rva;
		std::vector<std::uint8_t> bytes;
	};

	const std::array image{
		NativeImage{ 0x1347150, { 0x40, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF } },
		NativeImage{ 0x134A6F0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 } },
		NativeImage{ 0x1347320, { 0x40, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30 } },
		NativeImage{ 0x1347E60, { 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE } }
	};

	struct HookFixture
	{
		static constexpr std::uintptr_t Base = 0x140000000;
		static constexpr long Busy = 4317;
		std::array<void*, 4> originals{};
		std::array<Hook, 4> hooks{};
		std::vector<Hook> attachments;
		std::string calls;
		bool transaction{}, published{};
		long beginError{}, updateError{}, attachError{ 9 }, abortError{}, commitError{};
		std::size_t failedAttach{}, unreadableSite{ 4 }, modifiedSite{ 4 };

		HookFixture()
		{
			for (std::size_t index = 0; index < originals.size(); ++index) {
				originals[index] = reinterpret_cast<void*>(100 + index);
				hooks[index] = { &originals[index], reinterpret_cast<void*>(200 + index) };
			}
		}

		struct Operations
		{
			HookFixture& fixture;

			bool Read(std::uintptr_t address, std::span<std::uint8_t> output) const
			{
				fixture.calls += 'R';
				for (std::size_t index = 0; index < image.size(); ++index) {
					if (address != Base + image[index].rva)
						continue;
					REQUIRE(output.size() == image[index].bytes.size());
					if (index == fixture.unreadableSite)
						return false;
					std::copy(image[index].bytes.begin(), image[index].bytes.end(), output.begin());
					if (index == fixture.modifiedSite)
						output[0] ^= 1;
					return true;
				}
				FAIL("Unexpected native alpha entrypoint");
				return false;
			}

			long InvalidOperation() const { return Busy; }
			long Begin() const
			{
				fixture.calls += 'B';
				if (fixture.beginError == 0)
					fixture.transaction = true;
				return fixture.beginError;
			}

			long UpdateThread() const
			{
				fixture.calls += 'U';
				REQUIRE(fixture.transaction);
				return fixture.updateError;
			}

			long Attach(void** original, void* replacement) const
			{
				fixture.calls += 'A';
				REQUIRE(fixture.transaction);
				fixture.attachments.push_back({ original, replacement });
				return fixture.failedAttach == fixture.attachments.size() ? fixture.attachError : 0;
			}

			long Abort() const
			{
				fixture.calls += 'X';
				if (fixture.abortError == 0)
					fixture.transaction = false;
				return fixture.abortError;
			}

			long Commit() const
			{
				fixture.calls += 'C';
				REQUIRE(fixture.transaction);
				REQUIRE(fixture.attachments.size() == 4);
				fixture.transaction = false;
				if (fixture.commitError)
					return fixture.commitError;
				for (const auto& hook : fixture.attachments)
					*hook.original = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(*hook.original) + 0x100);
				fixture.published = true;
				return 0;
			}
		};

		InstallResult Install(bool vr = true, bool supported = true)
		{
			return InstallHooks(vr, supported, Base, hooks, Operations{ *this });
		}
	};
}

TEST_CASE("Alpha native hooks stay inactive outside verified VR", "[vr][alpha-list][hooks]")
{
	HookFixture fixture;
	CHECK(fixture.Install(false).status == InstallStatus::inactive);
	CHECK(fixture.calls.empty());
	CHECK(fixture.Install(true, false).status == InstallStatus::unsupported);
	CHECK(fixture.calls.empty());
	CHECK_FALSE(fixture.published);
}

TEST_CASE("Alpha hook admission validates all four native entrypoints before mutation", "[vr][alpha-list][hooks]")
{
	for (std::size_t site = 0; site < image.size(); ++site) {
		for (const bool unreadable : { false, true }) {
			CAPTURE(site, unreadable);
			HookFixture fixture;
			const auto before = fixture.originals;
			if (unreadable)
				fixture.unreadableSite = site;
			else
				fixture.modifiedSite = site;
			CHECK(fixture.Install().status == InstallStatus::modified);
			CHECK(fixture.calls == std::string(site + 1, 'R'));
			CHECK(fixture.originals == before);
			CHECK(fixture.attachments.empty());
			CHECK_FALSE(fixture.published);
		}
	}
}

TEST_CASE("Alpha hook installation preserves another owner's active transaction", "[vr][alpha-list][hooks]")
{
	HookFixture fixture;
	fixture.beginError = HookFixture::Busy;
	fixture.transaction = true;
	const auto result = fixture.Install();
	CHECK(result.status == InstallStatus::failed);
	CHECK(result.error == HookFixture::Busy);
	CHECK(fixture.calls == "RRRRB");
	CHECK(fixture.transaction);
	CHECK_FALSE(fixture.published);
}

TEST_CASE("Alpha hook installation aborts owned failed setup", "[vr][alpha-list][hooks]")
{
	SECTION("Begin failure")
	{
		HookFixture fixture;
		fixture.beginError = 8;
		const auto result = fixture.Install();
		CHECK(result.status == InstallStatus::failed);
		CHECK(result.error == 8);
		CHECK(fixture.calls == "RRRRBX");
		CHECK_FALSE(fixture.published);
	}
	SECTION("Thread update failure")
	{
		HookFixture fixture;
		fixture.updateError = 5;
		const auto result = fixture.Install();
		CHECK(result.status == InstallStatus::failed);
		CHECK(result.error == 5);
		CHECK(fixture.calls == "RRRRBUX");
		CHECK_FALSE(fixture.transaction);
		CHECK_FALSE(fixture.published);
	}
	SECTION("Abort failure has priority")
	{
		HookFixture fixture;
		fixture.updateError = 5;
		fixture.abortError = 13;
		const auto result = fixture.Install();
		CHECK(result.status == InstallStatus::failed);
		CHECK(result.error == 13);
		CHECK(fixture.calls == "RRRRBUX");
		CHECK_FALSE(fixture.published);
	}
}

TEST_CASE("Alpha failure at any attach leaves every native hook unpublished", "[vr][alpha-list][hooks]")
{
	for (std::size_t failed = 1; failed <= image.size(); ++failed) {
		CAPTURE(failed);
		HookFixture fixture;
		fixture.failedAttach = failed;
		const auto result = fixture.Install();
		CHECK(result.status == InstallStatus::failed);
		CHECK(result.error == fixture.attachError);
		CHECK(fixture.calls == "RRRRBU" + std::string(failed, 'A') + "X");
		CHECK_FALSE(fixture.transaction);
		CHECK_FALSE(fixture.published);
		for (std::size_t site = 0; site < image.size(); ++site)
			CHECK(fixture.originals[site] == reinterpret_cast<void*>(HookFixture::Base + image[site].rva));
	}
}

TEST_CASE("Alpha transaction publishes all four hooks including renderer retirement", "[vr][alpha-list][hooks]")
{
	HookFixture fixture;
	const auto result = fixture.Install();
	CHECK(result.status == InstallStatus::installed);
	CHECK(result.error == 0);
	CHECK(fixture.calls == "RRRRBUAAAAC");
	CHECK_FALSE(fixture.transaction);
	REQUIRE(fixture.published);
	REQUIRE(fixture.attachments.size() == 4);
	for (std::size_t site = 0; site < image.size(); ++site) {
		CHECK(fixture.originals[site] == reinterpret_cast<void*>(HookFixture::Base + image[site].rva + 0x100));
		CHECK(fixture.attachments[site].original == fixture.hooks[site].original);
		CHECK(fixture.attachments[site].replacement == fixture.hooks[site].replacement);
	}
}

TEST_CASE("Alpha commit failure leaves no partial installation", "[vr][alpha-list][hooks]")
{
	HookFixture fixture;
	fixture.commitError = 13;
	const auto result = fixture.Install();
	CHECK(result.status == InstallStatus::failed);
	CHECK(result.error == 13);
	CHECK(fixture.calls == "RRRRBUAAAAC");
	CHECK_FALSE(fixture.transaction);
	CHECK_FALSE(fixture.published);
	for (std::size_t site = 0; site < image.size(); ++site)
		CHECK(fixture.originals[site] == reinterpret_cast<void*>(HookFixture::Base + image[site].rva));
}

TEST_CASE("Alpha retirement invalidates membership at entry and scope exit", "[vr][alpha-list][hooks]")
{
	std::atomic_uint64_t generation{ 10 };
	{
		RetirementScope outer(generation);
		CHECK(generation == 11);
		{
			RetirementScope inner(generation);
			CHECK(generation == 12);
		}
		CHECK(generation == 13);
	}
	CHECK(generation == 14);
	const auto nativeException = [&] {
		RetirementScope lifetime(generation);
		CHECK(generation == 15);
		throw std::runtime_error("native consumer failed");
	};
	CHECK_THROWS_AS(nativeException(), std::runtime_error);
	CHECK(generation == 16);
}
