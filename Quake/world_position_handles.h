#ifndef QUIVER_WORLD_POSITION_HANDLES_H
#define QUIVER_WORLD_POSITION_HANDLES_H

#include "octree_world.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <span>

namespace quiver
{
// Tokens fit classic QuakeC vectors exactly: slot, low24 generation, high24 generation.
// Assignment aliases a handle; Copy must be used to make independent position values.
class PositionHandles
{
  public:
	using Token = std::array<float, 3>;
	static constexpr uint64_t MaxGeneration = (UINT64_C (1) << 48) - 1;
	struct SavedPosition
	{
		WorldPosition position;
		uint64_t	  owner = 0;
	};
	explicit PositionHandles (size_t capacity = 65536) : capacity (capacity > 0xffffff ? 0xffffff : capacity) {}
	std::optional<Token> Allocate (uint64_t owner)
	{
		if (generation == MaxGeneration || (freeSlots.empty () && slots.size () == capacity))
			return std::nullopt;
		size_t index;
		if (freeSlots.empty ())
		{
			index = slots.size ();
			slots.emplace_back ();
		}
		else
		{
			index = freeSlots.back ();
			freeSlots.pop_back ();
		}
		auto &slot = slots[index];
		slot = {{}, owner, ++generation, true};
		return Token{float (index + 1), float (generation & 0xffffff), float (generation >> 24)};
	}
	WorldPosition *Get (const Token &token)
	{
		auto index = Index (token);
		return index ? &slots[*index].position : nullptr;
	}
	const WorldPosition *Get (const Token &token) const
	{
		auto index = Index (token);
		return index ? &slots[*index].position : nullptr;
	}
	std::optional<uint64_t> Owner (const Token &token) const
	{
		auto index = Index (token);
		return index ? std::optional (slots[*index].owner) : std::nullopt;
	}
	bool Free (const Token &token)
	{
		auto index = Index (token);
		if (!index)
			return false;
		slots[*index].active = false;
		freeSlots.push_back (*index);
		return true;
	}
	void ForgetOwner (uint64_t owner)
	{
		for (size_t index = 0; index < slots.size (); ++index)
			if (slots[index].active && slots[index].owner == owner)
			{
				slots[index].active = false;
				freeSlots.push_back (index);
			}
	}
	void Reset ()
	{
		slots.clear ();
		freeSlots.clear ();
		// Preserve the allocation generation so tokens cannot revive after map changes.
	}
	size_t ActiveCount () const
	{
		return slots.size () - freeSlots.size ();
	}
	size_t OwnerCount (uint64_t owner) const
	{
		return std::count_if (slots.begin (), slots.end (), [owner] (const Slot &slot) { return slot.active && slot.owner == owner; });
	}
	uint64_t Generation () const
	{
		return generation;
	}
	// Zero tokens are nullable references. Every other token must be live. Output
	// IDs are one-based and preserve aliases; unreferenced handles are not saved.
	bool Export (std::span<const Token> references, std::vector<SavedPosition> &values, std::vector<uint32_t> &ids, std::string &error) const
	{
		std::vector<SavedPosition> saved;
		std::vector<uint32_t>	   mapped;
		std::map<Token, uint32_t>  seen;
		mapped.reserve (references.size ());
		for (const Token &token : references)
		{
			if (token == Token{})
			{
				mapped.push_back (0);
				continue;
			}
			auto index = Index (token);
			if (!index)
			{
				error = "registered world position contains an invalid or stale handle";
				return false;
			}
			auto [it, inserted] = seen.emplace (token, uint32_t (saved.size () + 1));
			if (inserted)
				saved.push_back ({slots[*index].position, slots[*index].owner});
			mapped.push_back (it->second);
		}
		values = std::move (saved);
		ids = std::move (mapped);
		return true;
	}
	// Transactional replacement. The checkpoint's generation watermark prevents
	// old tokens from becoming valid even after a save is loaded in a new process.
	bool Import (std::span<const SavedPosition> values, uint64_t watermark, std::vector<Token> &tokens, std::string &error)
	{
		const uint64_t floor = std::max (generation, watermark);
		if (values.size () > capacity || floor > MaxGeneration || values.size () > MaxGeneration - floor)
		{
			error = "world position checkpoint exceeds handle capacity or generation range";
			return false;
		}
		PositionHandles replacement (capacity);
		replacement.generation = floor;
		std::vector<Token> mapped;
		mapped.reserve (values.size ());
		for (const auto &value : values)
		{
			for (float coordinate : value.position.local)
				if (!std::isfinite (coordinate) || coordinate < 0 || coordinate >= ChunkSize)
				{
					error = "noncanonical position in handle checkpoint";
					return false;
				}
			const auto token = *replacement.Allocate (value.owner);
			*replacement.Get (token) = value.position;
			mapped.push_back (token);
		}
		*this = std::move (replacement);
		tokens = std::move (mapped);
		return true;
	}
	// Paging restores one owner without replacing unrelated handles. Reserve all
	// storage and validate first; after generation changes, no operation can fail.
	bool Append (std::span<const SavedPosition> values, uint64_t watermark, std::vector<Token> &tokens, std::string &error)
	{
		const uint64_t floor = std::max (generation, watermark);
		if (values.size () > capacity - ActiveCount () || floor > MaxGeneration || values.size () > MaxGeneration - floor)
		{
			error = "world position page exceeds handle capacity or generation range";
			return false;
		}
		for (const auto &value : values)
			for (float coordinate : value.position.local)
				if (!std::isfinite (coordinate) || coordinate < 0 || coordinate >= ChunkSize)
				{
					error = "noncanonical position in handle page";
					return false;
				}
		std::vector<Token> mapped;
		mapped.reserve (values.size ());
		slots.reserve (slots.size () + (values.size () > freeSlots.size () ? values.size () - freeSlots.size () : 0));
		generation = floor;
		for (const auto &value : values)
		{
			const auto token = *Allocate (value.owner);
			*Get (token) = value.position;
			mapped.push_back (token);
		}
		tokens = std::move (mapped);
		return true;
	}

