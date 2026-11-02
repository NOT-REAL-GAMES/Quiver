#include "world_protocol.h"

#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

using namespace quiver;
using namespace quiver::wire;

#define CHECK(condition)                                                                   \
	do                                                                                     \
	{                                                                                      \
		if (!(condition))                                                                  \
			throw std::runtime_error (#condition " at line " + std::to_string (__LINE__)); \
	} while (0)

static void Primitives ()
{
	Writer writer;
	CHECK (writer.U64 (UINT64_C (0x0123456789abcdef)));
	CHECK (writer.U64 (UINT64_MAX));
	CHECK (writer.I64 (INT64_MIN));
	CHECK (writer.I64 (INT64_MAX));
	const std::array<uint8_t, 8> golden = {0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01};
	CHECK (std::equal (golden.begin (), golden.end (), writer.Data ().begin ()));
	Reader	 reader (writer.Data ());
	uint64_t u;
	int64_t	 i;
	CHECK (reader.U64 (u) && u == UINT64_C (0x0123456789abcdef));
	CHECK (reader.U64 (u) && u == UINT64_MAX);
	CHECK (reader.I64 (i) && i == INT64_MIN);
	CHECK (reader.I64 (i) && i == INT64_MAX);
	CHECK (!reader.U64 (u) && u == UINT64_MAX);
	Writer shortWriter (7);
	CHECK (!shortWriter.U64 (1) && shortWriter.Data ().empty ());
	Writer positionWriter (35);
	CHECK (!positionWriter.Position ({}) && positionWriter.Data ().empty ());
	for (size_t size = 0; size < 8; ++size)
	{
		Reader truncated{std::span (golden).first (size)};
		u = 42;
		CHECK (!truncated.U64 (u) && u == 42);
	}
}

static void Packets ()
{
	const Config config{UINT64_MAX, Generator::Terrain};
	auto		 bytes = EncodeConfig (config);
	Config		 decoded;
	CHECK (DecodeConfig (bytes, decoded) && decoded.seed == config.seed && decoded.generator == config.generator);
	for (size_t size = 0; size < bytes.size (); ++size)
		CHECK (!DecodeConfig (std::span (bytes).first (size), decoded));
	bytes[4] = 1; // Old clients use voxel collision and must not join this world.
	CHECK (!DecodeConfig (bytes, decoded));
	bytes = EncodeConfig (config);
	bytes.push_back (0);
	CHECK (!DecodeConfig (bytes, decoded));
	bytes = EncodeConfig (config);
	bytes[16] = 255;
	CHECK (!DecodeConfig (bytes, decoded));
	bytes = EncodeConfig (config);
	bytes[17] ^= 1;
	CHECK (!DecodeConfig (bytes, decoded));

	WorldPosition position{{INT64_MIN, 987654321012345678, INT64_MAX}, {0.125f, 1023.999f, 512.25f}}, parsed;
	bytes = EncodePosition (position);
	CHECK (DecodePosition (bytes, parsed) && parsed.chunk == position.chunk && parsed.local == position.local);
	for (size_t size = 0; size < bytes.size (); ++size)
		CHECK (!DecodePosition (std::span (bytes).first (size), parsed));
	for (float invalid : {-1.0f, 1024.0f, std::numeric_limits<float>::infinity (), std::numeric_limits<float>::quiet_NaN ()})
	{
		auto bad = position;
		bad.local[0] = invalid;
		CHECK (EncodePosition (bad).empty ());
		Writer malformed;
		CHECK (malformed.U32 (ProtocolMarker) && malformed.U16 (ProtocolVersion) && malformed.U16 (2));
		CHECK (malformed.I64 (0) && malformed.I64 (0) && malformed.I64 (0));
		CHECK (malformed.F32 (invalid) && malformed.F32 (0) && malformed.F32 (0));
		CHECK (!DecodePosition (malformed.Data (), parsed));
		CHECK (parsed.chunk == position.chunk && parsed.local == position.local);
	}
	Edit edit{UINT64_MAX, UINT64_MAX - 1, UINT64_MAX, position, {7, Solid}}, parsedEdit;
	bytes = EncodeEdit (edit);
	CHECK (DecodeEdit (bytes, parsedEdit));
	CHECK (parsedEdit.requestId == edit.requestId && parsedEdit.newRevision == edit.newRevision && parsedEdit.cell == edit.cell);
	for (size_t size = 0; size < bytes.size (); ++size)
		CHECK (!DecodeEdit (std::span (bytes).first (size), parsedEdit));
	edit.newRevision = 123;
	CHECK (EncodeEdit (edit).empty ());
	edit.expectedRevision = UINT64_MAX;
	edit.newRevision = 1;
	CHECK (EncodeEdit (edit).empty ());
	edit.newRevision = 0;
	CHECK (!EncodeEdit (edit).empty ());
	edit.cell.contents = Unknown;
	CHECK (EncodeEdit (edit).empty ());
	edit.cell = {0, Empty};
	CHECK (!EncodeEdit (edit).empty ());
	edit.requestId = 0;
	CHECK (EncodeEdit (edit).empty ());
}

static ChunkFragment Fragment (uint64_t id, const std::vector<uint8_t> &data, size_t offset)
{
	ChunkFragment fragment;
	fragment.key = {INT64_MIN + static_cast<int64_t> (id), 1234567890, INT64_MAX};
	fragment.revision = 99;
	fragment.transferId = id;
	fragment.totalBytes = static_cast<uint32_t> (data.size ());
	fragment.offset = static_cast<uint32_t> (offset);
	fragment.payload.assign (data.begin () + offset, data.begin () + std::min (data.size (), offset + FragmentBytes));
	return fragment;
}

static void Fragments ()
{
	std::vector<uint8_t> data (FragmentBytes * 2 + 1);
	for (size_t i = 0; i < data.size (); ++i)
		data[i] = static_cast<uint8_t> (i);
	auto		  first = Fragment (1, data, 0);
	ChunkFragment decoded;
	auto		  bytes = EncodeFragment (first);
	CHECK (bytes.size () < 1232 && DecodeFragment (bytes, decoded) && decoded.payload == first.payload);
	for (size_t size = 0; size < bytes.size (); ++size)
		CHECK (!DecodeFragment (std::span (bytes).first (size), decoded));
	bytes.push_back (0);
	CHECK (!DecodeFragment (bytes, decoded));
	auto bad = first;
	bad.offset = 1;
	CHECK (EncodeFragment (bad).empty ());
	bad = first;
	bad.totalBytes = UINT32_MAX;
	CHECK (EncodeFragment (bad).empty ());
	bad = first;
	bad.payload.push_back (0);
	CHECK (EncodeFragment (bad).empty ());

	Reassembler	   receiver ({2, 4096, 4096, 100});
	CompletedChunk result;
	CHECK (receiver.Receive (Fragment (1, data, 2048), 0, result) == ReceiveResult::Pending);
	CHECK (receiver.Receive (first, 1, result) == ReceiveResult::Pending);
	CHECK (receiver.Receive (first, 2, result) == ReceiveResult::Duplicate);
	CHECK (receiver.BufferedBytes () == data.size ());
	CHECK (receiver.Receive (Fragment (1, data, 1024), 3, result) == ReceiveResult::Complete);
	CHECK (result.bytes == data && result.key == first.key && result.revision == 99);
	CHECK (receiver.BufferedBytes () == 0 && receiver.OutstandingTransfers () == 0);
	CHECK (receiver.Receive (first, 4, result) == ReceiveResult::Duplicate);
	first.revision++;
	CHECK (receiver.Receive (first, 5, result) == ReceiveResult::Rejected);
	receiver.Reset ();
	CHECK (receiver.Receive (first, 10, result) == ReceiveResult::Pending);
	CHECK (receiver.Receive (Fragment (2, data, 0), 11, result) == ReceiveResult::BudgetExceeded);
	first.payload[0] ^= 1;
	CHECK (receiver.Receive (first, 12, result) == ReceiveResult::Rejected);
	CHECK (receiver.BufferedBytes () == 0);
	CHECK (receiver.Receive (first, 13, result) == ReceiveResult::Pending);
	first.key.x++;
	CHECK (receiver.Receive (first, 14, result) == ReceiveResult::Rejected);
	CHECK (receiver.BufferedBytes () == 0);
	CHECK (receiver.Receive (first, 20, result) == ReceiveResult::Pending);
	CHECK (receiver.Receive (first, 90, result) == ReceiveResult::Duplicate);
	receiver.Expire (120); // Duplicates must not extend the deadline.
	CHECK (receiver.OutstandingTransfers () == 0);
	CHECK (receiver.Receive (first, 200, result) == ReceiveResult::Pending);
	receiver.Expire (199);
	CHECK (receiver.OutstandingTransfers () == 0);
	Reassembler one ({1, 4096, 8192, 100});
	CHECK (one.Receive (first, 0, result) == ReceiveResult::Pending);
	CHECK (one.Receive (Fragment (2, data, 0), 0, result) == ReceiveResult::BudgetExceeded);
}

// This is a deterministic codec/transport simulation, not an engine networking load test.
static void SimulatedSessions ()
{
	constexpr size_t						  clients = 64;
	std::array<Reassembler, clients>		  receivers;
	std::array<std::vector<uint8_t>, clients> expected;
	std::array<bool, clients>				  complete{};
	struct Delivery
	{
		size_t				 client;
		std::vector<uint8_t> packet;
	};
	std::vector<Delivery> deliveries, delayed;
	std::mt19937		  random (0x51565231);
	for (size_t client = 0; client < clients; ++client)
	{
		expected[client].resize (8193 + client * 37);
		for (auto &byte : expected[client])
			byte = static_cast<uint8_t> (random ());
		for (size_t offset = 0; offset < expected[client].size (); offset += FragmentBytes)
			deliveries.push_back ({client, EncodeFragment (Fragment (client + 1, expected[client], offset))});
	}
	std::shuffle (deliveries.begin (), deliveries.end (), random);
	uint64_t clock = 1;
	auto	 deliver = [&] (const Delivery &delivery)
	{
		ChunkFragment fragment;
		CHECK (DecodeFragment (delivery.packet, fragment));
		CompletedChunk result;
		auto		   status = receivers[delivery.client].Receive (fragment, clock++, result);
		CHECK (status == ReceiveResult::Pending || status == ReceiveResult::Complete || status == ReceiveResult::Duplicate);
		if (status == ReceiveResult::Complete)
		{
			CHECK (!complete[delivery.client]);
			CHECK (result.bytes == expected[delivery.client]);
			complete[delivery.client] = true;
		}
		CHECK (receivers[delivery.client].BufferedBytes () <= expected[delivery.client].size ());
	};
	for (const auto &delivery : deliveries)
	{
		if (random () % 10 == 0)
			delayed.push_back (delivery);
		else
		{
			deliver (delivery);
			if (random () % 4 == 0)
				deliver (delivery);
		}
	}
	std::shuffle (delayed.begin (), delayed.end (), random);
	for (const auto &delivery : delayed)
		deliver (delivery);
	for (size_t client = 0; client < clients; ++client)
		CHECK (complete[client] && receivers[client].BufferedBytes () == 0);
	std::cout << "64 simulated sessions: reordered, duplicated, delayed/retransmitted chunk packets passed\n";
}

static void MerkleSessions ()
{
	WorldConfig config;
	config.seed = 0x0123456789abcdef;
	World  server (config), client (config);
	Config announcement{config.seed, config.generator};
	announcement.descriptor = server.DescriptorBytes ();
	announcement.root = server.CurrentRoot ();
	announcement.epoch = server.AuthorityEpoch ();
	Config received;
	CHECK (DecodeConfig (EncodeConfig (announcement), received));
	CHECK (received.descriptor == client.DescriptorBytes () && received.root == ContentHash{});
	CHECK (received.epoch == announcement.epoch);
	RootState root{announcement.root, announcement.epoch}, decodedRoot;
	CHECK (DecodeRoot (EncodeRoot (root), decodedRoot) && decodedRoot.root == root.root && decodedRoot.epoch == root.epoch);
	root.epoch = 0;
	CHECK (EncodeRoot (root).empty ());
	WorldConfig otherConfig = config;
	otherConfig.seed++;
	World incompatible (otherConfig);
	CHECK (received.descriptor != incompatible.DescriptorBytes ());
	for (ChunkKey key : {ChunkKey{}, ChunkKey{-1000000000, 1000000000, 0}, ChunkKey{INT64_MIN, INT64_MAX, -1}})
	{
		TerrainProof proof, decoded;
		CHECK (server.GetTerrainProof (key, proof));
		auto bytes = EncodeTerrainProof (proof);
		CHECK (bytes.size () < 128); // Seed-only worlds never transmit voxel payloads.
		CHECK (DecodeTerrainProof (bytes, decoded) && ValidateTerrainProof (decoded));
		CHECK (!client.Find (key)); // Unknown until authoritative absence arrives.
		CHECK (client.InstallTerrainProof (decoded) && server.Require (key));
		CHECK (client.Find (key)->cells.Encode () == server.Find (key)->cells.Encode ());
	}
	WorldPosition position{{}, {64, 64, 32}};
	const Cell	  generatedCell = server.Sample (position);
	CHECK (server.Edit (position, {}));
	TerrainProof proof, decoded;
	CHECK (server.GetTerrainProof ({}, proof) && proof.overrideObject && proof.root != ContentHash{});
	auto bytes = EncodeTerrainProof (proof);
	CHECK (DecodeTerrainProof (bytes, decoded) && ValidateTerrainProof (decoded));
	CHECK (client.InstallTerrainProof (decoded));
	CHECK (client.Sample (position).contents == Empty);
	for (size_t size = 0; size < bytes.size (); ++size)
		CHECK (!DecodeTerrainProof (std::span (bytes).first (size), decoded));
	auto originalRoot = proof.root;
	proof.root[0] ^= 1;
	CHECK (!ValidateTerrainProof (proof));
	proof.root = originalRoot;
	ContentObject object = *proof.overrideObject, parsed;
	auto		  objectBytes = EncodeContentObject (object);
	CHECK (DecodeContentObject (objectBytes, parsed) && parsed.hash == object.hash && parsed.bytes == object.bytes);
	objectBytes.back () ^= 1;
	CHECK (!DecodeContentObject (objectBytes, parsed));
	proof.overrideObject->bytes.clear ();
	CHECK (DecodeTerrainProof (EncodeTerrainProof (proof), decoded));
	CHECK (!ValidateTerrainProof (decoded)); // An offer alone never makes terrain known.
	decoded.overrideObject = object;
	CHECK (ValidateTerrainProof (decoded));
	CHECK (client.InstallTerrainProof (decoded));
	const TerrainProof stale = decoded;
	CHECK (server.Edit (position, generatedCell));
	TerrainProof restored;
	CHECK (server.GetTerrainProof ({}, restored));
	CHECK (restored.revision > stale.revision && !restored.overrideObject && restored.root == ContentHash{});
	CHECK (client.InstallTerrainProof (restored));
	CHECK (client.InstallTerrainProof (stale));
	CHECK (client.Sample (position).contents == Solid && client.Find ({})->revision == restored.revision);
	// Restore a sparse edit so the following absence proof traverses a nonempty tree.
	CHECK (server.Edit (position, {}));
	TerrainProof missing;
	CHECK (server.GetTerrainProof ({17, -13, 8}, missing));
	CHECK (!missing.overrideObject && !missing.nodes.empty () && ValidateTerrainProof (missing));
	CHECK (DecodeTerrainProof (EncodeTerrainProof (missing), decoded) && client.InstallTerrainProof (decoded));
	missing.nodes.clear ();
	CHECK (!ValidateTerrainProof (missing)); // Missing nodes cannot masquerade as untouched terrain.
	Edit edit;
	edit.requestId = 1;
	edit.epoch = 0;
	CHECK (EncodeEdit (edit).empty ());
}

static void PeerSignaling ()
{
	PeerDirectory directory;
	directory.epoch = 7;
	directory.recipientId = 999;
	directory.sessionId[0] = 1;
	directory.token[0] = 2;
	directory.stunServers = "stun.example.test:19302";
	for (size_t i = 1; i <= MaxSessionPeers; ++i)
		directory.peers.push_back ({i, true, uint16_t (i & 1 ? 23456 : 0), i & 1 ? "255.255.255.255" : ""});
	auto bytes = EncodePeerDirectory (directory);
	CHECK (!bytes.empty () && bytes.size () <= 4096);
	PeerDirectory decoded;
	CHECK (DecodePeerDirectory (bytes, decoded) && decoded.recipientId == directory.recipientId && decoded.sessionId == directory.sessionId);
	CHECK (decoded.peers.size () == MaxSessionPeers && decoded.peers.back ().peerId == MaxSessionPeers && decoded.peers.back ().nat);
	for (size_t size = 0; size < bytes.size (); ++size)
		CHECK (!DecodePeerDirectory (std::span (bytes).first (size), decoded));
	bytes.push_back (0);
	CHECK (!DecodePeerDirectory (bytes, decoded));
	directory.peers.back ().peerId = directory.peers.front ().peerId;
	CHECK (EncodePeerDirectory (directory).empty ());
	directory.peers.back ().peerId = directory.recipientId;
	CHECK (EncodePeerDirectory (directory).empty ());
	directory.peers.back ().peerId = MaxSessionPeers;
	directory.peers.back ().nat = false;
	CHECK (EncodePeerDirectory (directory).empty ()); // No usable transport.
	directory.peers.back ().nat = true;
	directory.sessionId = {};
	CHECK (EncodePeerDirectory (directory).empty ());
	std::vector<uint8_t> data (MaxSignalBytes);
	for (size_t i = 0; i < data.size (); ++i)
		data[i] = uint8_t (i * 7);
	auto fragment = [&] (uint64_t peerId, uint64_t messageId, size_t offset)
	{
		SignalFragment result{7, peerId, messageId, uint16_t (data.size ()), uint16_t (offset), {}};
		result.payload.assign (data.begin () + offset, data.begin () + std::min (data.size (), offset + SignalFragmentBytes));
		return result;
	};
	SignalFragment parsed;
	bytes = EncodeSignalFragment (fragment (1, 1, 0));
	CHECK (bytes.size () + 4 <= 1024 && DecodeSignalFragment (bytes, parsed) && parsed.payload == fragment (1, 1, 0).payload);
	for (size_t size = 0; size < bytes.size (); ++size)
		CHECK (!DecodeSignalFragment (std::span (bytes).first (size), parsed));
	bytes.push_back (0);
	CHECK (!DecodeSignalFragment (bytes, parsed));
	auto invalid = fragment (1, 1, 0);
	invalid.epoch = 0;
	CHECK (EncodeSignalFragment (invalid).empty ());
	invalid = fragment (1, 1, 0);
	invalid.totalBytes = MaxSignalBytes + 1;
	CHECK (EncodeSignalFragment (invalid).empty ());
	invalid = fragment (1, 1, 0);
	invalid.offset = 1;
	CHECK (EncodeSignalFragment (invalid).empty ());
	CHECK (EncodeSignalFragment (fragment (0, 1, 0)).empty ());
	SignalReassembler assembler;
	CompletedSignal	  completed;
	for (size_t offset = data.size (); offset > 0; offset -= SignalFragmentBytes)
	{
		auto result = assembler.Receive (fragment (1, 1, offset - SignalFragmentBytes), 100, completed);
		CHECK (result == (offset == SignalFragmentBytes ? ReceiveResult::Complete : ReceiveResult::Pending));
	}
	CHECK (completed.bytes == data && completed.epoch == 7 && completed.peerId == 1 && assembler.BufferedBytes () == 0);
	CHECK (assembler.Receive (fragment (1, 1, 0), 101, completed) == ReceiveResult::Duplicate);
	for (uint64_t peerId = 2; peerId < 6; ++peerId)
		CHECK (assembler.Receive (fragment (peerId, 1, 0), 200, completed) == ReceiveResult::Pending);
	CHECK (assembler.BufferedBytes () == 32768);
	CHECK (assembler.Receive (fragment (6, 1, 0), 201, completed) == ReceiveResult::BudgetExceeded);
	CHECK (assembler.Receive (fragment (2, 1, 0), 5100, completed) == ReceiveResult::Duplicate);
	assembler.Expire (5200);
	CHECK (assembler.BufferedBytes () == 0); // Duplicates cannot pin memory.
	CHECK (assembler.Receive (fragment (2, 1, 0), 6000, completed) == ReceiveResult::Pending);
	invalid = fragment (2, 1, 0);
	invalid.payload[0] ^= 1;
	CHECK (assembler.Receive (invalid, 6001, completed) == ReceiveResult::Rejected && assembler.BufferedBytes () == 0);
	CHECK (assembler.Receive (fragment (2, 1, 0), 7000, completed) == ReceiveResult::Pending);
	invalid = fragment (2, 1, SignalFragmentBytes);
	++invalid.epoch;
	CHECK (assembler.Receive (invalid, 7001, completed) == ReceiveResult::Rejected);
	assembler.Reset ();
	CHECK (assembler.BufferedBytes () == 0);
}

static void MalformedInputs ()
{
	std::mt19937 random (271828);
	for (int test = 0; test < 10000; ++test)
	{
		std::vector<uint8_t> bytes (random () % 1200);
		for (auto &byte : bytes)
			byte = static_cast<uint8_t> (random ());
		Config		  config;
		WorldPosition position;
		Edit		  edit;
		ChunkFragment fragment;
		CHECK (!DecodeConfig (bytes, config));
		CHECK (!DecodePosition (bytes, position));
		CHECK (!DecodeEdit (bytes, edit));
		CHECK (!DecodeFragment (bytes, fragment));
		SignalFragment signal;
		PeerDirectory  peers;
		CHECK (!DecodeSignalFragment (bytes, signal));
		CHECK (!DecodePeerDirectory (bytes, peers));
	}
}

int main ()
{
	try
	{
		Primitives ();
		Packets ();
		Fragments ();
		SimulatedSessions ();
		MerkleSessions ();
		PeerSignaling ();
		MalformedInputs ();
		std::cout << "world protocol tests passed\n";
	}
	catch (const std::exception &error)
	{
		std::cerr << error.what () << '\n';
		return 1;
	}
}
