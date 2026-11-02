/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace quiver::peer
{
using Hash = std::array<uint8_t, 32>;
using Token = std::array<uint8_t, 32>;
struct Endpoint
{
	std::string address;
	uint16_t	port = 0;
	Token		token{};
	uint64_t	peerId = 0;
	bool		nat = false;
};
bool			 RandomToken (Token &token);
constexpr size_t MaxObjectBytes = 1024 * 1024;
struct Statistics
{
	uint64_t uploadedBytes = 0, downloadedBytes = 0, served = 0, fetched = 0, failed = 0;
	uint64_t natConnected = 0, natServed = 0, natFetched = 0, natFailed = 0;
};
// Main-thread, nonblocking service. Provider must expose only shareable terrain
// objects. Callers must validate fetched canonical object hashes before use.
// Providers and signal senders must not re-enter this service. Completion
// callbacks may Reset; that cancels any remaining callbacks in the same pump.
class Service
{
  public:
	using Provider = std::function<bool (const Hash &, std::vector<uint8_t> &)>;
	using Completion = std::function<void (bool, std::vector<uint8_t>)>;
	using SignalSender = std::function<void (uint64_t, const std::vector<uint8_t> &)>;
	Service ();
	~Service ();
	Service (const Service &) = delete;
	Service	   &operator= (const Service &) = delete;
	bool		Listen (uint16_t port = 0);
	uint16_t	Port () const;
	void		Authorize (const Token &token, double lifetimeSeconds = 120);
	void		RevokeAll ();
	void		SetProvider (Provider provider);
	static bool NATAvailable ();
	// Identities and allowed peers come from the game server. The session nonce
	// is immutable, unlike the rotating object authorization token. Signaling
	// callbacks are drained on Pump's thread, never GNS's networking thread.
	bool		ConfigureSession (uint64_t localPeerId, const Token &sessionId, const std::string &stunServers, SignalSender sender);
	void		DisableNAT ();
	void		SetPeers (const std::vector<Endpoint> &peers);
	bool		ReceiveSignal (uint64_t sourcePeerId, const std::vector<uint8_t> &bytes);
	// Returns false without a callback if the bounded request queue is full.
	bool		Fetch (std::vector<Endpoint> endpoints, const Hash &hash, Completion completion);
	void		Pump ();
	// Cancels pending callbacks, closes sockets, and clears authorization/provider.
	void		Reset ();
	Statistics	Stats () const;

  private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
} // namespace quiver::peer
