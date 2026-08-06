#include "GpuTimestamps.h"

#include "D3D.h"

#include <algorithm>
#include <cassert>

namespace Util
{
	void TimestampQueryBatch::Configure(uint32_t a_maxPairs, const char* a_debugName)
	{
		maxPairs = a_maxPairs;
		debugName = a_debugName;
		pairs.reserve(a_maxPairs);
		closed.assign(a_maxPairs, false);
	}

	bool TimestampQueryBatch::EnsureDisjoint(ID3D11Device* a_device)
	{
		if (disjoint)
			return true;
		if (creationFailed || !a_device)
			return false;
		D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
		if (FAILED(a_device->CreateQuery(&desc, disjoint.put()))) {
			creationFailed = true;
			return false;
		}
		SetResourceName(disjoint.get(), "%s disjoint", debugName);
		return true;
	}

	bool TimestampQueryBatch::EnsurePair(ID3D11Device* a_device, uint32_t a_index)
	{
		if (a_index < pairs.size() && pairs[a_index].begin && pairs[a_index].end)
			return true;
		if (creationFailed || !a_device || a_index >= maxPairs)
			return false;
		if (pairs.size() <= a_index)
			pairs.resize(a_index + 1);
		auto& pair = pairs[a_index];
		D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP, 0 };
		if (FAILED(a_device->CreateQuery(&desc, pair.begin.put())) ||
			FAILED(a_device->CreateQuery(&desc, pair.end.put()))) {
			creationFailed = true;
			return false;
		}
		SetResourceName(pair.begin.get(), "%s[%u] begin", debugName, a_index);
		SetResourceName(pair.end.get(), "%s[%u] end", debugName, a_index);
		return true;
	}

	bool TimestampQueryBatch::Preallocate(ID3D11Device* a_device)
	{
		if (!EnsureDisjoint(a_device))
			return false;
		for (uint32_t i = 0; i < maxPairs; i++) {
			if (!EnsurePair(a_device, i))
				return false;
		}
		return true;
	}

	void TimestampQueryBatch::ReleaseQueries()
	{
		disjoint = nullptr;
		pairs.clear();
		// Keep closed sized to maxPairs (not emptied): EnsurePair lazily
		// re-grows pairs up to maxPairs without requiring Configure() again,
		// and AcquireInterval/CommitInterval/CloseInterval index closed by
		// that same bound.
		closed.assign(maxPairs, false);
		allocCount = 0;
		used = 0;
		creationFailed = false;
#ifndef NDEBUG
		usedSequentialApi = false;
		usedNestingApi = false;
#endif
	}

	void TimestampQueryBatch::Reset()
	{
		allocCount = 0;
		used = 0;
		std::fill(closed.begin(), closed.end(), false);
#ifndef NDEBUG
		usedSequentialApi = false;
		usedNestingApi = false;
#endif
	}

	bool TimestampQueryBatch::BeginBatch(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		if (!a_context || !EnsureDisjoint(a_device))
			return false;
		a_context->Begin(disjoint.get());
		return true;
	}

	int TimestampQueryBatch::BeginInterval(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
#ifndef NDEBUG
		usedSequentialApi = true;
		assert(!(usedSequentialApi && usedNestingApi) && "TimestampQueryBatch: mixed sequential/nesting API use on one instance");
#endif
		if (!a_context || used >= maxPairs || !EnsurePair(a_device, used))
			return -1;
		a_context->End(pairs[used].begin.get());
		return static_cast<int>(used);
	}

	void TimestampQueryBatch::CommitInterval(ID3D11DeviceContext* a_context)
	{
		if (!a_context || used >= pairs.size() || !pairs[used].end)
			return;
		a_context->End(pairs[used].end.get());
		closed[used] = true;
		used++;
		allocCount = used;
	}

	int TimestampQueryBatch::AcquireInterval(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
#ifndef NDEBUG
		usedNestingApi = true;
		assert(!(usedSequentialApi && usedNestingApi) && "TimestampQueryBatch: mixed sequential/nesting API use on one instance");
#endif
		if (!a_context || allocCount >= maxPairs || !EnsurePair(a_device, allocCount))
			return -1;
		const uint32_t index = allocCount;
		closed[index] = false;
		a_context->End(pairs[index].begin.get());
		allocCount++;
		return static_cast<int>(index);
	}

	void TimestampQueryBatch::CloseInterval(ID3D11DeviceContext* a_context, uint32_t a_index)
	{
		if (!a_context || a_index >= pairs.size() || !pairs[a_index].end)
			return;
		a_context->End(pairs[a_index].end.get());
		closed[a_index] = true;
	}

	void TimestampQueryBatch::EndBatch(ID3D11DeviceContext* a_context)
	{
		if (a_context && disjoint)
			a_context->End(disjoint.get());
	}

	TimestampQueryBatch::Status TimestampQueryBatch::TryResolve(ID3D11DeviceContext* a_context,
		const std::function<void(uint32_t, uint64_t, uint64_t)>& a_visit) const
	{
		if (!a_context || !disjoint)
			return Status::Disjoint;

		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT info{};
		if (a_context->GetData(disjoint.get(), &info, sizeof(info), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			return Status::NotReady;
		if (info.Disjoint || !info.Frequency)
			return Status::Disjoint;

		for (uint32_t i = 0; i < allocCount; i++) {
			if (i >= closed.size() || !closed[i])
				continue;
			uint64_t tsBegin = 0, tsEnd = 0;
			if (a_context->GetData(pairs[i].begin.get(), &tsBegin, sizeof(tsBegin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				continue;
			if (a_context->GetData(pairs[i].end.get(), &tsEnd, sizeof(tsEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				continue;
			if (tsEnd > tsBegin)
				a_visit(i, tsEnd - tsBegin, info.Frequency);
		}
		return Status::Ok;
	}
}
