#include "world_position_handles.h"

#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

using quiver::PositionHandles;
#define CHECK(condition)                                                                   \
	do                                                                                     \
	{                                                                                      \
		if (!(condition))                                                                  \
			throw std::runtime_error (#condition " at line " + std::to_string (__LINE__)); \
	} while (0)

static void PersistenceTests ()
{
	using quiver::PositionCheckpoint;
	PositionHandles source (8);
	const auto		first = *source.Allocate (0), second = *source.Allocate (2);
	const auto		temporary = *source.Allocate (7);
	source.Get (first)->chunk = {INT64_MIN, INT64_MAX, -1234567890123456789};
	source.Get (first)->local = {0.125f, 1023.5f, 512.0625f};
	source.Get (second)->chunk = {900000000000, -900000000000, 0};
	source.Get (second)->local = {1023.999f, 0, 1};
	const std::vector<PositionHandles::Token> references{first, first, second, {}};
	PositionCheckpoint						  checkpoint;
	checkpoint.generation = source.Generation ();
	checkpoint.schema = {{"home", false}, {"quest_target", true}};
	std::vector<uint32_t> ids;
	std::string			  error;
	CHECK (source.Export (references, checkpoint.values, ids, error));
	CHECK ((ids == std::vector<uint32_t>{1, 1, 2, 0}) && checkpoint.values.size () == 2);
	checkpoint.references = {{0, 2, ids[0]}, {0, 3, ids[1]}, {1, 0, ids[2]}};
	std::vector<uint8_t> bytes;
	CHECK (checkpoint.Encode (8, bytes, error));
	CHECK (bytes[0] == 'W' && bytes[1] == 'P' && bytes[2] == 'H' && bytes[3] == '1' && bytes[4] == 1);
	PositionCheckpoint decoded;
	CHECK (PositionCheckpoint::Decode (bytes, 8, decoded, error));
	CHECK (decoded.generation == 3 && decoded.values.size () == 2 && decoded.schema[1].global && decoded.references.size () == 3);
	CHECK (decoded.values[0].position.chunk == checkpoint.values[0].position.chunk);
	CHECK (decoded.values[0].position.local == checkpoint.values[0].position.local);
	CHECK (decoded.values[1].owner == 2);
	std::vector<PositionHandles::Token> remapped;
	CHECK (source.Import (decoded.values, decoded.generation, remapped, error));
	CHECK (!source.Get (first) && !source.Get (second) && !source.Get (temporary));
	CHECK (remapped.size () == 2 && source.ActiveCount () == 2);
	CHECK (source.Get (remapped[ids[0] - 1]) == source.Get (remapped[ids[1] - 1]));
	CHECK (source.Get (remapped[1])->local == checkpoint.values[1].position.local);
	source.ForgetOwner (2);
	CHECK (!source.Get (remapped[1]) && source.Get (remapped[0]));
	// Loading into a new process still allocates past the saved watermark.
	PositionHandles restarted (8);
	CHECK (restarted.Import (decoded.values, decoded.generation, remapped, error));
	CHECK (!restarted.Get (first) && !restarted.Get (second) && !restarted.Get (temporary));
	const auto alive = remapped[0];
	CHECK (restarted.Import (decoded.values, decoded.generation, remapped, error));
	CHECK (!restarted.Get (alive) && restarted.Generation () == 7);
	const auto stable = remapped[0];
	const auto savedTokens = remapped;
	const auto stableGeneration = restarted.Generation ();
	auto	   invalidValues = decoded.values;
	invalidValues[0].position.local[0] = std::numeric_limits<float>::quiet_NaN ();
	CHECK (!restarted.Import (invalidValues, decoded.generation, remapped, error));
	CHECK (restarted.Get (stable) && restarted.Generation () == stableGeneration && remapped == savedTokens);
	CHECK (!restarted.Import (decoded.values, PositionHandles::MaxGeneration, remapped, error));
	CHECK (restarted.Get (stable) && remapped == savedTokens);
	PositionHandles tooSmall (1);
	const auto		existing = *tooSmall.Allocate (0);
	CHECK (!tooSmall.Import (decoded.values, decoded.generation, remapped, error) && tooSmall.Get (existing));
	// Export fails transactionally for stale or forged registered references.
	auto	   oldValues = decoded.values;
	const auto oldIds = ids;
	CHECK (!source.Export (references, oldValues, ids, error));
	CHECK (oldValues.size () == 2 && ids == oldIds);
	// Every truncated payload and trailing data is rejected without replacing output.
	for (size_t length = 0; length < bytes.size (); ++length)
	{
		CHECK (!PositionCheckpoint::Decode (std::span (bytes).first (length), 8, decoded, error));
		CHECK (decoded.schema.size () == 2 && decoded.values.size () == 2);
	}
	auto malformed = bytes;
	malformed.push_back (0);
	CHECK (!PositionCheckpoint::Decode (malformed, 8, decoded, error));
	for (size_t offset : {size_t (0), size_t (4), size_t (6), size_t (16), size_t (20), size_t (24), size_t (28), size_t (29)})
	{
		malformed = bytes;
		malformed[offset] = 255;
		CHECK (!PositionCheckpoint::Decode (malformed, 8, decoded, error));
	}
	auto rejects = [&] (PositionCheckpoint bad)
	{
		const auto unchanged = bytes;
		CHECK (!bad.Encode (8, bytes, error) && bytes == unchanged);
	};
	auto bad = checkpoint;
	bad.schema.push_back (bad.schema[0]);
	rejects (bad);
	bad = checkpoint;
	bad.schema[0].name = "origin_x.bad";
	rejects (bad);
	bad = checkpoint;
	bad.schema[0].name = std::string (128, 'x');
	rejects (bad);
	bad = checkpoint;
	bad.references.push_back (bad.references[0]);
	rejects (bad);
	bad = checkpoint;
	bad.references[0].entity = 8;
	rejects (bad);
	bad = checkpoint;
	bad.references[2].entity = 1;
	rejects (bad);
	bad = checkpoint;
	bad.references[0].binding = 2;
	rejects (bad);
	bad = checkpoint;
	bad.references[0].value = 0;
	rejects (bad);
	bad = checkpoint;
	bad.references[0].value = 3;
	rejects (bad);
	bad = checkpoint;
	bad.references.pop_back ();
	rejects (bad); // Unreachable serialized value.
	bad = checkpoint;
	bad.values[0].owner = 8;
	rejects (bad);
	bad = checkpoint;
	bad.values[0].position.local[1] = 1024;
	rejects (bad);
	bad = checkpoint;
	bad.generation = 1;
	rejects (bad);
	bad = checkpoint;
	bad.generation = PositionHandles::MaxGeneration;
	rejects (bad);
	CHECK (!checkpoint.Validate (0, error));
	PositionCheckpoint empty;
	CHECK (empty.Encode (1, bytes, error) && PositionCheckpoint::Decode (bytes, 1, decoded, error));
	CHECK (decoded.schema.empty () && decoded.references.empty () && decoded.values.empty ());
	std::mt19937 random (0x51565231);
	for (int test = 0; test < 10000; ++test)
	{
		std::vector<uint8_t> noise (random () % 256);
		for (auto &byte : noise)
			byte = uint8_t (random ());
		CHECK (!PositionCheckpoint::Decode (noise, 8, decoded, error));
	}
}

static void PagingHandleTests ()
{
	PositionHandles arena (4);
	const auto		unrelated = *arena.Allocate (1), first = *arena.Allocate (5), second = *arena.Allocate (5);
	arena.Get (unrelated)->local = {12, 34, 56};
	arena.Get (first)->chunk = {INT64_MIN, INT64_MAX, 47};
	arena.Get (first)->local = {0.125f, 511.25f, 1023.5f};
	arena.Get (second)->chunk = {-500000000000, 500000000000, 0};
	const std::array							references{first, second, first};
	std::vector<PositionHandles::SavedPosition> values;
	std::vector<uint32_t>						ids;
	std::string									error;
	CHECK (arena.OwnerCount (5) == 2 && arena.OwnerCount (1) == 1);
	CHECK (arena.Export (references, values, ids, error));
	CHECK ((ids == std::vector<uint32_t>{1, 2, 1}));
	const auto watermark = arena.Generation ();
	// The engine performs this only after the durable page write succeeds.
	arena.ForgetOwner (5);
	CHECK (!arena.Get (first) && !arena.Get (second) && arena.Get (unrelated));
	CHECK (arena.OwnerCount (5) == 0);
	for (auto &value : values)
		value.owner = 9;
	std::vector<PositionHandles::Token> restored;
	CHECK (arena.Append (values, watermark, restored, error));
	CHECK (arena.ActiveCount () == 3 && arena.OwnerCount (9) == 2 && arena.OwnerCount (1) == 1);
	CHECK ((arena.Get (unrelated)->local == std::array<float, 3> ({12, 34, 56})));
	CHECK (!arena.Get (first) && !arena.Get (second));
	CHECK (arena.Get (restored[ids[0] - 1]) == arena.Get (restored[ids[2] - 1]));
	CHECK (arena.Get (restored[0])->chunk == values[0].position.chunk);
	CHECK (arena.Get (restored[0])->local == values[0].position.local);
	CHECK (arena.Owner (restored[0]) == 9 && !arena.Owner (first));
	const auto savedTokens = restored;
	const auto generation = arena.Generation ();
	CHECK (!arena.Append (values, watermark, restored, error)); // Only one slot remains.
	CHECK (restored == savedTokens && arena.Get (unrelated) && arena.Get (savedTokens[0]) && arena.Generation () == generation);
	auto invalid = std::vector{values[0]};
	invalid[0].position.local[1] = std::numeric_limits<float>::infinity ();
	CHECK (!arena.Append (invalid, watermark, restored, error));
	CHECK (restored == savedTokens && arena.Generation () == generation);
	CHECK (!arena.Append (std::span (values).first (1), PositionHandles::MaxGeneration, restored, error));
	CHECK (restored == savedTokens && arena.Generation () == generation);
	arena.ForgetOwner (9);
	CHECK (arena.ActiveCount () == 1 && arena.Get (unrelated));
	// Repeated page cycles must never revive the first page's tokens or replace
	// another owner's handles, even when all restored slots are reused.
	for (int i = 0; i < 1000; ++i)
	{
		CHECK (arena.Append (values, watermark, restored, error));
		CHECK (!arena.Get (first) && !arena.Get (savedTokens[0]) && arena.Get (unrelated));
		arena.ForgetOwner (9);
	}
}

int main ()
{
	try
	{
		PositionHandles arena (2);
		auto			first = arena.Allocate (7), second = arena.Allocate (8);
		CHECK (first && second && arena.ActiveCount () == 2);
		CHECK (!arena.Allocate (9)); // Capacity exhaustion must not invalidate existing handles.
		const auto alias = *first;
		arena.Get (*first)->chunk = {INT64_MIN, INT64_MAX, 123456789012345};
		arena.Get (*first)->local = {0.125f, 1023.5f, 0};
		CHECK (arena.Get (alias)->chunk.x == INT64_MIN);
		*arena.Get (*second) = *arena.Get (*first);
		arena.Get (*first)->local[0] = 4;
		CHECK (arena.Get (*second)->local[0] == 0.125f);
		CHECK (arena.Free (*first));
		CHECK (!arena.Get (alias) && !arena.Free (alias));
		auto reused = arena.Allocate (9);
		CHECK (reused && (*reused)[0] == (*first)[0] && *reused != *first);
		CHECK (!arena.Get (alias));
		arena.ForgetOwner (8);
		CHECK (!arena.Get (*second) && arena.Get (*reused) && arena.ActiveCount () == 1);
		arena.ForgetOwner (8); // Idempotent owner destruction cannot duplicate free slots.
		CHECK (arena.ActiveCount () == 1);
		for (float invalid : {-1.0f, 0.5f, 16777216.0f, std::numeric_limits<float>::infinity (), std::numeric_limits<float>::quiet_NaN ()})
			for (int axis = 0; axis < 3; ++axis)
			{
				auto forged = *reused;
				forged[axis] = invalid;
				CHECK (!arena.Get (forged));
			}
		CHECK (!arena.Get ({0, 0, 0}));
		arena.Reset ();
		CHECK (!arena.Get (*reused) && arena.ActiveCount () == 0);
		auto afterReset = arena.Allocate (9);
		CHECK (afterReset && *afterReset != *reused && *afterReset != *first);
		CHECK (!arena.Get (alias));
		// Exercise reuse without growing the slot arena or reviving old aliases.
		for (int i = 0; i < 100000; ++i)
		{
			const auto old = *afterReset;
			CHECK (arena.Free (old));
			afterReset = arena.Allocate (9);
			CHECK (afterReset && !arena.Get (old) && arena.ActiveCount () == 1);
		}
		PositionHandles empty (0);
		CHECK (!empty.Allocate (0));
		PersistenceTests ();
		PagingHandleTests ();
		std::cout << "world position handles: lifecycle, checkpoint/page alias/remap/restart, malformed input and budget tests passed\n";
	}
	catch (const std::exception &error)
	{
		std::cerr << error.what () << '\n';
		return 1;
	}
}
