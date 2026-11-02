/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "world_peer.h"

namespace quiver::peer
{
// Internal NAT backend. The TCP service remains usable in builds without GNS.
class NATTransport
{
  public:
	NATTransport ();
	~NATTransport ();
	static bool Available ();
	bool		Configure (uint64_t localId, const Token &session, const std::string &stun, Service::SignalSender sender);
	void		Disable ();
	void		SetPeers (const std::vector<Endpoint> &peers);
	void		SetProvider (Service::Provider provider);
	void		Authorize (const Token &token, double lifetime);
	void		RevokeAll ();
	bool		ReceiveSignal (uint64_t source, const std::vector<uint8_t> &bytes);
	bool		Fetch (std::vector<Endpoint> peers, const Hash &hash, Service::Completion completion);
	bool		Ready () const;
	size_t		Pending () const;
	void		Pump (size_t &budget);
	std::vector<std::pair<Service::Completion, std::vector<uint8_t>>> TakeCompletions ();
	Statistics														  Stats () const;

  private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
} // namespace quiver::peer
