/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world_peer_gns.h"
#include "world_sha256.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <utility>
#ifdef QUIVER_HAVE_GNS
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingcustomsignaling.h>
#endif

namespace quiver::peer
{
#ifdef QUIVER_HAVE_GNS
namespace
{
double Now ()
{
	return std::chrono::duration<double> (std::chrono::steady_clock::now ().time_since_epoch ()).count ();
}
constexpr size_t BlockBytes = 16 * 1024, MaxSignal = 8192;
constexpr int	 VirtualPort = 7;
// This engine has one remote world per process. Avoid changing GNS's process
// identity underneath a second service (including in standalone test programs).
void			*activeTransport = nullptr;
struct SignalQueue
{
	std::mutex											  mutex;
	bool												  alive = true;
	std::deque<std::pair<uint64_t, std::vector<uint8_t>>> messages;
	bool												  Push (uint64_t peer, const void *bytes, int size)
	{
		std::lock_guard lock (mutex);
		if (!alive || size <= 0 || size > int (MaxSignal))
			return false;
		// Best-effort signaling may drop a burst; GNS retransmits rendezvous data.
		if (messages.size () < 32)
		{
			const auto *p = static_cast<const uint8_t *> (bytes);
			messages.emplace_back (peer, std::vector<uint8_t> (p, p + size));
		}
		return true;
	}
};
struct Signaling final : ISteamNetworkingConnectionSignaling
{
	std::shared_ptr<SignalQueue> queue;
	uint64_t					 peer;
	Signaling (std::shared_ptr<SignalQueue> q, uint64_t id) : queue (std::move (q)), peer (id) {}
	bool SendSignal (HSteamNetConnection, const SteamNetConnectionInfo_t &, const void *bytes, int size) override
	{
		return queue->Push (peer, bytes, size);
	}
	void Release () override
	{
		delete this;
	}
};
// GNS's custom-signaling callback checks new connections, but existing-connection
// signals bypass it. Bind every protobuf envelope to the server-authenticated
// sender before GNS can dispatch it. Field numbers are from the pinned upstream
// CMsgSteamNetworkingP2PRendezvous (from_identity=8, to_identity=10).
bool SignalIdentity (const std::vector<uint8_t> &bytes, const SteamNetworkingIdentity &source, const SteamNetworkingIdentity &local)
{
	size_t cursor = 0;
	bool   foundSource = false, foundTarget = false;
	auto   integer = [&] (uint64_t &value)
	{
		value = 0;
		for (int shift = 0; shift < 70 && cursor < bytes.size (); shift += 7)
		{
			uint8_t b = bytes[cursor++];
			if (shift == 63 && b > 1)
				return false;
			value |= uint64_t (b & 127) << shift;
			if (!(b & 128))
				return true;
		}
		return false;
	};
	while (cursor < bytes.size ())
	{
		uint64_t tag, length = 0;
		if (!integer (tag) || !(tag >> 3) || (tag >> 3) > 0x1fffffff)
			return false;
		const uint64_t field = tag >> 3, wire = tag & 7;
		if ((field == 8 || field == 10) && wire != 2)
			return false;
		if (wire == 0)
		{
			if (!integer (length))
				return false;
			continue;
		}
		if (wire == 1)
			length = 8;
		else if (wire == 5)
			length = 4;
		else if (wire != 2 || !integer (length))
			return false;
		if (length > bytes.size () - cursor)
			return false;
		if (field == 8 || field == 10)
		{
			bool &found = field == 8 ? foundSource : foundTarget;
			if (found || !length || length >= SteamNetworkingIdentity::k_cchMaxString)
				return false;
			std::string				text (bytes.begin () + cursor, bytes.begin () + cursor + size_t (length));
			SteamNetworkingIdentity identity;
			identity.Clear ();
			if (text.find ('\0') != std::string::npos || !identity.ParseString (text.c_str ()) || identity != (field == 8 ? source : local))
				return false;
			found = true;
		}
		cursor += size_t (length);
	}
	return foundSource;
}

uint32_t Read32 (const uint8_t *p)
{
	return uint32_t (p[0]) | uint32_t (p[1]) << 8 | uint32_t (p[2]) << 16 | uint32_t (p[3]) << 24;
}
void Write32 (std::vector<uint8_t> &bytes, uint32_t value)
{
	for (int i = 0; i < 4; ++i)
		bytes.push_back (uint8_t (value >> (i * 8)));
}
} // namespace
struct NATTransport::Impl
{
	struct Connection
	{
		uint64_t			 peer = 0, job = 0;
		bool				 outbound = false, connected = false, requestSent = false;
		double				 deadline = 0;
		std::vector<uint8_t> response;
		size_t				 offset = 0;
	};
	struct Job
	{
		uint64_t			  id = 0;
		std::vector<Endpoint> peers;
		size_t				  next = 0;
		Hash				  hash{};
		Service::Completion	  completion;
		double				  deadline = 0, attemptDeadline = 0;
		HSteamNetConnection	  connection = k_HSteamNetConnection_Invalid;
		uint32_t			  expected = 0;
		std::vector<uint8_t>  bytes;
	};
	bool															  initialized = false;
	uint64_t														  localId = 0, nextJob = 1;
	Token															  session{};
	std::string														  stun;
	Service::SignalSender											  sender;
	Service::Provider												  provider;
	std::shared_ptr<SignalQueue>									  signals = std::make_shared<SignalQueue> ();
	std::map<uint64_t, Endpoint>									  peers;
	std::map<HSteamNetConnection, Connection>						  connections;
	std::map<uint64_t, Job>											  jobs;
	std::vector<std::pair<Token, double>>							  tokens;
	Statistics														  stats;
	HSteamListenSocket												  listener = k_HSteamListenSocket_Invalid;
	HSteamNetPollGroup												  poll = k_HSteamNetPollGroup_Invalid;
	std::vector<std::pair<Service::Completion, std::vector<uint8_t>>> completed;

