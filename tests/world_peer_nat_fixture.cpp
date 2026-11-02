/* SPDX-License-Identifier: GPL-2.0-or-later */
// Process fixture for run_world_nat_smoke.py. Signaling is routed over stdio;
// object bytes must travel through the production Service's real ICE sockets.
#include "world_peer.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#ifdef QUIVER_HAVE_GNS
#include <steam/isteamnetworkingutils.h>
#endif

using namespace quiver::peer;

static std::string Hex (const std::vector<uint8_t> &bytes)
{
	constexpr char digits[] = "0123456789abcdef";
	std::string	   result;
	for (uint8_t byte : bytes)
	{
		result += digits[byte >> 4];
		result += digits[byte & 15];
	}
	return result;
}
static bool Unhex (const std::string &text, std::vector<uint8_t> &bytes)
{
	if (text.size () % 2 || text.size () > 16384)
		return false;
	auto digit = [] (char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
	for (size_t i = 0; i < text.size (); i += 2)
	{
		int a = digit (text[i]), b = digit (text[i + 1]);
		if (a < 0 || b < 0)
			return false;
		bytes.push_back (uint8_t (a * 16 + b));
	}
	return true;
}
int main (int argc, char **argv)
{
	uint64_t	local = 0, peer = 0;
	std::string role, stun, sessionText, authText;
	for (int i = 1; i + 1 < argc; i += 2)
	{
		const std::string key = argv[i], value = argv[i + 1];
		if (key == "--identity")
			local = std::stoull (value);
		else if (key == "--peer")
			peer = std::stoull (value);
		else if (key == "--role")
			role = value;
		else if (key == "--stun")
			stun = value;
		else if (key == "--session")
			sessionText = value;
		else if (key == "--auth")
			authText = value;
		else
			return 2;
	}
	if (!local || !peer || (role != "provider" && role != "receiver") || !Service::NATAvailable ())
		return 2;
	std::vector<uint8_t> sessionBytes, authBytes;
	if (!Unhex (sessionText, sessionBytes) || sessionBytes.size () != 32 || !Unhex (authText, authBytes) || authBytes.size () != 32)
		return 2;
	Token session{}, auth{};
	std::copy (sessionBytes.begin (), sessionBytes.end (), session.begin ());
	std::copy (authBytes.begin (), authBytes.end (), auth.begin ());
	Service service;
	if (!service.Listen () || !service.ConfigureSession (
								  local, session, stun, [] (uint64_t target, const std::vector<uint8_t> &bytes)
								  { std::cout << "SIGNAL " << target << " " << Hex (bytes) << std::endl; }))
		return 3;
#ifdef QUIVER_HAVE_GNS
	SteamNetworkingUtils ()->SetDebugOutputFunction (
		k_ESteamNetworkingSocketsDebugOutputType_Verbose,
		[] (ESteamNetworkingSocketsDebugOutputType, const char *message) { std::fprintf (stderr, "%s\n", message); });
#endif
	Endpoint endpoint;
	endpoint.peerId = peer;
	endpoint.nat = true;
	endpoint.token = auth;
	// Identity-binding probes must use another introduced peer, so rejection
	// cannot be explained by the unknown-peer admission check alone.
	Endpoint introduced = endpoint;
	introduced.peerId = 3;
	service.SetPeers ({endpoint, introduced});
	service.Authorize (auth);
	Hash hash{};
	hash.fill (0x7b);
	std::vector<uint8_t> object (128 * 1024);
	for (size_t i = 0; i < object.size (); ++i)
		object[i] = uint8_t ((i * 37 + i / 17) & 255);
	service.SetProvider (
		[&] (const Hash &requested, std::vector<uint8_t> &bytes)
		{
			if (role != "provider" || requested != hash)
				return false;
			bytes = object;
			return true;
		});
	// Verify admission and input-size limits before accepting any broker input.
	if (service.ReceiveSignal (999999, {1, 2, 3}) || service.ReceiveSignal (peer, std::vector<uint8_t> (8193, 0)))
		return 4;
	fcntl (STDIN_FILENO, F_SETFL, fcntl (STDIN_FILENO, F_GETFL) | O_NONBLOCK);
	std::string input;
	bool		fetched = false, resultReady = false;
	std::string resultStatus;
	size_t		resultSize = 0;
	std::cout << "READY" << std::endl;
	const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30);
	while (std::chrono::steady_clock::now () < deadline)
	{
		char	buffer[4096];
		ssize_t count;
		while ((count = read (STDIN_FILENO, buffer, sizeof buffer)) > 0)
		{
			input.append (buffer, size_t (count));
			if (input.size () > 65536)
				return 5;
		}
		for (size_t end; (end = input.find ('\n')) != std::string::npos;)
		{
			std::string line = input.substr (0, end);
			input.erase (0, end + 1);
			if (line == "FETCH" && role == "receiver" && !fetched)
			{
				fetched = true;
				if (!service.Fetch (
						{endpoint}, hash,
						[&] (bool success, std::vector<uint8_t> bytes)
						{
							resultStatus = success && bytes != object ? "corrupt" : success ? "success" : "failure";
							resultSize = bytes.size ();
							resultReady = true;
							fetched = false;
						}))
					return 6;
			}
			else if (line == "REVOKE" && role == "provider")
			{
				service.RevokeAll ();
				std::cout << "REVOKED" << std::endl;
			}
			else if (line.starts_with ("SIGNAL "))
			{
				const size_t split = line.find (' ', 7);
				if (split == std::string::npos)
					return 7;
				std::vector<uint8_t> bytes;
				if (!Unhex (line.substr (split + 1), bytes))
					return 7;
				// Mismatched-session or expired signals are expected to be rejected.
				service.ReceiveSignal (std::stoull (line.substr (7, split - 7)), bytes);
			}
			else if (line.starts_with ("CHECK "))
			{
				std::istringstream	 command (line);
				std::string			 verb, label, encoded, extra;
				uint64_t			 source = 0;
				std::vector<uint8_t> bytes;
				if (!(command >> verb >> label >> source >> encoded) || command >> extra || !Unhex (encoded, bytes))
					return 7;
				const bool accepted = service.ReceiveSignal (source, bytes);
				std::cout << "CHECK " << label << " " << (accepted ? 1 : 0) << std::endl;
			}
			else
				return 8;
		}
		service.Pump ();
		if (resultReady)
		{
			resultReady = false;
			const auto stats = service.Stats ();
			std::cout << "RESULT " << resultStatus << " " << resultSize << " " << stats.natFetched << " " << stats.natConnected << std::endl;
		}
		std::this_thread::sleep_for (std::chrono::milliseconds (1));
	}
	return 9;
}
