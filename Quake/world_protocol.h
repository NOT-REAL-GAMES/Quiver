#ifndef QUIVER_WORLD_PROTOCOL_H
#define QUIVER_WORLD_PROTOCOL_H

#include "octree_world.h"
#include <span>

namespace quiver::wire
{
constexpr uint32_t ProtocolMarker = 0x51565231; // QVR1, a separate Quiver protocol modifier.
constexpr uint16_t ProtocolVersion = 5;			// Planet descriptors and content-addressed solid geometry in terrain proofs.
constexpr size_t   FragmentBytes = 1024;
constexpr size_t   MaxChunkBytes = 16 * 1024 * 1024;

// Fixed little-endian primitives. Never use the legacy MSG variable-length int64 codec.
class Writer
{
  public:
	explicit Writer (size_t limit = 65536) : limit (limit) {}
	bool						U8 (uint8_t value);
	bool						U16 (uint16_t value);
	bool						U32 (uint32_t value);
	bool						U64 (uint64_t value);
	bool						I64 (int64_t value);
	bool						F32 (float value);
	bool						Bytes (std::span<const uint8_t> value);
	bool						Position (const WorldPosition &position);
	const std::vector<uint8_t> &Data () const
	{
		return data;
	}

  private:
	size_t				 limit;
	std::vector<uint8_t> data;
};

class Reader
{
  public:
	explicit Reader (std::span<const uint8_t> data) : data (data) {}
	bool   U8 (uint8_t &value);
	bool   U16 (uint16_t &value);
	bool   U32 (uint32_t &value);
	bool   U64 (uint64_t &value);
	bool   I64 (int64_t &value);
	bool   F32 (float &value);
	bool   Bytes (std::span<uint8_t> value);
	bool   Position (WorldPosition &position);
	size_t Remaining () const
	{
		return data.size () - offset;
	}

  private:
	std::span<const uint8_t> data;
	size_t					 offset = 0;
};

struct Config
{
	uint64_t			 seed = 1;
	Generator			 generator = Generator::Terrain;
	std::vector<uint8_t> descriptor;
	ContentHash			 root{};
	uint64_t			 epoch = 1;
};

struct Edit
{
	uint64_t	  requestId = 0;
	uint64_t	  expectedRevision = 0;
	// Zero in requests; commits must be exactly expectedRevision + 1.
	uint64_t	  newRevision = 0;
	WorldPosition position;
	Cell		  cell;
	uint64_t	  epoch = 1;
};

struct ChunkFragment
{
	ChunkKey			 key;
	uint64_t			 revision = 0;
	uint64_t			 transferId = 0;
	uint32_t			 totalBytes = 0;
	uint32_t			 offset = 0;
	std::vector<uint8_t> payload;
};

// Decoders reject unknown versions/types, malformed input and trailing bytes.
// Failure leaves the output unchanged. Encoders return empty on invalid input.
std::vector<uint8_t> EncodeConfig (const Config &config);
bool				 DecodeConfig (std::span<const uint8_t> bytes, Config &config);
std::vector<uint8_t> EncodePosition (const WorldPosition &position);
bool				 DecodePosition (std::span<const uint8_t> bytes, WorldPosition &position);
std::vector<uint8_t> EncodeEdit (const Edit &edit);
bool				 DecodeEdit (std::span<const uint8_t> bytes, Edit &edit);
std::vector<uint8_t> EncodeFragment (const ChunkFragment &fragment);
bool				 DecodeFragment (std::span<const uint8_t> bytes, ChunkFragment &fragment);

// Proof decoding is structural; callers anchor root/epoch and verify every object
// before installing. A zero-byte override is an offer to fetch its named object.
struct RootState
{
	ContentHash root{};
	uint64_t	epoch = 1;
};
std::vector<uint8_t> EncodeRoot (const RootState &state);
bool				 DecodeRoot (std::span<const uint8_t> bytes, RootState &state);
std::vector<uint8_t> EncodeTerrainProof (const TerrainProof &proof);
bool				 DecodeTerrainProof (std::span<const uint8_t> bytes, TerrainProof &proof);
std::vector<uint8_t> EncodeContentObject (const ContentObject &object);
bool				 DecodeContentObject (std::span<const uint8_t> bytes, ContentObject &object);

constexpr size_t SignalFragmentBytes = 512;
constexpr size_t MaxSignalBytes = 8192;
constexpr size_t MaxSessionPeers = 128;
struct PeerAddress
{
	uint64_t	peerId = 0;
	bool		nat = false;
	uint16_t	port = 0;
	std::string address;
};
struct PeerDirectory
{
	uint64_t				 epoch = 0, recipientId = 0;
	ContentHash				 sessionId{}, token{};
	std::string				 stunServers;
	std::vector<PeerAddress> peers;
};
struct SignalFragment
{
	uint64_t			 epoch = 0, peerId = 0, messageId = 0;
	uint16_t			 totalBytes = 0, offset = 0;
	std::vector<uint8_t> payload;
};
std::vector<uint8_t> EncodePeerDirectory (const PeerDirectory &directory);
bool				 DecodePeerDirectory (std::span<const uint8_t> bytes, PeerDirectory &directory);
std::vector<uint8_t> EncodeSignalFragment (const SignalFragment &fragment);
bool				 DecodeSignalFragment (std::span<const uint8_t> bytes, SignalFragment &fragment);

struct CompletedChunk
{
	ChunkKey			 key;
	uint64_t			 revision = 0;
	uint64_t			 transferId = 0;
	std::vector<uint8_t> bytes;
};

enum class ReceiveResult
{
	Pending,
	Complete,
	Duplicate,
	Rejected,
	BudgetExceeded
};

struct CompletedSignal
{
	uint64_t			 epoch = 0, peerId = 0, messageId = 0;
	std::vector<uint8_t> bytes;
};
// Four signals/32KiB maximum across all session peers; expires after five
// seconds without progress. Duplicate fragments never extend that deadline.
class SignalReassembler
{
  public:
	SignalReassembler ();
	~SignalReassembler ();
	ReceiveResult Receive (const SignalFragment &, uint64_t nowMilliseconds, CompletedSignal &);
	void		  Expire (uint64_t nowMilliseconds);
	void		  Reset ();
	size_t		  BufferedBytes () const;

  private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

struct ReassemblyLimits
{
	size_t	 maxTransfers = 4;
	size_t	 maxChunkBytes = MaxChunkBytes;
	size_t	 maxBufferedBytes = 32 * 1024 * 1024;
	uint64_t timeoutMilliseconds = 10000;
};

// One bounded receiver per connection. Transfer IDs must not be reused in a session.
// The caller must compare CompletedChunk::revision to the currently installed chunk
// before publishing, and validate the octree bytes with Octree::Decode.
class Reassembler
{
  public:
	explicit Reassembler (ReassemblyLimits limits = {});
	~Reassembler ();
	Reassembler (Reassembler &&) noexcept;
	Reassembler	 &operator= (Reassembler &&) noexcept;
	ReceiveResult Receive (const ChunkFragment &fragment, uint64_t nowMilliseconds, CompletedChunk &completed);
	void		  Expire (uint64_t nowMilliseconds);
	void		  Reset ();
	size_t		  BufferedBytes () const;
	size_t		  OutstandingTransfers () const;

  private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
} // namespace quiver::wire
#endif