  private:
	struct Slot
	{
		WorldPosition position;
		uint64_t	  owner = 0, generation = 0;
		bool		  active = false;
	};
	std::optional<size_t> Index (const Token &token) const
	{
		for (float component : token)
			if (!std::isfinite (component) || component < 0 || component > 0xffffff || std::floor (component) != component)
				return std::nullopt;
		if (token[0] < 1 || token[0] > slots.size ())
			return std::nullopt;
		const size_t   index = static_cast<size_t> (token[0]) - 1;
		const uint64_t value = static_cast<uint64_t> (token[1]) | (static_cast<uint64_t> (token[2]) << 24);
		if (!slots[index].active || !value || value != slots[index].generation)
			return std::nullopt;
		return index;
	}
	size_t				capacity;
	uint64_t			generation = 0;
	std::vector<Slot>	slots;
	std::vector<size_t> freeSlots;
};

// Portable, bounded checkpoint payload. Registered locations are sparse: absent
// references are null. Schema names, not VM offsets or raw tokens, cross the save.
struct PositionCheckpoint
{
	static constexpr size_t MaxSchema = 256, MaxValues = 65536, MaxReferences = 1048576, MaxBytes = 16 * 1024 * 1024;
	struct Binding
	{
		std::string name;
		bool		global = false;
	};
	struct Reference
	{
		uint32_t binding = 0, entity = 0, value = 0;
	};
	uint64_t									generation = 0;
	std::vector<Binding>						schema;
	std::vector<PositionHandles::SavedPosition> values;
	std::vector<Reference>						references;

