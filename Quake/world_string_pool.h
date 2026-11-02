/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_WORLD_STRING_POOL_H
#define QUIVER_WORLD_STRING_POOL_H

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace quiver
{
// Metadata for strings owned by a VM lifetime. Eviction cannot reclaim an
// individual string because QuakeC aliases may escape its original entity.
class WorldStringPool
{
  public:
	explicit WorldStringPool (size_t maxEntries = 16384, size_t maxBytes = 4 * 1024 * 1024) : maxEntries (maxEntries), maxBytes (maxBytes) {}
	// Resolve returns a stable existing VM ID, or nullopt. Allocate creates a
	// VM-owned string. The complete batch is checked before either table insertion
	// or allocation, and repeat content never calls Allocate again.
	template <typename Resolve, typename Allocate> bool Intern (std::span<const std::string_view> texts, Resolve resolve, Allocate allocate, std::string &error)
	{
		std::map<std::string, std::optional<int>, std::less<>> pending;
		size_t												   newBytes = 0;
		for (std::string_view text : texts)
		{
			if (entries.contains (text) || pending.contains (text))
				continue;
			if (text.find ('\0') != std::string_view::npos || pending.size () >= maxEntries - entries.size () || text.size () >= maxBytes - bytes - newBytes)
			{
				error = "restored QC string pool exceeds its entry/byte budget";
				return false;
			}
			newBytes += text.size () + 1;
			pending.emplace (text, resolve (text));
		}
		entries.merge (pending); // Same map allocator: transfers preallocated nodes.
		bytes += newBytes;
		try
		{
			for (auto &[text, id] : entries)
				if (!id)
					id = allocate (text);
		}
		catch (...)
		{
			// Retain successfully allocated IDs so a later retry can reuse them.
			// Null nodes have no VM allocation and can be discarded safely.
			for (auto it = entries.begin (); it != entries.end ();)
				if (!it->second)
				{
					bytes -= it->first.size () + 1;
					it = entries.erase (it);
				}
				else
					++it;
			throw;
		}
		return true;
	}
	std::optional<int> Find (std::string_view text) const
	{
		auto found = entries.find (text);
		return found == entries.end () ? std::nullopt : found->second;
	}
	void Clear ()
	{
		entries.clear ();
		bytes = 0;
	}
	size_t Count () const
	{
		return entries.size ();
	}
	size_t Bytes () const
	{
		return bytes;
	}

  private:
	size_t												   maxEntries, maxBytes, bytes = 0;
	std::map<std::string, std::optional<int>, std::less<>> entries;
};
} // namespace quiver
#endif
