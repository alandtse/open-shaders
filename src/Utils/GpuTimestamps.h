#pragma once

#include <d3d11.h>
#include <functional>
#include <vector>
#include <winrt/base.h>

namespace Util
{
	/**
	 * @brief One D3D11 timestamp-disjoint bracket with paired begin/end
	 * timestamp queries.
	 *
	 * Owns the query objects and the readback protocol (DONOTFLUSH polling,
	 * raw ticks + frequency out, so consumers keep their own precision and
	 * units). Consumers own the batching policy: frame rings, latency,
	 * and any payloads keyed by the returned interval index.
	 *
	 * Two interval APIs share one allocation cursor and completion set, but
	 * a single bracket (the span between BeginBatch/EndBatch) must use only
	 * one of them (debug-asserted; the check resets with Reset()):
	 *  - Sequential (BeginInterval/CommitInterval): for non-nesting
	 *    consumers. BeginInterval stamps the CURRENT slot without advancing
	 *    and returns its index; CommitInterval stamps the end, marks it
	 *    resolvable, and advances. An interval begun but never committed is
	 *    silently overwritten by the next BeginInterval.
	 *  - Nesting (AcquireInterval/CloseInterval): AcquireInterval stamps a
	 *    begin at a freshly allocated index and advances immediately, so
	 *    concurrently open (nested) intervals never collide on one slot.
	 *    CloseInterval stamps that index's end and marks it resolvable. An
	 *    interval acquired but never closed is simply never resolved.
	 */
	class TimestampQueryBatch
	{
	public:
		/// Sets capacity and the debug-name prefix for the D3D objects.
		/// No device work; call once before first use.
		void Configure(uint32_t a_maxPairs, const char* a_debugName);

		/// Eagerly creates the disjoint and every pair, for consumers that
		/// cannot afford lazy creation mid-pass. Lazy consumers can skip
		/// this; BeginInterval/AcquireInterval create pairs on demand.
		bool Preallocate(ID3D11Device* a_device);

		void ReleaseQueries();

		/// Opens the disjoint bracket. false when query creation failed.
		bool BeginBatch(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

		/// Stamps the current interval's begin; returns its index, or -1 at
		/// capacity / on creation failure. Does not advance.
		int BeginInterval(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

		/// Stamps the current interval's end, marks it resolvable, and
		/// advances to the next slot.
		void CommitInterval(ID3D11DeviceContext* a_context);

		/// Stamps a freshly allocated interval's begin and advances
		/// immediately; returns its index, or -1 at capacity / on creation
		/// failure. Safe to call again before the returned index is closed.
		int AcquireInterval(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

		/// Stamps the given interval's end and marks it resolvable.
		void CloseInterval(ID3D11DeviceContext* a_context, uint32_t a_index);

		/// Closes the disjoint bracket.
		void EndBatch(ID3D11DeviceContext* a_context);

		enum class Status
		{
			NotReady,  ///< disjoint result not available yet; poll again later
			Disjoint,  ///< bracket spanned a clock event; discard the samples
			Ok
		};

		/// Polls with DONOTFLUSH (never stalls). On Ok, calls
		/// a_visit(intervalIndex, deltaTicks, frequency) for every resolvable
		/// (closed/committed) interval whose two timestamps resolved with
		/// end > begin; anything else is skipped, not reported.
		Status TryResolve(ID3D11DeviceContext* a_context,
			const std::function<void(uint32_t, uint64_t, uint64_t)>& a_visit) const;

		/// Committed interval count for the current bracket. Sequential-API
		/// consumers only: equals the allocation cursor for that API.
		uint32_t Used() const { return used; }

		bool CreationFailed() const { return creationFailed; }

		/// Forgets all intervals (committed or not) so the batch can record
		/// a new bracket.
		void Reset();

	private:
		struct Pair
		{
			winrt::com_ptr<ID3D11Query> begin;
			winrt::com_ptr<ID3D11Query> end;
		};

		bool EnsureDisjoint(ID3D11Device* a_device);
		bool EnsurePair(ID3D11Device* a_device, uint32_t a_index);

		winrt::com_ptr<ID3D11Query> disjoint;
		std::vector<Pair> pairs;
		std::vector<bool> closed;
		uint32_t maxPairs = 0;
		uint32_t allocCount = 0;  ///< high-water mark of stamped-begin slots this bracket
		uint32_t used = 0;        ///< sequential-API committed count; == allocCount for that API
		bool creationFailed = false;
#ifndef NDEBUG
		bool usedSequentialApi = false;
		bool usedNestingApi = false;
#endif
		const char* debugName = "TimestampQueryBatch";
	};
}
