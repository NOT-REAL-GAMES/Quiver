#include "world_protocol.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

namespace quiver::wire
{
namespace
{
enum class Packet : uint16_t
{
	Config = 1,
	Position,
	Edit,
	Fragment,
	Root,
	Proof,
	Object,
	Signal,
	Peers
};

bool ValidPosition (const WorldPosition &position)
{
	for (float value : position.local)
		if (!std::isfinite (value) || value < 0 || value >= ChunkSize)
			return false;
	return true;
}

bool WriteHeader (Writer &writer, Packet type)
{
	return writer.U32 (ProtocolMarker) && writer.U16 (ProtocolVersion) && writer.U16 (static_cast<uint16_t> (type));
}

bool ReadHeader (Reader &reader, Packet expected)
{
	uint32_t marker;
	uint16_t version, type;
	return reader.U32 (marker) && reader.U16 (version) && reader.U16 (type) && marker == ProtocolMarker && version == ProtocolVersion &&
		   type == static_cast<uint16_t> (expected);
}

bool WriteKey (Writer &writer, ChunkKey key)
{
	return writer.I64 (key.x) && writer.I64 (key.y) && writer.I64 (key.z);
}

bool ReadKey (Reader &reader, ChunkKey &key)
{
	return reader.I64 (key.x) && reader.I64 (key.y) && reader.I64 (key.z);
}

bool ValidEdit (const Edit &edit)
{
	if (!edit.epoch || !edit.requestId || !ValidPosition (edit.position))
		return false;
	if (edit.newRevision && (edit.expectedRevision == UINT64_MAX || edit.newRevision != edit.expectedRevision + 1))
		return false;
	if (edit.cell.contents > Empty || edit.cell.contents < Lava)
		return false;
	return edit.cell.contents == Empty ? edit.cell.material == 0 : edit.cell.material != 0;
}

bool ValidFragment (const ChunkFragment &fragment)
{
	return fragment.transferId && fragment.totalBytes && fragment.totalBytes <= MaxChunkBytes && fragment.offset < fragment.totalBytes &&
		   fragment.offset % FragmentBytes == 0 && fragment.payload.size () == std::min<size_t> (FragmentBytes, fragment.totalBytes - fragment.offset);
}
} // namespace

bool Writer::Bytes (std::span<const uint8_t> value)
{
	if (value.size () > limit - data.size ())
		return false;
	data.insert (data.end (), value.begin (), value.end ());
	return true;
}

bool Writer::U8 (uint8_t value)
{
	return Bytes ({&value, 1});
}
bool Writer::U16 (uint16_t value)
{
	uint8_t bytes[2];
	for (size_t i = 0; i < sizeof (bytes); ++i)
		bytes[i] = static_cast<uint8_t> (value >> (8 * i));
	return Bytes (bytes);
}
bool Writer::U32 (uint32_t value)
{
	uint8_t bytes[4];
	for (size_t i = 0; i < sizeof (bytes); ++i)
		bytes[i] = static_cast<uint8_t> (value >> (8 * i));
	return Bytes (bytes);
}
bool Writer::U64 (uint64_t value)
{
	uint8_t bytes[8];
	for (size_t i = 0; i < sizeof (bytes); ++i)
		bytes[i] = static_cast<uint8_t> (value >> (8 * i));
	return Bytes (bytes);
}
bool Writer::I64 (int64_t value)
{
	return U64 (std::bit_cast<uint64_t> (value));
}
bool Writer::F32 (float value)
{
	return U32 (std::bit_cast<uint32_t> (value));
}
bool Writer::Position (const WorldPosition &position)
{
	if (!ValidPosition (position) || limit - data.size () < 36)
		return false;
	return WriteKey (*this, position.chunk) && F32 (position.local[0]) && F32 (position.local[1]) && F32 (position.local[2]);
}

bool Reader::Bytes (std::span<uint8_t> value)
{
	if (value.size () > Remaining ())
		return false;
	if (!value.empty ())
		std::memcpy (value.data (), data.data () + offset, value.size ());
	offset += value.size ();
	return true;
}
bool Reader::U8 (uint8_t &value)
{
	return Bytes ({&value, 1});
}
bool Reader::U16 (uint16_t &value)
{
	uint8_t bytes[2];
	if (!Bytes (bytes))
		return false;
	value = 0;
	for (size_t i = 0; i < sizeof (bytes); ++i)
		value |= static_cast<uint16_t> (bytes[i]) << (8 * i);
	return true;
}
bool Reader::U32 (uint32_t &value)
{
	uint8_t bytes[4];
	if (!Bytes (bytes))
		return false;
	value = 0;
	for (size_t i = 0; i < sizeof (bytes); ++i)
		value |= static_cast<uint32_t> (bytes[i]) << (8 * i);
	return true;
}
bool Reader::U64 (uint64_t &value)
{
	uint8_t bytes[8];
	if (!Bytes (bytes))
		return false;
	value = 0;
	for (size_t i = 0; i < sizeof (bytes); ++i)
		value |= static_cast<uint64_t> (bytes[i]) << (8 * i);
	return true;
}
bool Reader::I64 (int64_t &value)
{
	uint64_t bits;
	if (!U64 (bits))
		return false;
	value = std::bit_cast<int64_t> (bits);
	return true;
}
bool Reader::F32 (float &value)
{
	uint32_t bits;
	if (!U32 (bits))
		return false;
	value = std::bit_cast<float> (bits);
	return true;
}
bool Reader::Position (WorldPosition &position)
{
	WorldPosition candidate;
	if (!ReadKey (*this, candidate.chunk) || !F32 (candidate.local[0]) || !F32 (candidate.local[1]) || !F32 (candidate.local[2]) || !ValidPosition (candidate))
		return false;
	position = candidate;
	return true;
}

std::vector<uint8_t> EncodeConfig (const Config &config)
{
	if ((config.generator != Generator::Empty && config.generator != Generator::Terrain && config.generator != Generator::Planet) || !config.epoch ||
		config.descriptor.size () > 4096)
		return {};
	Writer writer;
	WriteHeader (writer, Packet::Config);
	writer.U64 (config.seed);
	writer.U8 (uint8_t (config.generator));
	writer.U16 (ChunkSize);
	writer.U16 (VoxelSize);
	writer.U64 (config.epoch);
	writer.Bytes (config.root);
	writer.U16 (uint16_t (config.descriptor.size ()));
	writer.Bytes (config.descriptor);
	return writer.Data ();
}
bool DecodeConfig (std::span<const uint8_t> bytes, Config &config)
{
	Reader	 reader (bytes);
	Config	 candidate;
	uint8_t	 generator;
	uint16_t chunkSize, voxelSize, descriptorSize;
	if (!ReadHeader (reader, Packet::Config) || !reader.U64 (candidate.seed) || !reader.U8 (generator) || generator > 2 || !reader.U16 (chunkSize) ||
		!reader.U16 (voxelSize) || chunkSize != ChunkSize || voxelSize != VoxelSize || !reader.U64 (candidate.epoch) || !candidate.epoch ||
		!reader.Bytes (candidate.root) || !reader.U16 (descriptorSize) || descriptorSize > 4096 || reader.Remaining () != descriptorSize)
		return false;
	candidate.descriptor.resize (descriptorSize);
	if (!reader.Bytes (candidate.descriptor))
		return false;
	candidate.generator = static_cast<Generator> (generator);
	config = candidate;
	return true;
}
std::vector<uint8_t> EncodePosition (const WorldPosition &position)
{
	Writer writer;
	WriteHeader (writer, Packet::Position);
	return writer.Position (position) ? writer.Data () : std::vector<uint8_t>{};
}
bool DecodePosition (std::span<const uint8_t> bytes, WorldPosition &position)
{
	Reader		  reader (bytes);
	WorldPosition candidate;
	if (!ReadHeader (reader, Packet::Position) || !reader.Position (candidate) || reader.Remaining ())
		return false;
	position = candidate;
	return true;
}
std::vector<uint8_t> EncodeEdit (const Edit &edit)
{
	if (!ValidEdit (edit))
		return {};
	Writer writer;
	WriteHeader (writer, Packet::Edit);
	writer.U64 (edit.requestId);
	writer.U64 (edit.expectedRevision);
	writer.U64 (edit.newRevision);
	writer.U64 (edit.epoch);
	writer.Position (edit.position);
	writer.U16 (edit.cell.material);
	writer.U16 (std::bit_cast<uint16_t> (edit.cell.contents));
	return writer.Data ();
}
bool DecodeEdit (std::span<const uint8_t> bytes, Edit &edit)
{
	Reader	 reader (bytes);
	Edit	 candidate;
	uint16_t contents;
	if (!ReadHeader (reader, Packet::Edit) || !reader.U64 (candidate.requestId) || !reader.U64 (candidate.expectedRevision) ||
		!reader.U64 (candidate.newRevision) || !reader.U64 (candidate.epoch) || !reader.Position (candidate.position) ||
		!reader.U16 (candidate.cell.material) || !reader.U16 (contents) || reader.Remaining ())
		return false;
	candidate.cell.contents = std::bit_cast<int16_t> (contents);
	if (!ValidEdit (candidate))
		return false;
	edit = candidate;
	return true;
}
std::vector<uint8_t> EncodeFragment (const ChunkFragment &fragment)
{
	if (!ValidFragment (fragment))
		return {};
	Writer writer;
	WriteHeader (writer, Packet::Fragment);
	WriteKey (writer, fragment.key);
	writer.U64 (fragment.revision);
	writer.U64 (fragment.transferId);
	writer.U32 (fragment.totalBytes);
	writer.U32 (fragment.offset);
	writer.U16 (static_cast<uint16_t> (fragment.payload.size ()));
	writer.Bytes (fragment.payload);
	return writer.Data ();
}
bool DecodeFragment (std::span<const uint8_t> bytes, ChunkFragment &fragment)
{
	Reader		  reader (bytes);
	ChunkFragment candidate;
	uint16_t	  payloadSize;
	if (!ReadHeader (reader, Packet::Fragment) || !ReadKey (reader, candidate.key) || !reader.U64 (candidate.revision) || !reader.U64 (candidate.transferId) ||
		!reader.U32 (candidate.totalBytes) || !reader.U32 (candidate.offset) || !reader.U16 (payloadSize) || payloadSize > FragmentBytes ||
		payloadSize != reader.Remaining ())
		return false;
	candidate.payload.resize (payloadSize);
	if (!reader.Bytes (candidate.payload) || !ValidFragment (candidate))
		return false;
	fragment = std::move (candidate);
	return true;
}

namespace
{
constexpr size_t MaxProofNodes = 65;
bool			 WriteObject (Writer &writer, const ContentObject &object)
{
	return (object.type == ContentObjectType::SpatialNode || object.type == ContentObjectType::TerrainOverride ||
			object.type == ContentObjectType::SolidAsset) &&
		   object.bytes.size () <= MaxChunkBytes && writer.U8 (uint8_t (object.type)) && writer.Bytes (object.hash) &&
		   writer.U32 (uint32_t (object.bytes.size ())) && writer.Bytes (object.bytes);
}
bool ReadObject (Reader &reader, ContentObject &object)
{
	uint8_t	 type;
	uint32_t size;
	if (!reader.U8 (type) || type < 1 || type > 3 || !reader.Bytes (object.hash) || !reader.U32 (size) || size > MaxChunkBytes || size > reader.Remaining ())
		return false;
	object.type = ContentObjectType (type);
	object.bytes.resize (size);
	return reader.Bytes (object.bytes);
}
} // namespace
std::vector<uint8_t> EncodeRoot (const RootState &state)
{
	Writer writer;
	if (!state.epoch || !WriteHeader (writer, Packet::Root) || !writer.U64 (state.epoch) || !writer.Bytes (state.root))
		return {};
	return writer.Data ();
}
bool DecodeRoot (std::span<const uint8_t> bytes, RootState &state)
{
	Reader	  reader (bytes);
	RootState candidate;
	if (!ReadHeader (reader, Packet::Root) || !reader.U64 (candidate.epoch) || !candidate.epoch || !reader.Bytes (candidate.root) || reader.Remaining ())
		return false;
	state = candidate;
	return true;
}
std::vector<uint8_t> EncodeContentObject (const ContentObject &object)
{
	Writer writer (MaxChunkBytes);
	if (!WriteHeader (writer, Packet::Object) || !WriteObject (writer, object))
		return {};
	return writer.Data ();
}
bool DecodeContentObject (std::span<const uint8_t> bytes, ContentObject &object)
{
	Reader		  reader (bytes);
	ContentObject candidate;
	if (!ReadHeader (reader, Packet::Object) || !ReadObject (reader, candidate) || reader.Remaining ())
		return false;
	if (candidate.bytes.empty () || HashContent (candidate.type, candidate.bytes) != candidate.hash)
		return false;
	object = std::move (candidate);
	return true;
}
std::vector<uint8_t> EncodeTerrainProof (const TerrainProof &proof)
{
	Writer writer (MaxChunkBytes);
	if (proof.nodes.size () > MaxProofNodes || !WriteHeader (writer, Packet::Proof) || !writer.Bytes (proof.root) || !WriteKey (writer, proof.key) ||
		!writer.U64 (proof.revision) || !writer.U8 (uint8_t (proof.nodes.size ())))
		return {};
	for (const auto &node : proof.nodes)
		if (node.type != ContentObjectType::SpatialNode || !WriteObject (writer, node))
			return {};
	if (!writer.U8 (proof.overrideObject ? 1 : 0) ||
		(proof.overrideObject && (proof.overrideObject->type != ContentObjectType::TerrainOverride || !WriteObject (writer, *proof.overrideObject))))
		return {};
	if (proof.assets.size () > 256 || !writer.U16 (uint16_t (proof.assets.size ())))
		return {};
	for (const auto &asset : proof.assets)
		if (asset.type != ContentObjectType::SolidAsset || !WriteObject (writer, asset))
			return {};
	return writer.Data ();
}
bool DecodeTerrainProof (std::span<const uint8_t> bytes, TerrainProof &proof)
{
	if (bytes.size () > MaxChunkBytes)
		return false;
	Reader		 reader (bytes);
	TerrainProof candidate;
	uint8_t		 count, present;
	if (!ReadHeader (reader, Packet::Proof) || !reader.Bytes (candidate.root) || !ReadKey (reader, candidate.key) || !reader.U64 (candidate.revision) ||
		!reader.U8 (count) || count > MaxProofNodes)
		return false;
	candidate.nodes.resize (count);
	for (auto &node : candidate.nodes)
		if (!ReadObject (reader, node) || node.type != ContentObjectType::SpatialNode || node.bytes.empty () || node.bytes.size () > 4096 ||
			HashContent (node.type, node.bytes) != node.hash)
			return false;
	if (!reader.U8 (present) || present > 1)
		return false;
	if (present)
	{
		candidate.overrideObject.emplace ();
		auto &object = *candidate.overrideObject;
		if (!ReadObject (reader, object) || object.type != ContentObjectType::TerrainOverride ||
			(!object.bytes.empty () && HashContent (object.type, object.bytes) != object.hash))
			return false;
	}
	uint16_t assetCount;
	if (!reader.U16 (assetCount) || assetCount > 256)
		return false;
	candidate.assets.resize (assetCount);
	for (auto &asset : candidate.assets)
		if (!ReadObject (reader, asset) || asset.type != ContentObjectType::SolidAsset || asset.bytes.empty () ||
			HashContent (asset.type, asset.bytes) != asset.hash)
			return false;
	if (reader.Remaining ())
		return false;
	proof = std::move (candidate);
	return true;
}

namespace
{
bool ValidSignal (const SignalFragment &fragment)
{
	return fragment.epoch && fragment.peerId && fragment.messageId && fragment.totalBytes && fragment.totalBytes <= MaxSignalBytes &&
		   fragment.offset < fragment.totalBytes && fragment.offset % SignalFragmentBytes == 0 &&
		   fragment.payload.size () == std::min<size_t> (SignalFragmentBytes, fragment.totalBytes - fragment.offset);
}
bool ValidPeerDirectory (const PeerDirectory &directory)
{
	if (!directory.epoch || !directory.recipientId || directory.sessionId == ContentHash{} || directory.token == ContentHash{} ||
		directory.stunServers.size () > 255 || directory.peers.size () > MaxSessionPeers)
		return false;
	for (unsigned char c : directory.stunServers)
		if (c < 32 || c > 126)
			return false;
	std::set<uint64_t> ids;
	for (const auto &peer : directory.peers)
	{
		if (!peer.peerId || peer.peerId == directory.recipientId || !ids.insert (peer.peerId).second || peer.address.size () > 15 ||
			(peer.address.empty () != (peer.port == 0)) || (!peer.port && !peer.nat))
			return false;
		for (char c : peer.address)
			if ((c < '0' || c > '9') && c != '.')
				return false;
	}
	return true;
}
} // namespace
std::vector<uint8_t> EncodePeerDirectory (const PeerDirectory &directory)
{
	if (!ValidPeerDirectory (directory))
		return {};
	Writer writer;
	WriteHeader (writer, Packet::Peers);
	writer.U64 (directory.epoch);
	writer.U64 (directory.recipientId);
	writer.Bytes (directory.sessionId);
	writer.Bytes (directory.token);
	writer.U16 (uint16_t (directory.stunServers.size ()));
	writer.Bytes ({reinterpret_cast<const uint8_t *> (directory.stunServers.data ()), directory.stunServers.size ()});
	writer.U16 (uint16_t (directory.peers.size ()));
	for (const auto &peer : directory.peers)
	{
		writer.U64 (peer.peerId);
		writer.U8 (peer.nat);
		writer.U16 (peer.port);
		writer.U8 (uint8_t (peer.address.size ()));
		writer.Bytes ({reinterpret_cast<const uint8_t *> (peer.address.data ()), peer.address.size ()});
	}
	return writer.Data ();
}
bool DecodePeerDirectory (std::span<const uint8_t> bytes, PeerDirectory &directory)
{
	if (bytes.size () > 4096)
		return false;
	Reader		  reader (bytes);
	PeerDirectory candidate;
	uint16_t	  length, count;
	if (!ReadHeader (reader, Packet::Peers) || !reader.U64 (candidate.epoch) || !reader.U64 (candidate.recipientId) || !reader.Bytes (candidate.sessionId) ||
		!reader.Bytes (candidate.token) || !reader.U16 (length) || length > 255 || length > reader.Remaining ())
		return false;
	std::vector<uint8_t> text (length);
	if (!reader.Bytes (text) || !reader.U16 (count) || count > MaxSessionPeers)
		return false;
	candidate.stunServers.assign (text.begin (), text.end ());
	candidate.peers.resize (count);
	for (auto &peer : candidate.peers)
	{
		uint8_t nat, size;
		if (!reader.U64 (peer.peerId) || !reader.U8 (nat) || nat > 1 || !reader.U16 (peer.port) || !reader.U8 (size) || size > 15 || size > reader.Remaining ())
			return false;
		text.resize (size);
		if (!reader.Bytes (text))
			return false;
		peer.nat = nat != 0;
		peer.address.assign (text.begin (), text.end ());
	}
	if (reader.Remaining () || !ValidPeerDirectory (candidate))
		return false;
	directory = std::move (candidate);
	return true;
}
std::vector<uint8_t> EncodeSignalFragment (const SignalFragment &fragment)
{
	if (!ValidSignal (fragment))
		return {};
	Writer writer;
	WriteHeader (writer, Packet::Signal);
	writer.U64 (fragment.epoch);
	writer.U64 (fragment.peerId);
	writer.U64 (fragment.messageId);
	writer.U16 (fragment.totalBytes);
	writer.U16 (fragment.offset);
	writer.U16 (uint16_t (fragment.payload.size ()));
	writer.Bytes (fragment.payload);
	return writer.Data ();
}
bool DecodeSignalFragment (std::span<const uint8_t> bytes, SignalFragment &fragment)
{
	Reader		   reader (bytes);
	SignalFragment candidate;
	uint16_t	   size;
	if (!ReadHeader (reader, Packet::Signal) || !reader.U64 (candidate.epoch) || !reader.U64 (candidate.peerId) || !reader.U64 (candidate.messageId) ||
		!reader.U16 (candidate.totalBytes) || !reader.U16 (candidate.offset) || !reader.U16 (size) || size > SignalFragmentBytes || size != reader.Remaining ())
		return false;
	candidate.payload.resize (size);
	if (!reader.Bytes (candidate.payload) || !ValidSignal (candidate))
		return false;
	fragment = std::move (candidate);
	return true;
}
struct SignalReassembler::Impl
{
	struct Partial
	{
		uint64_t			 epoch, lastProgress;
		std::vector<uint8_t> bytes;
		std::vector<bool>	 received;
		size_t				 count = 0;
	};
	using Key = std::pair<uint64_t, uint64_t>;
	std::map<Key, Partial>				 pending;
	std::deque<std::pair<Key, uint64_t>> finished;
	size_t								 buffered = 0;
};
SignalReassembler::SignalReassembler () : impl (std::make_unique<Impl> ()) {}
SignalReassembler::~SignalReassembler () = default;
size_t SignalReassembler::BufferedBytes () const
{
	return impl->buffered;
}
void SignalReassembler::Reset ()
{
	impl->pending.clear ();
	impl->finished.clear ();
	impl->buffered = 0;
}
void SignalReassembler::Expire (uint64_t now)
{
	for (auto it = impl->pending.begin (); it != impl->pending.end ();)
		if (now < it->second.lastProgress || now - it->second.lastProgress >= 5000)
		{
			impl->buffered -= it->second.bytes.size ();
			it = impl->pending.erase (it);
		}
		else
			++it;
}
ReceiveResult SignalReassembler::Receive (const SignalFragment &fragment, uint64_t now, CompletedSignal &completed)
{
	Expire (now);
	if (!ValidSignal (fragment))
		return ReceiveResult::Rejected;
	const Impl::Key key{fragment.peerId, fragment.messageId};
	for (const auto &finished : impl->finished)
		if (finished.first == key)
			return finished.second == fragment.epoch ? ReceiveResult::Duplicate : ReceiveResult::Rejected;
	if (!impl->pending.contains (key))
	{
		if (impl->pending.size () >= 4 || impl->buffered + fragment.totalBytes > 32768)
			return ReceiveResult::BudgetExceeded;
		Impl::Partial partial{
			fragment.epoch, now, std::vector<uint8_t> (fragment.totalBytes),
			std::vector<bool> ((fragment.totalBytes + SignalFragmentBytes - 1) / SignalFragmentBytes)};
		impl->pending.emplace (key, std::move (partial));
		impl->buffered += fragment.totalBytes;
	}
	auto  it = impl->pending.find (key);
	auto &partial = it->second;
	auto  reject = [&]
	{
		impl->buffered -= partial.bytes.size ();
		impl->pending.erase (it);
		return ReceiveResult::Rejected;
	};
	if (partial.epoch != fragment.epoch || partial.bytes.size () != fragment.totalBytes)
		return reject ();
	size_t slot = fragment.offset / SignalFragmentBytes;
	if (partial.received[slot])
		return std::equal (fragment.payload.begin (), fragment.payload.end (), partial.bytes.begin () + fragment.offset) ? ReceiveResult::Duplicate : reject ();
	std::copy (fragment.payload.begin (), fragment.payload.end (), partial.bytes.begin () + fragment.offset);
	partial.received[slot] = true;
	partial.lastProgress = now;
	if (++partial.count != partial.received.size ())
		return ReceiveResult::Pending;
	CompletedSignal result{fragment.epoch, fragment.peerId, fragment.messageId, std::move (partial.bytes)};
	impl->buffered -= result.bytes.size ();
	impl->pending.erase (it);
	impl->finished.emplace_back (key, fragment.epoch);
	if (impl->finished.size () > 64)
		impl->finished.pop_front ();
	completed = std::move (result);
	return ReceiveResult::Complete;
}

struct Reassembler::Impl
{
	struct Transfer
	{
		ChunkKey			 key;
		uint64_t			 revision, lastProgress;
		std::vector<uint8_t> bytes;
		std::vector<bool>	 received;
		size_t				 fragments = 0;
	};
	struct Finished
	{
		uint64_t id, revision;
		ChunkKey key;
		uint32_t size;
	};
	ReassemblyLimits					   limits;
	std::unordered_map<uint64_t, Transfer> transfers;
	std::deque<Finished>				   finished;
	size_t								   buffered = 0;
	explicit Impl (ReassemblyLimits limits) : limits (limits) {}
};

Reassembler::Reassembler (ReassemblyLimits limits) : impl (std::make_unique<Impl> (limits)) {}
Reassembler::~Reassembler () = default;
Reassembler::Reassembler (Reassembler &&) noexcept = default;
Reassembler &Reassembler::operator= (Reassembler &&) noexcept = default;
size_t		 Reassembler::BufferedBytes () const
{
	return impl->buffered;
}
size_t Reassembler::OutstandingTransfers () const
{
	return impl->transfers.size ();
}
void Reassembler::Reset ()
{
	impl->transfers.clear ();
	impl->finished.clear ();
	impl->buffered = 0;
}
void Reassembler::Expire (uint64_t now)
{
	for (auto it = impl->transfers.begin (); it != impl->transfers.end ();)
	{
		// Clock rollback expires old transfers, rather than pinning their allocations.
		if (now < it->second.lastProgress || now - it->second.lastProgress >= impl->limits.timeoutMilliseconds)
		{
			impl->buffered -= it->second.bytes.size ();
			it = impl->transfers.erase (it);
		}
		else
			++it;
	}
}
ReceiveResult Reassembler::Receive (const ChunkFragment &fragment, uint64_t now, CompletedChunk &completed)
{
	Expire (now);
	if (!ValidFragment (fragment) || fragment.totalBytes > impl->limits.maxChunkBytes)
		return ReceiveResult::Rejected;
	for (const auto &finished : impl->finished)
		if (finished.id == fragment.transferId)
			return finished.key == fragment.key && finished.revision == fragment.revision && finished.size == fragment.totalBytes ? ReceiveResult::Duplicate
																																  : ReceiveResult::Rejected;
	if (!impl->transfers.contains (fragment.transferId))
	{
		if (impl->transfers.size () >= impl->limits.maxTransfers || fragment.totalBytes > impl->limits.maxBufferedBytes - impl->buffered)
			return ReceiveResult::BudgetExceeded;
		Impl::Transfer transfer;
		transfer.key = fragment.key;
		transfer.revision = fragment.revision;
		transfer.lastProgress = now;
		transfer.bytes.resize (fragment.totalBytes);
		transfer.received.resize ((fragment.totalBytes + FragmentBytes - 1) / FragmentBytes);
		impl->transfers.emplace (fragment.transferId, std::move (transfer));
		impl->buffered += fragment.totalBytes;
	}
	auto  it = impl->transfers.find (fragment.transferId);
	auto &transfer = it->second;
	auto  reject = [&] ()
	{
		impl->buffered -= transfer.bytes.size ();
		impl->transfers.erase (it);
		return ReceiveResult::Rejected;
	};
	if (!(transfer.key == fragment.key) || transfer.revision != fragment.revision || transfer.bytes.size () != fragment.totalBytes)
		return reject ();
	const size_t slot = fragment.offset / FragmentBytes;
	if (transfer.received[slot])
		return std::equal (fragment.payload.begin (), fragment.payload.end (), transfer.bytes.begin () + fragment.offset) ? ReceiveResult::Duplicate
																														  : reject ();
	std::copy (fragment.payload.begin (), fragment.payload.end (), transfer.bytes.begin () + fragment.offset);
	transfer.received[slot] = true;
	transfer.lastProgress = now;
	if (++transfer.fragments != transfer.received.size ())
		return ReceiveResult::Pending;
	CompletedChunk result{fragment.key, fragment.revision, fragment.transferId, std::move (transfer.bytes)};
	impl->buffered -= result.bytes.size ();
	impl->transfers.erase (it);
	impl->finished.push_back ({fragment.transferId, fragment.revision, fragment.key, fragment.totalBytes});
	if (impl->finished.size () > 32)
		impl->finished.pop_front ();
	completed = std::move (result);
	return ReceiveResult::Complete;
}
} // namespace quiver::wire
