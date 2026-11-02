/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world_string_pool.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

#define CHECK(condition)                                                                   \
	do                                                                                     \
	{                                                                                      \
		if (!(condition))                                                                  \
			throw std::runtime_error (#condition " at line " + std::to_string (__LINE__)); \
	} while (0)

int main ()
{
	try
	{
		quiver::WorldStringPool pool (16, 1024);
		int						allocations = 0;
		std::string				error;
		auto					resolve = [] (std::string_view text) -> std::optional<int>
		{
			if (text.empty ())
				return 0;
			if (text == "literal")
				return 24;
			if (text == "precache")
				return -17;
			return std::nullopt;
		};
		auto								  allocate = [&] (const std::string &) { return -100 - ++allocations; };
		const std::array<std::string_view, 6> batch{"", "literal", "precache", "generated actor name", "generated actor name", "quest description"};
		CHECK (pool.Intern (batch, resolve, allocate, error));
		CHECK (allocations == 2 && pool.Count () == 5);
		CHECK (pool.Find ("") == 0 && pool.Find ("literal") == 24 && pool.Find ("precache") == -17);
		const auto	 actor = pool.Find ("generated actor name"), quest = pool.Find ("quest description");
		const size_t bytes = pool.Bytes ();
		for (int cycle = 0; cycle < 10000; ++cycle)
			CHECK (pool.Intern (batch, resolve, allocate, error));
		CHECK (allocations == 2 && pool.Count () == 5 && pool.Bytes () == bytes);
		CHECK (pool.Find ("generated actor name") == actor && pool.Find ("quest description") == quest);
		// Exhaustion rejects the entire batch before making even one allocation.
		const std::string					  large (1024, 'x');
		const std::array<std::string_view, 2> excessive{"small new string", large};
		CHECK (!pool.Intern (excessive, resolve, allocate, error));
		CHECK (allocations == 2 && pool.Count () == 5 && pool.Bytes () == bytes && !pool.Find ("small new string"));
		const std::array<std::string_view, 1> embedded{std::string_view ("a\0b", 3)};
		CHECK (!pool.Intern (embedded, resolve, allocate, error) && allocations == 2);
		quiver::WorldStringPool				  entries (1, 100);
		const std::array<std::string_view, 2> two{"first", "second"};
		CHECK (!entries.Intern (two, resolve, allocate, error) && entries.Count () == 0 && allocations == 2);
		quiver::WorldStringPool exact (1, 6);
		CHECK (exact.Intern (std::span (two).first (1), resolve, allocate, error));
		CHECK (exact.Bytes () == 6 && exact.Count () == 1 && allocations == 3);
		// An allocation exception leaves earlier allocations cached for a retry,
		// without retaining null IDs or consuming budget for failed allocations.
		quiver::WorldStringPool failure (16, 1024);
		bool					throwOnce = true;
		auto					throwing = [&] (const std::string &text)
		{
			if (text == "second" && throwOnce)
			{
				throwOnce = false;
				throw std::runtime_error ("injected allocation failure");
			}
			return allocate (text);
		};
		try
		{
			failure.Intern (two, resolve, throwing, error);
			CHECK (false);
		}
		catch (const std::runtime_error &exception)
		{
			CHECK (std::string_view (exception.what ()) == "injected allocation failure");
		}
		CHECK (failure.Count () == 1 && failure.Find ("first") && !failure.Find ("second"));
		const auto first = failure.Find ("first");
		const int  beforeRetry = allocations;
		CHECK (failure.Intern (two, resolve, throwing, error));
		CHECK (allocations == beforeRetry + 1 && failure.Find ("first") == first && failure.Count () == 2);
		pool.Clear ();
		CHECK (!pool.Find ("literal") && pool.Count () == 0 && pool.Bytes () == 0);
		CHECK (pool.Intern (batch, resolve, allocate, error));
		CHECK (pool.Find ("generated actor name") != actor);
		quiver::WorldStringPool empty (0, 0);
		CHECK (!empty.Intern (batch, resolve, allocate, error));
		std::cout << "world string pool: 10000 reloads, stable aliases, compiled/precache reuse, budgets, failure recovery and lifecycle reset passed\n";
	}
	catch (const std::exception &exception)
	{
		std::cerr << exception.what () << '\n';
		return 1;
	}
}
