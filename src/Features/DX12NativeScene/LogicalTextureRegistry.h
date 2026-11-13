#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace DX12Native
{
	struct TextureIdentity
	{
		std::string digest;
		uint64_t width = 0, height = 0;
		uint32_t format = 0, mipLevels = 0;
		bool operator==(const TextureIdentity&) const = default;
	};

	struct TextureIdentityHash
	{
		size_t operator()(const TextureIdentity& a_key) const
		{
			size_t result = std::hash<std::string>{}(a_key.digest);
			for (const auto value : { a_key.width, a_key.height, uint64_t(a_key.format), uint64_t(a_key.mipLevels) })
				result ^= std::hash<uint64_t>{}(value) + size_t(0x9e3779b9) + (result << 6) + (result >> 2);
			return result;
		}
	};

	/** @brief Assigns logical handles while generations retain their shared tickets. */
	class LogicalTextureRegistry
	{
	public:
		struct Ticket
		{
			uint64_t handle = 0;
		};
		explicit LogicalTextureRegistry(size_t a_capacity = 8192) : capacity(a_capacity) {}

		/** @brief Reuses an active identity or assigns a never-reused handle within the bound. */
		std::shared_ptr<const Ticket> Acquire(const TextureIdentity& a_identity)
		{
			std::scoped_lock lock(mutex);
			if (auto it = entries.find(a_identity); it != entries.end()) {
				if (auto active = it->second.lock())
					return active;
				entries.erase(it);
			}
			if (entries.size() >= capacity) {
				for (auto it = entries.begin(); it != entries.end();)
					if (it->second.expired())
						it = entries.erase(it);
					else
						++it;
			}
			if (entries.size() >= capacity || nextHandle == 0)
				return {};
			auto ticket = std::make_shared<const Ticket>(Ticket{ nextHandle++ });
			entries.emplace(a_identity, ticket);
			return ticket;
		}

	private:
		std::mutex mutex;
		std::unordered_map<TextureIdentity, std::weak_ptr<const Ticket>, TextureIdentityHash> entries;
		size_t capacity;
		uint64_t nextHandle = 1;
	};
}
