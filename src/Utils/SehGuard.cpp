#include "SehGuard.h"

namespace Util
{
	__declspec(noinline) bool InvokeSehGuarded(void (*a_fn)(void*), void* a_context, DWORD* a_exceptionCode) noexcept
	{
		DWORD code = 0;
		__try {
			a_fn(a_context);
		} __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			if (a_exceptionCode)
				*a_exceptionCode = code;
			return false;
		}

		return true;
	}
}