	static bool ValidName (const std::string &name)
	{
		if (name.empty () || name.size () > 127)
			return false;
		for (size_t i = 0; i < name.size (); ++i)
		{
			const char c = name[i];
			if (c != '_' && !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(i && c >= '0' && c <= '9'))
				return false;
		}
		return true;
	}
	bool Validate (uint32_t entityCount, std::string &error) const
	{
		auto fail = [&] (const char *message)
		{
			error = message;
			return false;
		};
		if (!entityCount || schema.size () > MaxSchema || values.size () > MaxValues || references.size () > MaxReferences || generation < values.size () ||
			generation > PositionHandles::MaxGeneration || values.size () > PositionHandles::MaxGeneration - generation)
			return fail ("invalid world position checkpoint counts or generation");
		std::set<std::pair<bool, std::string>> names;
		for (const auto &binding : schema)
			if (!ValidName (binding.name) || !names.emplace (binding.global, binding.name).second)
				return fail ("invalid or duplicate world position registration");
		for (const auto &value : values)
		{
			if (value.owner >= entityCount)
				return fail ("world position owner outside saved entities");
			for (float coordinate : value.position.local)
				if (!std::isfinite (coordinate) || coordinate < 0 || coordinate >= ChunkSize)
					return fail ("noncanonical position in world position checkpoint");
		}
		std::vector<bool>						used (values.size ());
		std::set<std::pair<uint32_t, uint32_t>> locations;
		for (const auto &reference : references)
		{
			if (reference.binding >= schema.size () || reference.entity >= entityCount || !reference.value || reference.value > values.size () ||
				(schema[reference.binding].global && reference.entity) || !locations.emplace (reference.binding, reference.entity).second)
				return fail ("invalid or duplicate world position reference");
			used[reference.value - 1] = true;
		}
		if (std::find (used.begin (), used.end (), false) != used.end ())
			return fail ("unreferenced value in world position checkpoint");
		return true;
	}
	bool Encode (uint32_t entityCount, std::vector<uint8_t> &bytes, std::string &error) const
	{
		if (!Validate (entityCount, error))
			return false;
		std::vector<uint8_t> output;
		auto				 integer = [&] (uint64_t value, size_t width)
		{
			for (size_t i = 0; i < width; ++i)
				output.push_back (uint8_t (value >> (8 * i)));
		};
		integer (0x31485057, 4); // WPH1
		integer (1, 2);
		integer (0, 2);
		integer (generation, 8);
		integer (schema.size (), 4);
		integer (values.size (), 4);
		integer (references.size (), 4);
		for (const auto &binding : schema)
		{
			integer (binding.global, 1);
			integer (binding.name.size (), 1);
			output.insert (output.end (), binding.name.begin (), binding.name.end ());
		}
		for (const auto &value : values)
		{
			integer (value.owner, 4);
			integer (std::bit_cast<uint64_t> (value.position.chunk.x), 8);
			integer (std::bit_cast<uint64_t> (value.position.chunk.y), 8);
			integer (std::bit_cast<uint64_t> (value.position.chunk.z), 8);
			for (float coordinate : value.position.local)
				integer (std::bit_cast<uint32_t> (coordinate), 4);
		}
		for (const auto &reference : references)
		{
			integer (reference.binding, 4);
			integer (reference.entity, 4);
			integer (reference.value, 4);
		}
		if (output.size () > MaxBytes)
		{
			error = "world position checkpoint exceeds byte budget";
			return false;
		}
		bytes = std::move (output);
		return true;
	}
	static bool Decode (std::span<const uint8_t> bytes, uint32_t entityCount, PositionCheckpoint &result, std::string &error)
	{
		auto fail = [&] (const char *message)
		{
			error = message;
			return false;
		};
		if (bytes.size () < 28 || bytes.size () > MaxBytes)
			return fail ("invalid world position checkpoint byte count");
		size_t cursor = 0;
		bool   valid = true;
		auto   integer = [&] (size_t width) -> uint64_t
		{
			if (cursor > bytes.size () || width > bytes.size () - cursor)
			{
				valid = false;
				return 0;
			}
			uint64_t value = 0;
			for (size_t i = 0; i < width; ++i)
				value |= uint64_t (bytes[cursor++]) << (8 * i);
			return value;
		};
		if (integer (4) != 0x31485057 || integer (2) != 1 || integer (2) != 0)
			return fail ("unsupported world position checkpoint format");
		PositionCheckpoint decoded;
		decoded.generation = integer (8);
		const size_t schemas = integer (4), values = integer (4), references = integer (4);
		if (schemas > MaxSchema || values > MaxValues || references > MaxReferences || schemas * 3 + values * 40 + references * 12 > bytes.size () - cursor)
			return fail ("invalid world position checkpoint record counts");
		for (size_t i = 0; i < schemas; ++i)
		{
			const auto kind = integer (1), length = integer (1);
			if (!valid || kind > 1 || !length || length > 127 || length > bytes.size () - cursor)
				return fail ("invalid checkpoint registration name");
			decoded.schema.push_back ({std::string (reinterpret_cast<const char *> (bytes.data () + cursor), size_t (length)), kind != 0});
			cursor += length;
		}
		decoded.values.resize (values);
		for (auto &value : decoded.values)
		{
			value.owner = integer (4);
			value.position.chunk = {std::bit_cast<int64_t> (integer (8)), std::bit_cast<int64_t> (integer (8)), std::bit_cast<int64_t> (integer (8))};
			for (float &coordinate : value.position.local)
				coordinate = std::bit_cast<float> (uint32_t (integer (4)));
		}
		decoded.references.resize (references);
		for (auto &reference : decoded.references)
			reference = {uint32_t (integer (4)), uint32_t (integer (4)), uint32_t (integer (4))};
		if (!valid || cursor != bytes.size ())
			return fail ("truncated or trailing world position checkpoint bytes");
		if (!decoded.Validate (entityCount, error))
			return false;
		result = std::move (decoded);
		return true;
	}
};
} // namespace quiver
#endif
