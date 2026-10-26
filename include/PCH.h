#pragma once

#include <mutex>
#include <set>
#include <shared_mutex>
#include <new>
#include <format>
#include <source_location>
#include <string_view>
#include <system_error>
void* operator new[](size_t size, const char* pName, int flags, unsigned debugFlags, const char* file, int line);
void* operator new[](size_t size, size_t alignment, size_t alignmentOffset, const char* pName, int flags,
	unsigned debugFlags, const char* file, int line);

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <xbyak/xbyak.h>

#include <detours/detours.h>

#ifdef NDEBUG
#	include <spdlog/sinks/basic_file_sink.h>
#else
#	include <spdlog/sinks/msvc_sink.h>
#endif

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#ifndef TRACY_SUPPORT
	#undef TRACY_ENABLE
#endif

using namespace std::literals;

// Defined in src/Utils/VTableHookFallback.cpp
namespace Util
{
	std::uintptr_t VTableHookFallback(void* a_object, std::size_t a_idx, void* a_thunk, LONG a_detourError);
}

namespace stl
{
	using namespace SKSE::stl;

	template <class T, std::size_t Size = 5>
	void write_thunk_call(std::uintptr_t a_src)
	{
		auto& trampoline = SKSE::GetTrampoline();
		if (Size == 6) {
			T::func = *(uintptr_t*)trampoline.write_call<6>(a_src, T::thunk);
		} else {
			T::func = trampoline.write_call<Size>(a_src, T::thunk);
		}
	}

	template <class F, size_t index, class T>
	void write_vfunc()
	{
		REL::Relocation<std::uintptr_t> vtbl{ F::VTABLE[index] };
		T::func = vtbl.write_vfunc(T::size, T::thunk);
	}

	template <std::size_t idx, class T>
	void write_vfunc(REL::VariantID id)
	{
		REL::Relocation<std::uintptr_t> vtbl{ id };
		T::func = vtbl.write_vfunc(idx, T::thunk);
	}

	template <std::size_t idx, class T>
	void write_vfunc(REL::VariantOffset offset)
	{
		REL::Relocation<std::uintptr_t> vtbl{ offset };
		T::func = vtbl.write_vfunc(idx, T::thunk);
	}

	template <class T>
	void write_thunk_jmp(std::uintptr_t a_src)
	{
		auto& trampoline = SKSE::GetTrampoline();
		T::func = trampoline.write_branch<5>(a_src, T::thunk);
	}

	template <class F, class T>
	void write_vfunc()
	{
		write_vfunc<F, 0, T>();
	}

	inline LONG detour_attach(PVOID* a_target, PVOID a_thunk)
	{
		LONG result = DetourTransactionBegin();
		if (result != NO_ERROR) {
			// Begin can claim the transaction before failing; release it for later hooks.
			DetourTransactionAbort();
			return result;
		}
		// A failed thread update must abort too, or the transaction commits and skips the fallback.
		result = DetourUpdateThread(GetCurrentThread());
		if (result == NO_ERROR)
			result = DetourAttach(a_target, a_thunk);
		if (result == NO_ERROR)
			result = DetourTransactionCommit();
		else
			DetourTransactionAbort();
		return result;
	}

	// Attaching the same thunk to the same function twice chains the thunk into itself and recurses forever.
	inline bool detour_registry(std::uintptr_t a_target, PVOID a_thunk, bool a_record)
	{
		static std::mutex lock;
		static std::set<std::pair<std::uintptr_t, std::uintptr_t>> installed;
		const std::pair key{ a_target, reinterpret_cast<std::uintptr_t>(a_thunk) };
		std::scoped_lock guard(lock);
		if (a_record)
			installed.insert(key);
		return installed.contains(key);
	}

	inline bool detour_is_installed(std::uintptr_t a_target, PVOID a_thunk)
	{
		return detour_registry(a_target, a_thunk, false);
	}

	inline void detour_record_installed(std::uintptr_t a_target, PVOID a_thunk)
	{
		detour_registry(a_target, a_thunk, true);
	}

	template <class T>
	long detour_thunk(std::uintptr_t a_address)
	{
		if (detour_is_installed(a_address, reinterpret_cast<PVOID>(T::thunk)))
			return NO_ERROR;
		T::func = a_address;
		const auto result = detour_attach(reinterpret_cast<PVOID*>(&T::func), reinterpret_cast<PVOID>(T::thunk));
		if (result != NO_ERROR) {
			SKSE::log::error("Detour attach failed at 0x{:X} (error {})", a_address, result);
			// Leave func null so callers checking it see the hook is not installed.
			T::func = 0;
		} else {
			detour_record_installed(a_address, reinterpret_cast<PVOID>(T::thunk));
		}
		return result;
	}

