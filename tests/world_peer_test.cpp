/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world_peer.h"
#include <cassert>
#include <chrono>
#include <thread>
#include <iostream>
using namespace quiver::peer;
int main ()
{
	Service origin, client;
	Token	token{}, other{};
	assert (RandomToken (token) && RandomToken (other) && token != other);
	assert (origin.Listen ());
	origin.Authorize (token);
	Hash hash{};
	hash[0] = 19;
	std::vector<uint8_t> object (300000);
	for (size_t i = 0; i < object.size (); ++i)
		object[i] = uint8_t (i * 31);
	origin.SetProvider (
		[&] (const Hash &key, std::vector<uint8_t> &bytes)
		{
			if (key != hash)
				return false;
			bytes = object;
			return true;
		});
	auto pump = [&] (bool &done)
	{
		auto end = std::chrono::steady_clock::now () + std::chrono::seconds (6);
		while (!done && std::chrono::steady_clock::now () < end)
		{
			origin.Pump ();
			client.Pump ();
			std::this_thread::sleep_for (std::chrono::milliseconds (1));
		}
		assert (done);
	};
	bool done = false;
	// Rejected authorization must fail over to another endpoint, including a
	// payload larger than a socket read or per-frame transfer budget.
	assert (client.Fetch (
		{{"127.0.0.1", origin.Port (), other}, {"127.0.0.1", origin.Port (), token}}, hash,
		[&] (bool ok, std::vector<uint8_t> bytes)
		{
			assert (ok && bytes == object);
			done = true;
		}));
	pump (done);
	assert (client.Stats ().fetched == 1 && origin.Stats ().served == 1);
	done = false;
	Hash absent{};
	assert (client.Fetch (
		{{"127.0.0.1", origin.Port (), token}}, absent,
		[&] (bool ok, std::vector<uint8_t> bytes)
		{
			assert (!ok && bytes.empty ());
			done = true;
		}));
	pump (done);
	origin.RevokeAll ();
	done = false;
	assert (client.Fetch (
		{{"127.0.0.1", origin.Port (), token}}, hash,
		[&] (bool ok, std::vector<uint8_t>)
		{
			assert (!ok);
			done = true;
		}));
	pump (done);
	// Expiration must revoke an otherwise valid token even without disconnect.
	origin.Authorize (token, 1);
	std::this_thread::sleep_for (std::chrono::milliseconds (1100));
	done = false;
	assert (client.Fetch (
		{{"127.0.0.1", origin.Port (), token}}, hash,
		[&] (bool ok, std::vector<uint8_t>)
		{
			assert (!ok);
			done = true;
		}));
	pump (done);
	origin.Authorize (token);
	origin.SetProvider (
		[] (const Hash &, std::vector<uint8_t> &bytes)
		{
			bytes.resize (MaxObjectBytes + 1);
			return true;
		});
	done = false;
	assert (client.Fetch (
		{{"127.0.0.1", origin.Port (), token}}, hash,
		[&] (bool ok, std::vector<uint8_t>)
		{
			assert (!ok);
			done = true;
		}));
	pump (done);
	// Reset cancels queued callbacks; queue capacity is bounded.
	for (int i = 0; i < 8; ++i)
		assert (client.Fetch ({{"127.0.0.1", origin.Port (), token}}, hash, [] (bool, std::vector<uint8_t>) { assert (false); }));
	assert (!client.Fetch ({{"127.0.0.1", origin.Port (), token}}, hash, [] (bool, std::vector<uint8_t>) {}));
	client.Reset ();
	client.Pump ();
	std::cout << "peer transport: transfer, failover, authorization, limits, cancellation passed\n";
}
