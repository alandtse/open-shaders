#pragma once

#include <exception>
#include <new>

namespace DX12Native
{
	/** @brief Continues after source-specific failures while preserving fatal failures. */
	template <class Entries, class Process, class OnError>
	void ProcessTextureSources(const Entries& a_entries, Process&& a_process, OnError&& a_onError)
	{
		for (const auto& entry : a_entries) {
			try { a_process(entry); }
			catch (const std::bad_alloc&) { throw; }
			catch (const std::exception& error) { a_onError(error); }
		}
	}
}