	template <class T>
	long detour_thunk(REL::RelocationID a_relId)
	{
		return detour_thunk<T>(a_relId.address());
	}

	template <class T>
	void detour_thunk_ignore_func(REL::RelocationID a_relId)
	{
		if (detour_is_installed(a_relId.address(), reinterpret_cast<PVOID>(T::thunk)))
			return;
		auto target = reinterpret_cast<PVOID>(a_relId.address());
		if (const auto result = detour_attach(&target, reinterpret_cast<PVOID>(T::thunk)); result != NO_ERROR)
			SKSE::log::error("Detour attach failed at 0x{:X} (error {})", a_relId.address(), result);
		else
			detour_record_installed(a_relId.address(), reinterpret_cast<PVOID>(T::thunk));
	}

	template <std::size_t idx, class T>
	void detour_vfunc(void* target)
	{
		auto vtable = *reinterpret_cast<uintptr_t**>(target);
		const auto original = vtable[idx];
		if (original == reinterpret_cast<std::uintptr_t>(T::thunk) || detour_is_installed(original, reinterpret_cast<PVOID>(T::thunk)))
			return;
		T::func = original;
		const auto result = detour_attach(reinterpret_cast<PVOID*>(&T::func), reinterpret_cast<PVOID>(T::thunk));
		if (result == NO_ERROR)
			detour_record_installed(original, reinterpret_cast<PVOID>(T::thunk));
		else
			T::func = Util::VTableHookFallback(target, idx, reinterpret_cast<PVOID>(T::thunk), result);
	}
}

namespace logger = SKSE::log;

namespace util
{
	using SKSE::stl::report_and_fail;
}

#include "Plugin.h"
#include <wrl/client.h>

#include <DirectXColors.h>
#include <DirectXMath.h>

namespace DX
{
	// Helper class for COM exceptions
	class com_exception : public std::exception
	{
	public:
		explicit com_exception(HRESULT hr, const std::source_location& where = std::source_location::current()) :
			result(hr)
		{
			std::string_view file = where.file_name();
			file.remove_prefix(file.find_last_of("\\/") == std::string_view::npos ? 0 : file.find_last_of("\\/") + 1);
			message = std::format("{} (HRESULT 0x{:08X}) at {}:{}", std::system_category().message(hr), static_cast<uint32_t>(hr), file, where.line());
		}

		const char* what() const noexcept override { return message.c_str(); }

	private:
		HRESULT result;
		std::string message;
	};

	// Helper utility converts D3D API failures into exceptions that name the failing call site.
	inline void ThrowIfFailed(HRESULT hr, const std::source_location& where = std::source_location::current())
	{
		if (FAILED(hr)) {
			throw com_exception(hr, where);
		}
	}
}

#include <ClibUtil/distribution.hpp>
#include <ClibUtil/editorID.hpp>
#include <ClibUtil/numeric.hpp>
#include <ClibUtil/rng.hpp>
#include <ClibUtil/simpleINI.hpp>

#include <imgui.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include <magic_enum/magic_enum.hpp>

#include <EASTL/algorithm.h>
#include <EASTL/array.h>
#include <EASTL/bitset.h>
#include <EASTL/bonus/fixed_ring_buffer.h>
#include <EASTL/fixed_list.h>
#include <EASTL/fixed_slist.h>
#include <EASTL/fixed_vector.h>
#include <EASTL/functional.h>
#include <EASTL/hash_set.h>
#include <EASTL/map.h>
#include <EASTL/numeric_limits.h>
#include <EASTL/set.h>
#include <EASTL/shared_ptr.h>
#include <EASTL/string.h>
#include <EASTL/tuple.h>
#include <EASTL/unique_ptr.h>
#include <EASTL/unordered_map.h>
#include <EASTL/vector.h>

#include <ankerl/unordered_dense.h>
template <>
struct ankerl::unordered_dense::hash<std::string>
{
	using is_transparent = void;  // enable heterogeneous overloads
	using is_avalanching = void;  // mark class as high quality avalanching hash

	[[nodiscard]] auto operator()(std::string_view str) const noexcept -> uint64_t
	{
		return ankerl::unordered_dense::hash<std::string_view>{}(str);
	}
};

#include "SimpleMath.h"

using float2 = DirectX::SimpleMath::Vector2;
using float3 = DirectX::SimpleMath::Vector3;
using float4 = DirectX::SimpleMath::Vector4;
using float4x4 = DirectX::SimpleMath::Matrix;
using uint = uint32_t;

#include "Globals.h"
#include "Profiler.h"
#include "Util.h"
#include "Feature.h"
#include "Buffer.h"
#include "EngineFix.h"
