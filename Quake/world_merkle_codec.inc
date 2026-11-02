/* SPDX-License-Identifier: GPL-2.0-or-later */
// Included by octree_world.cpp inside namespace quiver.
ContentHash HashContent (ContentObjectType type, const std::vector<uint8_t> &bytes)
{
	std::vector<uint8_t> canonical{'Q', 'W', 'O', 1, uint8_t (type)};
	canonical.insert (canonical.end (), bytes.begin (), bytes.end ());
	return WorldSHA256 (canonical);
}
namespace
{
void Put64 (std::vector<uint8_t> &bytes, uint64_t value)
{
	for (int i = 0; i < 8; ++i)
		bytes.push_back (uint8_t (value >> (i * 8)));
}
uint64_t Get64 (const std::vector<uint8_t> &bytes, size_t offset)
{
	if (offset > bytes.size () || bytes.size () - offset < 8)
		throw std::runtime_error ("truncated Merkle object");
	uint64_t value = 0;
	for (int i = 0; i < 8; ++i)
		value |= uint64_t (bytes[offset + i]) << (i * 8);
	return value;
}
struct SpatialNode
{
	ChunkKey				   key;
	uint8_t					   level = 64; // 64 is a chunk leaf; other levels branch on MSB-first octants.
	ContentHash				   value{};
	std::array<ContentHash, 8> children{};
};
int Octant (ChunkKey key, int level)
{
	int result = 0;
	for (int axis = 0; axis < 3; ++axis)
		result |= int ((uint64_t (key[axis]) >> (63 - level)) & 1) << axis;
	return result;
}
int CommonLevels (ChunkKey a, ChunkKey b)
{
	uint64_t different = uint64_t (a.x) ^ uint64_t (b.x);
	different |= uint64_t (a.y) ^ uint64_t (b.y);
	different |= uint64_t (a.z) ^ uint64_t (b.z);
	return std::countl_zero (different);
}
SpatialNode DecodeSpatial (const ContentObject &object)
{
	if (object.type != ContentObjectType::SpatialNode || object.bytes.size () < 26 || object.bytes[0] != 1 || object.bytes[1] > 64 ||
		HashContent (object.type, object.bytes) != object.hash)
		throw std::runtime_error ("invalid spatial object");
	SpatialNode n;
	n.level = object.bytes[1];
	for (int axis = 0; axis < 3; ++axis)
		n.key[axis] = std::bit_cast<int64_t> (Get64 (object.bytes, 2 + axis * 8));
	if (n.level == 64)
	{
		if (object.bytes.size () != 58)
			throw std::runtime_error ("invalid spatial leaf size");
		std::copy_n (object.bytes.begin () + 26, 32, n.value.begin ());
		if (n.value == ContentHash{})
			throw std::runtime_error ("empty spatial leaf");
	}
	else
	{
		if (object.bytes.size () != 282)
			throw std::runtime_error ("invalid spatial branch size");
		int count = 0;
		for (int i = 0; i < 8; ++i)
		{
			std::copy_n (object.bytes.begin () + 26 + i * 32, 32, n.children[i].begin ());
			count += n.children[i] != ContentHash{};
		}
		if (count < 2)
			throw std::runtime_error ("uncompressed spatial branch");
		for (int axis = 0; axis < 3; ++axis)
			if ((uint64_t (n.key[axis]) & (n.level == 0 ? UINT64_MAX : (UINT64_MAX >> n.level))) != 0)
				throw std::runtime_error ("noncanonical spatial prefix");
	}
	return n;
}
std::vector<uint8_t> EncodeSpatial (SpatialNode node)
{
	std::vector<uint8_t> bytes{1, node.level};
	if (node.level < 64)
		for (int axis = 0; axis < 3; ++axis)
			node.key[axis] = std::bit_cast<int64_t> (uint64_t (node.key[axis]) & (node.level ? UINT64_MAX << (64 - node.level) : 0));
	for (int axis = 0; axis < 3; ++axis)
		Put64 (bytes, uint64_t (node.key[axis]));
	if (node.level == 64)
		bytes.insert (bytes.end (), node.value.begin (), node.value.end ());
	else
		for (auto &child : node.children)
			bytes.insert (bytes.end (), child.begin (), child.end ());
	return bytes;
}
std::vector<uint8_t> EncodeOverride (const Node &actual, const Node &base)
{
	if (!actual.children && !base.children)
	{
		if (actual.cell == base.cell)
			return {0};
		return {
			1, uint8_t (actual.cell.material), uint8_t (actual.cell.material >> 8), uint8_t (actual.cell.contents),
			uint8_t (uint16_t (actual.cell.contents) >> 8)};
	}
	std::array<std::vector<uint8_t>, 8> children;
	for (int i = 0; i < 8; ++i)
		children[i] = EncodeOverride (actual.children ? (*actual.children)[i] : actual, base.children ? (*base.children)[i] : base);
	bool same = children[0][0] != 2;
	for (int i = 1; i < 8; ++i)
		same &= children[i] == children[0];
	if (same)
		return children[0];
	std::vector<uint8_t> bytes{2};
	for (const auto &child : children)
		bytes.insert (bytes.end (), child.begin (), child.end ());
	return bytes;
}
void ApplyOverride (Node *node, const std::vector<uint8_t> &bytes, size_t &cursor, int depth)
{
	if (cursor >= bytes.size ())
		throw std::runtime_error ("truncated terrain override");
	uint8_t tag = bytes[cursor++];
	if (tag == 0)
		return;
	if (tag == 1)
	{
		if (bytes.size () - cursor < 4)
			throw std::runtime_error ("truncated override cell");
		Cell cell{uint16_t (bytes[cursor] | uint16_t (bytes[cursor + 1]) << 8), int16_t (bytes[cursor + 2] | uint16_t (bytes[cursor + 3]) << 8)};
		cursor += 4;
		if (!ValidCell (cell))
			throw std::runtime_error ("invalid override cell");
		if (node)
			node->Fill (cell);
		return;
	}
	if (tag != 2 || depth >= 7)
		throw std::runtime_error ("invalid override octree depth");
	if (node && !node->children)
		node->Split ();
	for (int i = 0; i < 8; ++i)
		ApplyOverride (node ? &(*node->children)[i] : nullptr, bytes, cursor, depth + 1);
	if (node)
		node->Collapse ();
}
struct SolidReference
{
	ContentHash	   hash{};
	WorldPosition  origin;
	SolidOperation operation;
};
std::vector<SolidReference> DecodeSolidReferences (const std::vector<uint8_t> &bytes, size_t &cursor)
{
	uint64_t count = Get64 (bytes, cursor);
	cursor += 8;
	if (!count || count > 256 || count > (bytes.size () - cursor) / 69)
		throw std::runtime_error ("invalid solid reference count");
	std::vector<SolidReference> refs;
	for (uint64_t i = 0; i < count; ++i)
	{
		SolidReference ref;
		std::copy_n (bytes.begin () + cursor, 32, ref.hash.begin ());
		cursor += 32;
		ref.operation = SolidOperation (bytes[cursor++]);
		if (ref.operation != SolidOperation::Add && ref.operation != SolidOperation::Subtract)
			throw std::runtime_error ("invalid solid operation");
		for (int axis = 0; axis < 3; ++axis)
		{
			ref.origin.chunk[axis] = std::bit_cast<int64_t> (Get64 (bytes, cursor));
			cursor += 8;
		}
		for (float &f : ref.origin.local)
		{
			uint32_t value = 0;
			for (int j = 0; j < 4; ++j)
				value |= uint32_t (bytes[cursor++]) << (j * 8);
			if (value >= 1024 * 1024)
				throw std::runtime_error ("unnormalized solid origin");
			f = float (value) / 1024;
		}
		refs.push_back (ref);
	}
	return refs;
}
std::shared_ptr<const std::vector<SolidDefinition>>
DecodeOverrideSolids (const std::vector<uint8_t> &bytes, size_t &cursor, const std::vector<ContentObject> &assets = {})
{
	if (bytes[0] == 1)
		return {};
	auto solids = std::make_shared<std::vector<SolidDefinition>> ();
	if (bytes[0] == 3)
	{
		auto										 refs = DecodeSolidReferences (bytes, cursor);
		std::map<ContentHash, const ContentObject *> byHash;
		for (const auto &asset : assets)
		{
			if (asset.type != ContentObjectType::SolidAsset || HashContent (asset.type, asset.bytes) != asset.hash ||
				!byHash.emplace (asset.hash, &asset).second)
				throw std::runtime_error ("invalid or duplicate solid asset");
		}
		std::set<ContentHash> used;
		for (const auto &ref : refs)
		{
			auto found = byHash.find (ref.hash);
			if (found == byHash.end ())
				throw std::runtime_error ("missing solid asset");
			SolidDefinition solid;
			std::string		error;
			if (!DecodeSolid (found->second->bytes, solid, &error))
				throw std::runtime_error (error);
			if (solid.origin.chunk != ChunkKey{} || solid.origin.local != std::array<float, 3>{} || solid.operation != SolidOperation::Add)
				throw std::runtime_error ("noncanonical solid geometry asset");
			solid.origin = ref.origin;
			solid.operation = ref.operation;
			solids->push_back (std::move (solid));
			used.insert (ref.hash);
		}
		if (used.size () != assets.size ())
			throw std::runtime_error ("unreferenced solid asset");
		return solids;
	}
	const uint64_t count = Get64 (bytes, cursor);
	cursor += 8;
	if (!count || count > 256)
		throw std::runtime_error ("invalid solid operation count");
	for (uint64_t i = 0; i < count; ++i)
	{
		const uint64_t length = Get64 (bytes, cursor);
		cursor += 8;
		if (length > bytes.size () - cursor)
			throw std::runtime_error ("truncated solid operation");
		std::vector<uint8_t> encoded (bytes.begin () + cursor, bytes.begin () + cursor + size_t (length));
		SolidDefinition		 solid;
		std::string			 error;
		if (!DecodeSolid (encoded, solid, &error))
			throw std::runtime_error (error);
		solids->push_back (std::move (solid));
		cursor += size_t (length);
	}
	return solids;
}
void ValidateOverride (const ContentObject &object, const std::vector<ContentObject> *assets = nullptr)
{
	if (object.type != ContentObjectType::TerrainOverride || object.bytes.empty () || object.bytes.size () > MaxEncodedBytes || object.bytes[0] < 1 ||
		object.bytes[0] > 3 || HashContent (object.type, object.bytes) != object.hash)
		throw std::runtime_error ("invalid override object");
	size_t cursor = 1;
	ApplyOverride (nullptr, object.bytes, cursor, 0);
	if (object.bytes[0] == 3 && !assets)
		DecodeSolidReferences (object.bytes, cursor);
	else
		DecodeOverrideSolids (object.bytes, cursor, assets ? *assets : std::vector<ContentObject>{});
	if (cursor != object.bytes.size ())
		throw std::runtime_error ("trailing override bytes");
	if (assets && object.bytes[0] != 3 && !assets->empty ())
		throw std::runtime_error ("unexpected solid assets");
}
} // namespace
bool ValidateTerrainProof (const TerrainProof &proof, std::string *error)
{
	try
	{
		if (proof.nodes.size () > 65)
			throw std::runtime_error ("terrain proof too deep");
		ContentHash expected = proof.root, value{};
		int			parent = -1;
		for (size_t i = 0; i < proof.nodes.size (); ++i)
		{
			if (expected == ContentHash{} || proof.nodes[i].hash != expected)
				throw std::runtime_error ("unanchored terrain proof");
			auto node = DecodeSpatial (proof.nodes[i]);
			if (node.level <= parent)
				throw std::runtime_error ("unordered terrain proof");
			if (parent >= 0 && CommonLevels (node.key, proof.key) <= parent)
				throw std::runtime_error ("misplaced terrain proof node");
			expected = {};
			if (CommonLevels (node.key, proof.key) < node.level)
			{
				if (i + 1 != proof.nodes.size ())
					throw std::runtime_error ("extra absence nodes");
				break;
			}
			if (node.level == 64)
			{
				value = node.value;
				if (i + 1 != proof.nodes.size ())
					throw std::runtime_error ("extra leaf nodes");
				break;
			}
			parent = node.level;
			expected = node.children[Octant (proof.key, node.level)];
		}
		if (expected != ContentHash{})
			throw std::runtime_error ("incomplete terrain proof");
		if (value == ContentHash{})
		{
			if (proof.overrideObject || !proof.assets.empty ())
				throw std::runtime_error ("unexpected override");
		}
		else
		{
			if (!proof.overrideObject || proof.overrideObject->hash != value)
				throw std::runtime_error ("missing terrain override");
			ValidateOverride (*proof.overrideObject, &proof.assets);
		}
		return true;
	}
	catch (const std::exception &e)
	{
		if (error)
			*error = e.what ();
		return false;
	}
}
