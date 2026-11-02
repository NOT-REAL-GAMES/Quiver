/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world_peer.h"
#include "world_peer_gns.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/select.h>
#endif

namespace quiver::peer
{
namespace
{
using Clock = std::chrono::steady_clock;
double Now ()
{
	return std::chrono::duration<double> (Clock::now ().time_since_epoch ()).count ();
}
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket Invalid = INVALID_SOCKET;
int				 Error ()
{
	return WSAGetLastError ();
}
bool Again ()
{
	int e = Error ();
	return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
}
void Close (Socket s)
{
	if (s != Invalid)
		closesocket (s);
}
bool Nonblocking (Socket s)
{
	u_long one = 1;
	return ioctlsocket (s, FIONBIO, &one) == 0;
}
#else
using Socket = int;
constexpr Socket Invalid = -1;
int				 Error ()
{
	return errno;
}
bool Again ()
{
	int e = Error ();
	return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS || e == EINTR;
}
void Close (Socket s)
{
	if (s != Invalid)
		close (s);
}
bool Nonblocking (Socket s)
{
	int flags = fcntl (s, F_GETFL, 0);
	if (flags < 0 || fcntl (s, F_SETFL, flags | O_NONBLOCK) < 0)
		return false;
	fcntl (s, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
	int one = 1;
	setsockopt (s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof (one));
#endif
	return true;
}
#endif
int Send (Socket s, const uint8_t *data, size_t length)
{
	int flags = 0;
#ifdef MSG_NOSIGNAL
	flags = MSG_NOSIGNAL;
#endif
	return static_cast<int> (send (s, reinterpret_cast<const char *> (data), static_cast<int> (length), flags));
}
int Receive (Socket s, uint8_t *data, size_t length)
{
	return static_cast<int> (recv (s, reinterpret_cast<char *> (data), static_cast<int> (length), 0));
}
struct Connection
{
	Socket				 socket = Invalid;
	std::vector<uint8_t> bytes;
	size_t				 offset = 0;
	double				 deadline = 0;
	bool				 sending = false;
};
struct Job
{
	Connection			  connection;
	std::vector<Endpoint> endpoints;
	size_t				  next = 0;
	Hash				  hash;
	Service::Completion	  completion;
	double				  deadline = 0;
	bool				  connecting = false;
	uint32_t			  expected = 0;
};
bool Start (Job &job, double now)
{
	Close (job.connection.socket);
	job.connection = {};
	job.expected = 0;
	while (job.next < job.endpoints.size ())
	{
		const auto &endpoint = job.endpoints[job.next++];
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons (endpoint.port);
		if (!endpoint.port || inet_pton (AF_INET, endpoint.address.c_str (), &address.sin_addr) != 1)
			continue;
		Socket socket = ::socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (socket == Invalid)
			continue;
		if (!Nonblocking (socket))
		{
			Close (socket);
			continue;
		}
		int result = connect (socket, reinterpret_cast<sockaddr *> (&address), sizeof (address));
		if (result != 0 && !Again ())
		{
			Close (socket);
			continue;
		}
		job.connecting = result != 0;
		job.connection.socket = socket;
		job.connection.deadline = now + .75;
		job.connection.sending = true;
		job.connection.bytes = {'Q', 'O', 'B', 1};
		job.connection.bytes.insert (job.connection.bytes.end (), endpoint.token.begin (), endpoint.token.end ());
		job.connection.bytes.insert (job.connection.bytes.end (), job.hash.begin (), job.hash.end ());
		return true;
	}
	return false;
}
} // namespace

bool RandomToken (Token &token)
{
	token.fill (0);
#ifdef _WIN32
	HMODULE module = LoadLibraryW (L"bcrypt.dll");
	if (!module)
		return false;
	using Generate = NTSTATUS (WINAPI *) (BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG);
	auto generate = reinterpret_cast<Generate> (GetProcAddress (module, "BCryptGenRandom"));
	bool ok = generate && generate (nullptr, token.data (), static_cast<ULONG> (token.size ()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
	FreeLibrary (module);
	return ok;
#else
	int file = open ("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (file < 0)
		return false;
	size_t offset = 0;
	while (offset < token.size ())
	{
		ssize_t count = read (file, token.data () + offset, token.size () - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
		{
			close (file);
			token.fill (0);
			return false;
		}
		offset += static_cast<size_t> (count);
	}
	close (file);
	return true;
#endif
}

struct Service::Impl
{
	NATTransport						  nat;
	Socket								  listener = Invalid;
	uint16_t							  port = 0;
	Provider							  provider;
	std::vector<std::pair<Token, double>> tokens;
	std::vector<Connection>				  incoming;
	std::vector<Job>					  jobs;
	Statistics							  stats;
	double								  last = Now (), allowance = 256 * 1024;
	bool								  FetchTCP (std::vector<Endpoint> endpoints, const Hash &hash, Completion completion)
	{
		if (jobs.size () + nat.Pending () >= 8 || !completion || endpoints.empty ())
			return false;
		if (endpoints.size () > 8)
			endpoints.resize (8);
		Job job;
		job.endpoints = std::move (endpoints);
		job.hash = hash;
		job.completion = std::move (completion);
		job.deadline = Now () + 3;
		jobs.push_back (std::move (job));
		return true;
	}
#ifdef _WIN32
	bool winsock = false;
	Impl ()
	{
		WSADATA data;
		winsock = WSAStartup (MAKEWORD (2, 2), &data) == 0;
	}
#endif
	~Impl ()
	{
		Close (listener);
		for (auto &connection : incoming)
			Close (connection.socket);
		for (auto &job : jobs)
			Close (job.connection.socket);
#ifdef _WIN32
		if (winsock)
			WSACleanup ();
#endif
	}
};
Service::Service () : impl (std::make_unique<Impl> ()) {}
Service::~Service () = default;
void Service::Reset ()
{
	impl = std::make_unique<Impl> ();
}
uint16_t Service::Port () const
{
	return impl->port;
}
Statistics Service::Stats () const
{
	auto result = impl->nat.Stats ();
	result.uploadedBytes += impl->stats.uploadedBytes;
	result.downloadedBytes += impl->stats.downloadedBytes;
	result.served += impl->stats.served;
	result.fetched += impl->stats.fetched;
	result.failed += impl->stats.failed;
	return result;
}
void Service::SetProvider (Provider provider)
{
	impl->nat.SetProvider (provider);
	impl->provider = std::move (provider);
}
bool Service::NATAvailable ()
{
	return NATTransport::Available ();
}
bool Service::ConfigureSession (uint64_t localPeerId, const Token &sessionId, const std::string &stunServers, SignalSender sender)
{
	if (!impl->nat.Configure (localPeerId, sessionId, stunServers, std::move (sender)))
		return false;
	impl->nat.SetProvider (impl->provider);
	for (const auto &token : impl->tokens)
		if (token.second > Now ())
			impl->nat.Authorize (token.first, token.second - Now ());
	return true;
}
void Service::DisableNAT ()
{
	impl->nat.Disable ();
}
void Service::SetPeers (const std::vector<Endpoint> &peers)
{
	impl->nat.SetPeers (peers);
}
bool Service::ReceiveSignal (uint64_t sourcePeerId, const std::vector<uint8_t> &bytes)
{
	return impl->nat.ReceiveSignal (sourcePeerId, bytes);
}
void Service::RevokeAll ()
{
	impl->nat.RevokeAll ();
	impl->tokens.clear ();
	for (auto &connection : impl->incoming)
		Close (connection.socket);
	impl->incoming.clear ();
}
void Service::Authorize (const Token &token, double lifetimeSeconds)
{
	impl->nat.Authorize (token, lifetimeSeconds);
	const double expiry = Now () + std::clamp (lifetimeSeconds, 1.0, 300.0);
	for (auto &entry : impl->tokens)
		if (entry.first == token)
		{
			entry.second = expiry;
			return;
		}
	if (impl->tokens.size () < 128)
		impl->tokens.emplace_back (token, expiry);
}
bool Service::Listen (uint16_t port)
{
	Close (impl->listener);
	impl->listener = Invalid;
	impl->port = 0;
	Socket socket = ::socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket == Invalid)
		return false;
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl (INADDR_ANY);
	address.sin_port = htons (port);
	if (!Nonblocking (socket) || bind (socket, reinterpret_cast<sockaddr *> (&address), sizeof (address)) != 0 || listen (socket, 8) != 0)
	{
		Close (socket);
		return false;
	}
#ifdef _WIN32
	int length = sizeof (address);
#else
	socklen_t length = sizeof (address);
#endif
	if (getsockname (socket, reinterpret_cast<sockaddr *> (&address), &length) != 0)
	{
		Close (socket);
		return false;
	}
	impl->listener = socket;
	impl->port = ntohs (address.sin_port);
	return true;
}
bool Service::Fetch (std::vector<Endpoint> endpoints, const Hash &hash, Completion completion)
{
	if (impl->jobs.size () + impl->nat.Pending () >= 8 || !completion || endpoints.empty ())
		return false;
	if (impl->nat.Ready ())
	{
		auto fallback = [state = impl.get (), endpoints, hash, completion] (bool success, std::vector<uint8_t> bytes) mutable
		{
			if (success)
			{
				completion (true, std::move (bytes));
				return;
			}
			if (!state->FetchTCP (std::move (endpoints), hash, completion))
				completion (false, {});
		};
		if (impl->nat.Fetch (endpoints, hash, std::move (fallback)))
			return true;
	}
	return impl->FetchTCP (std::move (endpoints), hash, std::move (completion));
}
void Service::Pump ()
{
	auto		&state = *impl;
	const double now = Now ();
	std::erase_if (state.tokens, [now] (const auto &token) { return token.second <= now; });
	state.allowance = std::min (256.0 * 1024, state.allowance + (now - state.last) * 2 * 1024 * 1024);
	state.last = now;
	size_t		 budget = static_cast<size_t> (state.allowance);
	const size_t initialBudget = budget;
	state.nat.Pump (budget);
	if (state.listener != Invalid)
		for (int i = 0; i < 8 && state.incoming.size () < 8; ++i)
		{
			Socket socket = accept (state.listener, nullptr, nullptr);
			if (socket == Invalid)
				break;
			if (!Nonblocking (socket))
			{
				Close (socket);
				continue;
			}
			Connection connection;
			connection.socket = socket;
			connection.bytes.resize (68);
			connection.deadline = now + 3;
			state.incoming.push_back (std::move (connection));
		}
	for (auto it = state.incoming.begin (); it != state.incoming.end ();)
	{
		auto &c = *it;
		bool  done = now >= c.deadline;
		if (!done && budget)
		{
			size_t amount = std::min ({budget, c.bytes.size () - c.offset, size_t (65536)});
			int	   count = c.sending ? Send (c.socket, c.bytes.data () + c.offset, amount) : Receive (c.socket, c.bytes.data () + c.offset, amount);
			if (count == 0 || (count < 0 && !Again ()))
				done = true;
			if (count > 0)
			{
				c.offset += count;
				budget -= count;
				if (c.sending)
					state.stats.uploadedBytes += count;
				if (c.offset == c.bytes.size ())
				{
					if (c.sending)
					{
						done = true;
						++state.stats.served;
					}
					else
					{
						Token token;
						Hash  hash;
						std::copy_n (c.bytes.begin () + 4, 32, token.begin ());
						std::copy_n (c.bytes.begin () + 36, 32, hash.begin ());
						bool authorized = std::any_of (state.tokens.begin (), state.tokens.end (), [&] (const auto &entry) { return entry.first == token; });
						std::vector<uint8_t> payload;
						if (std::memcmp (c.bytes.data (), "QOB\1", 4) != 0 || !authorized || !state.provider || !state.provider (hash, payload) ||
							payload.empty () || payload.size () > MaxObjectBytes)
							done = true;
						else
						{
							c.bytes.resize (4);
							uint32_t size = static_cast<uint32_t> (payload.size ());
							for (int b = 0; b < 4; ++b)
								c.bytes[b] = static_cast<uint8_t> (size >> (8 * b));
							c.bytes.insert (c.bytes.end (), payload.begin (), payload.end ());
							c.offset = 0;
							c.sending = true;
						}
					}
				}
			}
		}
		if (done)
		{
			Close (c.socket);
			it = state.incoming.erase (it);
		}
		else
			++it;
	}
	// Deliver completions after iteration so callbacks can enqueue more requests.
	auto completions = state.nat.TakeCompletions ();
	for (auto it = state.jobs.begin (); it != state.jobs.end ();)
	{
		auto &job = *it;
		auto &c = job.connection;
		bool  failed = now >= job.deadline, success = false, retry = false;
		if (!failed && c.socket == Invalid && !Start (job, now))
			failed = true;
		if (!failed && now >= c.deadline)
			retry = true;
		if (!failed && !retry && job.connecting)
		{
			fd_set writable;
			FD_ZERO (&writable);
#ifndef _WIN32
			if (c.socket >= FD_SETSIZE)
				retry = true;
			else
#endif
			{
				FD_SET (c.socket, &writable);
				timeval timeout{};
				if (select (static_cast<int> (c.socket + 1), nullptr, &writable, nullptr, &timeout) > 0)
				{
					int error = 0;
#ifdef _WIN32
					int size = sizeof (error);
#else
					socklen_t size = sizeof (error);
#endif
					if (getsockopt (c.socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *> (&error), &size) != 0 || error)
						retry = true;
					else
						job.connecting = false;
				}
			}
		}
		if (!failed && !retry && !job.connecting && budget)
		{
			size_t amount = std::min ({budget, c.bytes.size () - c.offset, size_t (65536)});
			int	   count = c.sending ? Send (c.socket, c.bytes.data () + c.offset, amount) : Receive (c.socket, c.bytes.data () + c.offset, amount);
			if (count == 0 || (count < 0 && !Again ()))
				retry = true;
			if (count > 0)
			{
				c.offset += count;
				budget -= count;
				if (!c.sending)
					state.stats.downloadedBytes += count;
				c.deadline = std::min (job.deadline, now + 1.5);
				if (c.offset == c.bytes.size ())
				{
					if (c.sending)
					{
						c.sending = false;
						c.bytes.assign (4, 0);
						c.offset = 0;
					}
					else if (!job.expected)
					{
						for (int b = 0; b < 4; ++b)
							job.expected |= uint32_t (c.bytes[b]) << (8 * b);
						if (!job.expected || job.expected > MaxObjectBytes)
							retry = true;
						else
						{
							c.bytes.resize (job.expected);
							c.offset = 0;
						}
					}
					else
						success = true;
				}
			}
		}
		if (retry && !Start (job, now))
			failed = true;
		if (success || failed)
		{
			Close (c.socket);
			if (success)
				++state.stats.fetched;
			else
			{
				++state.stats.failed;
				c.bytes.clear ();
			}
			completions.emplace_back (std::move (job.completion), std::move (c.bytes));
			it = state.jobs.erase (it);
		}
		else
			++it;
	}
	state.allowance -= initialBudget - budget;
	for (auto &completion : completions)
	{
		bool success = !completion.second.empty ();
		completion.first (success, std::move (completion.second));
		if (impl.get () != &state)
			return; // Reset from a callback cancels the rest.
	}
}
} // namespace quiver::peer
