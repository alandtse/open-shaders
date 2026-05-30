#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Captures Skyrim console output by detouring RE::ConsoleLog::VPrint — the single
// sink every console command's output funnels through (verified across SE/AE/VR:
// its caller Print has 600+ command-handler callers — getav, getpos, etc.).
// Command output is otherwise unreadable, since RE::Console::ExecuteCommand is void.
//
// GATED: ConsoleLog is flooded during cell load (100k+ calls with heavy modlists),
// so capture is OFF by default and the detour is a near-no-op (one relaxed atomic
// load) unless a capture window is open. Open a short window around one command —
// BeginCapture(), run it, read, EndCapture() — to grab just that command's output
// without the firehose, the per-call overhead, or the spam evicting it.
namespace ConsoleLogCapture
{
	struct Line
	{
		uint64_t seq = 0;    ///< monotonic, assigned in capture order
		uint32_t frame = 0;  ///< render frame at capture time (0 if unavailable)
		std::string text;    ///< one console line, trailing newline stripped
	};

	/// Install the VPrint detour. Idempotent and thread-safe; call from the
	/// hook-install path (trampoline must still have space).
	void Install();

	/// Clear the buffer and start capturing. The detour records lines only while
	/// a window is open.
	void BeginCapture();

	/// Stop capturing; the detour returns to a near-no-op.
	void EndCapture();

	bool IsCapturing();

	/// Sequence number the next captured line will receive.
	uint64_t HeadSeq();

	/// Up to maxLines most-recent captured lines, oldest-first.
	std::vector<Line> Snapshot(size_t maxLines);
}
