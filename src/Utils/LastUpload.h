#pragma once

#include <cstring>
#include <optional>
#include <type_traits>

namespace Util
{
	/** @brief Remembers the bytes last written to a GPU buffer so an upload of identical data can be skipped. */
	template <class T>
	class LastUpload
	{
		static_assert(std::is_trivially_copyable_v<T>, "LastUpload compares raw bytes");

	public:
		/** @brief True when a_data must be uploaded: nothing was recorded since construction or Reset, or its bytes differ. */
		[[nodiscard]] bool Differs(const T& a_data) const { return !last || std::memcmp(&*last, &a_data, sizeof(T)) != 0; }

		/** @brief Records a_data as the buffer's contents; call only after the upload succeeded. */
		void Record(const T& a_data) { last = a_data; }

		/** @brief Forgets the recorded contents, e.g. when the buffer is recreated, so the next upload always happens. */
		void Reset() { last.reset(); }

	private:
		std::optional<T> last;
	};
}
