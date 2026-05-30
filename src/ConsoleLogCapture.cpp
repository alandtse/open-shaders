#include "ConsoleLogCapture.h"

#include "Globals.h"
#include "State.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace
{
	std::mutex g_mutex;
	std::deque<ConsoleLogCapture::Line> g_lines;
	std::atomic<uint64_t> g_seq{ 0 };
	std::atomic<bool> g_capturing{ false };  // gate: detour is a no-op unless true
	constexpr size_t kMaxLines = 1024;       // bounded; oldest dropped first

	void Append(const char* a_text, size_t a_len)
	{
		while (a_len > 0 && (a_text[a_len - 1] == '\n' || a_text[a_len - 1] == '\r'))
			--a_len;
		if (a_len == 0)
			return;
		const uint64_t s = g_seq.fetch_add(1, std::memory_order_relaxed);
		const uint32_t frame = globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
		std::lock_guard<std::mutex> lock(g_mutex);
		g_lines.push_back({ s, frame, std::string(a_text, a_len) });
		while (g_lines.size() > kMaxLines)
			g_lines.pop_front();
	}

	// Detour on RE::ConsoleLog::VPrint. When no capture window is open this is a
	// single relaxed atomic load then a tail-forward — cheap enough to leave on a
	// function the engine floods during cell load.
	struct VPrintHook
	{
		static void thunk(RE::ConsoleLog* a_self, const char* a_fmt, std::va_list a_args)
		{
			if (g_capturing.load(std::memory_order_relaxed) && a_fmt) {
				std::va_list copy;
				va_copy(copy, a_args);  // vsnprintf and the original each consume the list
				char buf[1024];
				const int n = std::vsnprintf(buf, sizeof(buf), a_fmt, copy);
				va_end(copy);
				if (n > 0) {
					const size_t len = (n < static_cast<int>(sizeof(buf))) ? static_cast<size_t>(n) : sizeof(buf) - 1;
					Append(buf, len);
				}
			}
			func(a_self, a_fmt, a_args);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

namespace ConsoleLogCapture
{
	void Install()
	{
		static std::once_flag once;
		std::call_once(once, []() {
			// Same address CommonLibSSE-NG resolves for ConsoleLog::VPrint; the
			// RelocationID covers SE/AE and maps to VR via the address library.
			stl::detour_thunk<VPrintHook>(REL::RelocationID(50180, 51110));
			logger::info("ConsoleLogCapture: hooked ConsoleLog::VPrint (gated)");
		});
	}

	void BeginCapture()
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_lines.clear();
		}
		g_capturing.store(true, std::memory_order_relaxed);
	}

	void EndCapture()
	{
		g_capturing.store(false, std::memory_order_relaxed);
	}

	bool IsCapturing()
	{
		return g_capturing.load(std::memory_order_relaxed);
	}

	uint64_t HeadSeq()
	{
		return g_seq.load(std::memory_order_relaxed);
	}

	std::vector<Line> Snapshot(size_t a_maxLines)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const size_t total = g_lines.size();
		const size_t start = total > a_maxLines ? total - a_maxLines : 0;
		std::vector<Line> out;
		out.reserve(total - start);
		for (size_t i = start; i < total; ++i)
			out.push_back(g_lines[i]);
		return out;
	}
}
