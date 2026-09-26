#pragma once

#include <Windows.h>
#include <memory>
#include <type_traits>

namespace Util
{
	/**
	 * @brief Runs a_fn(a_context) under SEH; false plus the exception code when it faults.
	 *        A fault skips destructors in a_fn's frames, so construct nothing there that needs one.
	 * @param a_fn The function to invoke under the guard.
	 * @param a_context Opaque pointer forwarded to a_fn.
	 * @param a_exceptionCode Receives the SEH exception code on a fault; may be null.
	 * @return true if a_fn returned normally, false if it faulted.
	 */
	bool InvokeSehGuarded(void (*a_fn)(void*), void* a_context, DWORD* a_exceptionCode = nullptr) noexcept;

	/**
	 * @brief Lambda form of InvokeSehGuarded; the __try frame lives in SehGuard.cpp.
	 * @param a_fn The callable to invoke under the guard.
	 * @param a_exceptionCode Receives the SEH exception code on a fault; may be null.
	 * @return true if a_fn returned normally, false if it faulted.
	 */
	template <class F>
	bool SehGuarded(F&& a_fn, DWORD* a_exceptionCode = nullptr) noexcept
	{
		return InvokeSehGuarded(
			[](void* a_ctx) { (*static_cast<std::remove_reference_t<F>*>(a_ctx))(); },
			std::addressof(a_fn), a_exceptionCode);
	}
}