	SteamNetworkingIdentity Identity (uint64_t id) const
	{
		// GNS generic strings hold only 31 characters. Hash the complete session
		// nonce and peer ID into its 32-byte identity form instead of truncating.
		std::vector<uint8_t> canonical{'Q', 'N', 'I', 1};
		canonical.insert (canonical.end (), session.begin (), session.end ());
		for (int i = 0; i < 8; ++i)
			canonical.push_back (uint8_t (id >> (8 * i)));
		const auto				hash = WorldSHA256 (canonical);
		SteamNetworkingIdentity identity;
		identity.Clear ();
		identity.SetGenericBytes (hash.data (), hash.size ());
		return identity;
	}
	~Impl ()
	{
		{
			std::lock_guard lock (signals->mutex);
			signals->alive = false;
			signals->messages.clear ();
		}
		if (!initialized)
			return;
		for (const auto &[handle, connection] : connections)
			SteamNetworkingSockets ()->CloseConnection (handle, 0, "world session ended", false);
		if (listener)
			SteamNetworkingSockets ()->CloseListenSocket (listener);
		if (poll)
			SteamNetworkingSockets ()->DestroyPollGroup (poll);
		GameNetworkingSockets_Kill ();
		activeTransport = nullptr;
	}
	void Close (HSteamNetConnection handle)
	{
		SteamNetworkingSockets ()->CloseConnection (handle, 0, "world object transfer finished", false);
		connections.erase (handle);
	}
	void Finish (uint64_t id, bool success)
	{
		auto it = jobs.find (id);
		if (it == jobs.end ())
			return;
		auto &job = it->second;
		if (auto c = connections.find (job.connection); c != connections.end ())
		{
			c->second.job = 0;
			c->second.requestSent = false;
			c->second.deadline = Now () + 30;
			if (!success)
				Close (job.connection);
		}
		if (success)
		{
			++stats.fetched;
			++stats.natFetched;
		}
		else
		{
			++stats.failed;
			++stats.natFailed;
			job.bytes.clear ();
		}
		completed.emplace_back (std::move (job.completion), std::move (job.bytes));
		jobs.erase (it);
	}
	bool Start (Job &job)
	{
		if (job.connection)
			Close (job.connection);
		job.connection = k_HSteamNetConnection_Invalid;
		job.bytes.clear ();
		job.expected = 0;
		while (job.next < job.peers.size ())
		{
			const Endpoint &endpoint = job.peers[job.next++];
			if (!endpoint.nat || !endpoint.peerId || !peers.contains (endpoint.peerId))
				continue;
			for (auto &[handle, c] : connections)
				if (c.peer == endpoint.peerId && c.outbound && !c.job && c.connected)
				{
					c.job = job.id;
					c.deadline = job.deadline;
					c.requestSent = false;
					job.connection = handle;
					job.attemptDeadline = std::min (job.deadline, Now () + 5);
					return true;
				}
			if (connections.size () >= 16)
				return false;
			auto identity = Identity (endpoint.peerId);
			auto handle = SteamNetworkingSockets ()->ConnectP2PCustomSignaling (new Signaling (signals, endpoint.peerId), &identity, VirtualPort, 0, nullptr);
			if (!handle)
				continue; // GNS releases the signaling object on failure.
			Connection c;
			c.peer = endpoint.peerId;
			c.job = job.id;
			c.deadline = job.deadline;
			c.outbound = true;
			c.deadline = job.deadline;
			connections.emplace (handle, std::move (c));
			SteamNetworkingSockets ()->SetConnectionPollGroup (handle, poll);
			job.connection = handle;
			job.attemptDeadline = std::min (job.deadline, Now () + 5);
			return true;
		}
		return false;
	}
	void Message (SteamNetworkingMessage_t *message)
	{
		auto it = connections.find (message->m_conn);
		if (it == connections.end ())
			return;
		auto		&c = it->second;
		const auto	*bytes = static_cast<const uint8_t *> (message->m_pData);
		const size_t size = size_t (message->m_cbSize);
		stats.downloadedBytes += size;
		if (!c.outbound)
		{
			Token token;
			Hash  hash;
			if (size != 68 || std::memcmp (bytes, "QOB\1", 4) || !c.response.empty ())
			{
				Close (message->m_conn);
				return;
			}
			std::copy_n (bytes + 4, 32, token.begin ());
			std::copy_n (bytes + 36, 32, hash.begin ());
			const bool authorized = std::any_of (tokens.begin (), tokens.end (), [&] (const auto &t) { return t.first == token && t.second > Now (); });
			if (!authorized || !provider || !provider (hash, c.response) || c.response.empty () || c.response.size () > MaxObjectBytes)
			{
				Close (message->m_conn);
				return;
			}
			c.offset = 0;
			c.deadline = Now () + 8;
			return;
		}
		auto j = jobs.find (c.job);
		if (j == jobs.end ())
		{
			Close (message->m_conn);
			return;
		}
		auto &job = j->second;
		if (size <= 12 || size > BlockBytes + 12 || std::memcmp (bytes, "QND\1", 4))
		{
			Finish (job.id, false);
			return;
		}
		uint32_t total = Read32 (bytes + 4), offset = Read32 (bytes + 8);
		if (!total || total > MaxObjectBytes || offset != job.bytes.size () || (job.expected && job.expected != total) || size - 12 > total - offset)
		{
			Finish (job.id, false);
			return;
		}
		job.expected = total;
		job.bytes.insert (job.bytes.end (), bytes + 12, bytes + size);
		job.attemptDeadline = std::min (job.deadline, Now () + 5);
		if (job.bytes.size () == total)
			Finish (job.id, true);
	}
};
#else
struct NATTransport::Impl
{
};
#endif
NATTransport::NATTransport () : impl (std::make_unique<Impl> ()) {}
NATTransport::~NATTransport () = default;
void NATTransport::Disable ()
{
	impl = std::make_unique<Impl> ();
}
bool NATTransport::Available ()
{
#ifdef QUIVER_HAVE_GNS
	return true;
#else
	return false;
#endif
}
bool NATTransport::Configure (uint64_t localId, const Token &session, const std::string &stun, Service::SignalSender sender)
{
#ifdef QUIVER_HAVE_GNS
	if (impl->initialized)
	{
		if (impl->localId == localId && impl->session == session && impl->stun == stun)
			return true;
		impl = std::make_unique<Impl> ();
	}
	if (activeTransport || !localId || session == Token{} || stun.size () > 255 || !sender)
		return false;
	impl->localId = localId;
	impl->session = session;
	impl->stun = stun;
	impl->sender = std::move (sender);
	auto				  identity = impl->Identity (localId);
	SteamNetworkingErrMsg error;
	if (!GameNetworkingSockets_Init (&identity, error))
		return false;
	impl->initialized = true;
	activeTransport = impl.get ();
	auto *utils = SteamNetworkingUtils ();
	// Only native host/reflexive ICE candidates: the game server already provides
	// reliable object fallback, and no TURN credentials or Steam SDR are required.
	bool  configured = true;
	configured &= utils->SetGlobalConfigValueInt32 (k_ESteamNetworkingConfig_P2P_Transport_ICE_Implementation, 1);
	configured &= utils->SetGlobalConfigValueInt32 (
		k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable,
		k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Private | k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Public);
	configured &= utils->SetGlobalConfigValueString (k_ESteamNetworkingConfig_P2P_STUN_ServerList, stun.c_str ());
	configured &= utils->SetGlobalConfigValueInt32 (k_ESteamNetworkingConfig_TimeoutInitial, 7000);
	configured &= utils->SetGlobalConfigValueInt32 (k_ESteamNetworkingConfig_TimeoutConnected, 5000);
	configured &= utils->SetGlobalConfigValueInt32 (k_ESteamNetworkingConfig_SendBufferSize, 128 * 1024);
	configured &= utils->SetGlobalConfigValueInt32 (k_ESteamNetworkingConfig_RecvBufferSize, 128 * 1024);
	configured &= utils->SetGlobalConfigValueInt32 (k_ESteamNetworkingConfig_RecvBufferMessages, 64);
	configured &= utils->SetGlobalConfigValueInt32 (k_ESteamNetworkingConfig_RecvMaxMessageSize, int (BlockBytes + 12));
	configured &= utils->SetGlobalConfigValueInt32 (k_ESteamNetworkingConfig_SendRateMax, 256 * 1024);
	if (!configured)
	{
		impl = std::make_unique<Impl> ();
		return false;
	}
	impl->poll = SteamNetworkingSockets ()->CreatePollGroup ();
	impl->listener = SteamNetworkingSockets ()->CreateListenSocketP2P (VirtualPort, 0, nullptr);
	if (!impl->poll || !impl->listener)
	{
		impl = std::make_unique<Impl> ();
		return false;
	}
	return true;
#else
	(void)localId;
	(void)session;
	(void)stun;
	(void)sender;
	return false;
#endif
}
void NATTransport::SetPeers (const std::vector<Endpoint> &peers)
{
#ifdef QUIVER_HAVE_GNS
	impl->peers.clear ();
	for (const auto &peer : peers)
		if (peer.nat && peer.peerId && peer.peerId != impl->localId && impl->peers.size () < 128)
			impl->peers[peer.peerId] = peer;
	if (!impl->initialized)
		return;
	for (auto it = impl->connections.begin (); it != impl->connections.end ();)
	{
		auto handle = it->first;
		bool remove = !impl->peers.contains (it->second.peer);
		++it;
		if (remove)
			impl->Close (handle);
	}
#else
	(void)peers;
#endif
}
void NATTransport::SetProvider (Service::Provider provider)
{
#ifdef QUIVER_HAVE_GNS
	impl->provider = std::move (provider);
#else
	(void)provider;
#endif
}
void NATTransport::Authorize (const Token &token, double lifetime)
{
#ifdef QUIVER_HAVE_GNS
	const double expiry = Now () + std::clamp (lifetime, 1.0, 300.0);
	for (auto &t : impl->tokens)
		if (t.first == token)
		{
			t.second = expiry;
			return;
		}
	if (impl->tokens.size () < 128)
		impl->tokens.emplace_back (token, expiry);
#else
	(void)token;
	(void)lifetime;
#endif
}
void NATTransport::RevokeAll ()
{
#ifdef QUIVER_HAVE_GNS
	impl->tokens.clear ();
	if (!impl->initialized)
		return;
	for (auto it = impl->connections.begin (); it != impl->connections.end ();)
	{
		auto handle = it->first;
		bool incoming = !it->second.outbound;
		++it;
		if (incoming)
			impl->Close (handle);
	}
#endif
}
bool NATTransport::ReceiveSignal (uint64_t source, const std::vector<uint8_t> &bytes)
{
#ifdef QUIVER_HAVE_GNS
	if (!impl->initialized || bytes.empty () || bytes.size () > MaxSignal || !impl->peers.contains (source))
		return false;
	if (!SignalIdentity (bytes, impl->Identity (source), impl->Identity (impl->localId)))
		return false;
	struct Context final : ISteamNetworkingSignalingRecvContext
	{
		Impl	&state;
		uint64_t source;
		Context (Impl &s, uint64_t id) : state (s), source (id) {}
		ISteamNetworkingConnectionSignaling *OnConnectRequest (HSteamNetConnection handle, const SteamNetworkingIdentity &identity, int port) override
		{
			if (port != VirtualPort || identity != state.Identity (source) || state.connections.size () >= 16)
				return nullptr;
			size_t incoming = 0;
			for (const auto &[h, c] : state.connections)
				incoming += !c.outbound;
			if (incoming >= 8)
				return nullptr;
			if (SteamNetworkingSockets ()->AcceptConnection (handle) != k_EResultOK)
				return nullptr;
			Impl::Connection c;
			c.peer = source;
			c.deadline = Now () + 8;
			state.connections.emplace (handle, std::move (c));
			SteamNetworkingSockets ()->SetConnectionPollGroup (handle, state.poll);
			return new Signaling (state.signals, source);
		}
		void SendRejectionSignal (const SteamNetworkingIdentity &, const void *, int) override {}
	} context (*impl, source);
	return SteamNetworkingSockets ()->ReceivedP2PCustomSignal (bytes.data (), int (bytes.size ()), &context);
#else
	(void)source;
	(void)bytes;
	return false;
#endif
}
bool NATTransport::Fetch (std::vector<Endpoint> peers, const Hash &hash, Service::Completion completion)
{
#ifdef QUIVER_HAVE_GNS
	if (!impl->initialized || impl->jobs.size () >= 8 || !completion)
		return false;
	std::erase_if (peers, [&] (const Endpoint &p) { return !p.nat || !impl->peers.contains (p.peerId); });
	if (peers.empty ())
		return false;
	if (peers.size () > 8)
		peers.resize (8);
	Impl::Job job;
	job.id = impl->nextJob++;
	job.peers = std::move (peers);
	job.hash = hash;
	job.completion = std::move (completion);
	job.deadline = Now () + 8;
	impl->jobs.emplace (job.id, std::move (job));
	return true;
#else
	(void)peers;
	(void)hash;
	(void)completion;
	return false;
#endif
}
bool NATTransport::Ready () const
{
#ifdef QUIVER_HAVE_GNS
	return impl->initialized;
#else
	return false;
#endif
}
size_t NATTransport::Pending () const
{
#ifdef QUIVER_HAVE_GNS
	return impl->jobs.size ();
#else
	return 0;
#endif
}
Statistics NATTransport::Stats () const
{
#ifdef QUIVER_HAVE_GNS
	return impl->stats;
#else
	return {};
#endif
}
void NATTransport::Pump (size_t &budget)
{
#ifdef QUIVER_HAVE_GNS
	if (!impl->initialized)
		return;
	auto		&state = *impl;
	const double now = Now ();
	std::erase_if (state.tokens, [now] (const auto &t) { return t.second <= now; });
	SteamNetworkingSockets ()->RunCallbacks ();
	std::deque<std::pair<uint64_t, std::vector<uint8_t>>> signals;
	{
		std::lock_guard lock (state.signals->mutex);
		signals.swap (state.signals->messages);
	}
	for (const auto &signal : signals)
		if (state.peers.contains (signal.first))
			state.sender (signal.first, signal.second);
	for (auto it = state.connections.begin (); it != state.connections.end ();)
	{
		auto  handle = it->first;
		auto &c = it->second;
		++it;
		SteamNetConnectionInfo_t info;
		if (!SteamNetworkingSockets ()->GetConnectionInfo (handle, &info) || info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
			info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally || now >= c.deadline)
		{
			state.Close (handle);
			continue;
		}
		if (info.m_eState != k_ESteamNetworkingConnectionState_Connected)
			continue;
		if (!c.connected)
		{
			c.connected = true;
			++state.stats.natConnected;
		}
		if (c.outbound && c.job && !c.requestSent && budget >= 68)
		{
			auto job = state.jobs.find (c.job);
			auto peer = state.peers.find (c.peer);
			if (job == state.jobs.end () || peer == state.peers.end ())
				continue;
			std::vector<uint8_t> request{'Q', 'O', 'B', 1};
			request.insert (request.end (), peer->second.token.begin (), peer->second.token.end ());
			request.insert (request.end (), job->second.hash.begin (), job->second.hash.end ());
			if (SteamNetworkingSockets ()->SendMessageToConnection (
					handle, request.data (), uint32_t (request.size ()), k_nSteamNetworkingSend_Reliable, nullptr) == k_EResultOK)
			{
				c.requestSent = true;
				state.stats.uploadedBytes += request.size ();
				budget -= request.size ();
			}
		}
		if (!c.outbound && !c.response.empty ())
		{
			while (c.offset < c.response.size () && budget > 12)
			{
				size_t				 length = std::min ({BlockBytes, c.response.size () - c.offset, budget - 12});
				std::vector<uint8_t> packet{'Q', 'N', 'D', 1};
				Write32 (packet, uint32_t (c.response.size ()));
				Write32 (packet, uint32_t (c.offset));
				packet.insert (packet.end (), c.response.begin () + c.offset, c.response.begin () + c.offset + length);
				auto result = SteamNetworkingSockets ()->SendMessageToConnection (
					handle, packet.data (), uint32_t (packet.size ()), k_nSteamNetworkingSend_Reliable, nullptr);
				if (result != k_EResultOK)
					break;
				c.offset += length;
				budget -= packet.size ();
				state.stats.uploadedBytes += packet.size ();
			}
			if (c.offset == c.response.size ())
			{
				c.response.clear ();
				c.offset = 0;
				c.deadline = now + 30;
				++state.stats.served;
				++state.stats.natServed;
			}
		}
	}
	// Application receive work is bounded independently of GNS's bounded queues.
	for (int i = 0; i < 16 && budget >= BlockBytes + 12; ++i)
	{
		SteamNetworkingMessage_t *message = nullptr;
		if (SteamNetworkingSockets ()->ReceiveMessagesOnPollGroup (state.poll, &message, 1) != 1)
			break;
		budget -= std::min (budget, size_t (message->m_cbSize));
		state.Message (message);
		message->Release ();
	}
	for (auto it = state.jobs.begin (); it != state.jobs.end ();)
	{
		auto	&job = it->second;
		uint64_t id = job.id;
		++it;
		if (now >= job.deadline)
		{
			state.Finish (id, false);
			continue;
		}
		if (!job.connection || !state.connections.contains (job.connection) || now >= job.attemptDeadline)
			if (!state.Start (job))
				state.Finish (id, false);
	}

#else
	(void)budget;
#endif
}
std::vector<std::pair<Service::Completion, std::vector<uint8_t>>> NATTransport::TakeCompletions ()
{
#ifdef QUIVER_HAVE_GNS
	auto result = std::move (impl->completed);
	impl->completed.clear ();
	return result;
#else
	return {};
#endif
}

} // namespace quiver::peer
