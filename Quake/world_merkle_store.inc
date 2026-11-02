/* SPDX-License-Identifier: GPL-2.0-or-later */
// World::Impl members; all database access runs on the owned worker.
void BindHash (Statement &s, int column, const ContentHash &hash)
{
	SqlCheck (sqlite3_bind_blob (s.statement, column, hash.data (), 32, SQLITE_TRANSIENT), db, "bind content hash");
}
ContentHash ColumnHash (sqlite3_stmt *s, int column)
{
	if (sqlite3_column_bytes (s, column) != 32 || sqlite3_column_type (s, column) != SQLITE_BLOB)
		throw std::runtime_error ("invalid stored hash");
	ContentHash result;
	std::copy_n (static_cast<const uint8_t *> (sqlite3_column_blob (s, column)), 32, result.begin ());
	return result;
}
ContentHash Root ()
{
	Statement s (db, "SELECT root FROM terrain_state WHERE id=1");
	if (sqlite3_step (s.statement) != SQLITE_ROW)
		throw std::runtime_error ("missing terrain root");
	return ColumnHash (s.statement, 0);
}
void RefreshContentHead ()
{
	auto	  root = Root ();
	Statement epoch (db, "SELECT epoch FROM terrain_state WHERE id=1");
	if (sqlite3_step (epoch.statement) != SQLITE_ROW)
		throw std::runtime_error ("missing authority epoch");
	std::lock_guard lock (contentMutex);
	cachedRoot = root;
	cachedEpoch = uint64_t (sqlite3_column_int64 (epoch.statement, 0));
}
ContentObject Object (ContentHash hash)
{
	Statement s (db, "SELECT type,data FROM world_objects WHERE hash=?");
	BindHash (s, 1, hash);
	if (sqlite3_step (s.statement) != SQLITE_ROW)
		throw std::runtime_error ("missing content object");
	int size = sqlite3_column_bytes (s.statement, 1), type = sqlite3_column_int (s.statement, 0);
	if (size <= 0 || size > int (MaxEncodedBytes) || type < 1 || type > 3)
		throw std::runtime_error ("invalid content object size/type");
	const auto	 *p = static_cast<const uint8_t *> (sqlite3_column_blob (s.statement, 1));
	ContentObject object{hash, ContentObjectType (type), {p, p + size}};
	if (HashContent (object.type, object.bytes) != hash)
		throw std::runtime_error ("corrupt content object hash");
	return object;
}
ContentHash PutObject (ContentObjectType type, const std::vector<uint8_t> &bytes)
{
	if (bytes.empty () || bytes.size () > MaxEncodedBytes)
		throw std::runtime_error ("invalid content object size");
	auto	  hash = HashContent (type, bytes);
	Statement s (db, "INSERT OR IGNORE INTO world_objects(hash,type,data) VALUES(?,?,?)");
	BindHash (s, 1, hash);
	SqlCheck (sqlite3_bind_int (s.statement, 2, int (type)), db, "bind object type");
	SqlCheck (sqlite3_bind_blob (s.statement, 3, bytes.data (), int (bytes.size ()), SQLITE_TRANSIENT), db, "bind object bytes");
	SqlCheck (sqlite3_step (s.statement), db, "store immutable object");
	return hash;
}
ContentHash PutSpatial (const SpatialNode &node)
{
	return PutObject (ContentObjectType::SpatialNode, EncodeSpatial (node));
}
ContentHash UpdateTree (ContentHash root, ChunkKey key, ContentHash value)
{
	if (root == ContentHash{})
	{
		if (value == ContentHash{})
			return {};
		SpatialNode leaf;
		leaf.key = key;
		leaf.value = value;
		return PutSpatial (leaf);
	}
	auto node = DecodeSpatial (Object (root));
	int	 common = CommonLevels (node.key, key);
	if (common < node.level)
	{
		if (value == ContentHash{})
			return root;
		SpatialNode branch;
		branch.key = key;
		branch.level = uint8_t (common);
		branch.children[Octant (node.key, common)] = root;
		branch.children[Octant (key, common)] = UpdateTree ({}, key, value);
		return PutSpatial (branch);
	}
	if (node.level == 64)
	{
		if (value == ContentHash{})
			return {};
		node.value = value;
		return PutSpatial (node);
	}
	int slot = Octant (key, node.level);
	node.children[slot] = UpdateTree (node.children[slot], key, value);
	int			count = 0;
	ContentHash only{};
	for (auto &child : node.children)
		if (child != ContentHash{})
		{
			++count;
			only = child;
		}
	if (count < 2)
		return only;
	return PutSpatial (node);
}
uint64_t ChunkRevision (ChunkKey key)
{
	Statement s (db, "SELECT revision FROM terrain_revisions WHERE x=? AND y=? AND z=?");
	s.Key (key);
	int status = sqlite3_step (s.statement);
	SqlCheck (status, db, "read terrain revision");
	return status == SQLITE_ROW ? uint64_t (sqlite3_column_int64 (s.statement, 0)) : 0;
}
TerrainProof Proof (ChunkKey key)
{
	TerrainProof p;
	p.key = key;
	p.root = Root ();
	p.revision = ChunkRevision (key);
	auto current = p.root;
	while (current != ContentHash{})
	{
		p.nodes.push_back (Object (current));
		auto n = DecodeSpatial (p.nodes.back ());
		if (CommonLevels (n.key, key) < n.level)
			break;
		if (n.level == 64)
		{
			p.overrideObject = Object (n.value);
			if (!p.overrideObject->bytes.empty () && p.overrideObject->bytes[0] == 3)
			{
				size_t cursor = 1;
				ApplyOverride (nullptr, p.overrideObject->bytes, cursor, 0);
				auto				  refs = DecodeSolidReferences (p.overrideObject->bytes, cursor);
				std::set<ContentHash> seen;
				size_t				  total = p.overrideObject->bytes.size () + 32768;
				for (const auto &ref : refs)
					if (seen.insert (ref.hash).second)
					{
						auto asset = Object (ref.hash);
						total += asset.bytes.size () + 37;
						if (total > MaxEncodedBytes)
							throw std::runtime_error ("terrain proof assets exceed transfer budget");
						p.assets.push_back (std::move (asset));
					}
			}
			break;
		}
		current = n.children[Octant (key, n.level)];
		if (p.nodes.size () > 65)
			throw std::runtime_error ("cyclic spatial tree");
	}
	return p;
}
std::shared_ptr<Chunk> Load (ChunkKey key)
{
	auto proof = Proof (key);
	auto chunk = Generate (key);
	chunk->revision = proof.revision;
	if (proof.overrideObject)
	{
		ValidateOverride (*proof.overrideObject, &proof.assets);
		size_t cursor = 1;
		ApplyOverride (&chunk->cells.impl->root, proof.overrideObject->bytes, cursor, 0);
		chunk->solids = DecodeOverrideSolids (proof.overrideObject->bytes, cursor, proof.assets);
	}
	return chunk;
}
void StoreTerrain (const Chunk &chunk)
{
	auto		base = Generate (chunk.key);
	auto		bytes = EncodeOverride (chunk.cells.impl->root, base->cells.impl->root);
	ContentHash value{};
	if (bytes != std::vector<uint8_t>{0} || (chunk.solids && !chunk.solids->empty ()))
	{
		bool hasSolids = chunk.solids && !chunk.solids->empty ();
		bytes.insert (bytes.begin (), hasSolids ? 3 : 1);
		if (hasSolids)
		{
			if (chunk.solids->size () > 256)
				throw std::runtime_error ("solid operation count exceeds 256 per chunk");
			Put64 (bytes, chunk.solids->size ());
			std::set<ContentHash> uniqueAssets;
			size_t				  assetBytes = 0;
			for (const auto &solid : *chunk.solids)
			{
				auto			placement = EncodeSolid (solid);
				SolidDefinition geometry = solid;
				geometry.origin = {};
				geometry.operation = SolidOperation::Add;
				auto encoded = EncodeSolid (geometry);
				auto hash = PutObject (ContentObjectType::SolidAsset, encoded);
				if (uniqueAssets.insert (hash).second)
					assetBytes += encoded.size () + 37;
				if (bytes.size () + 69 + assetBytes + 32768 > MaxEncodedBytes)
					throw std::runtime_error ("solid proof exceeds transfer budget");
				bytes.insert (bytes.end (), hash.begin (), hash.end ());
				bytes.insert (bytes.end (), placement.begin () + 5, placement.begin () + 42);
			}
		}
		value = PutObject (ContentObjectType::TerrainOverride, bytes);
	}
	auto	  root = UpdateTree (Root (), chunk.key, value);
	Statement revision (db, "INSERT OR REPLACE INTO terrain_revisions VALUES(?,?,?,?)");
	revision.Key (chunk.key);
	SqlCheck (sqlite3_bind_int64 (revision.statement, 4, int64_t (chunk.revision)), db, "bind terrain revision");
	SqlCheck (sqlite3_step (revision.statement), db, "persist terrain revision");
	Statement update (db, "UPDATE terrain_state SET root=? WHERE id=1");
	BindHash (update, 1, root);
	SqlCheck (sqlite3_step (update.statement), db, "publish terrain root");
}
void Store (const Chunk &chunk)
{
	SqlExec (db, "BEGIN IMMEDIATE");
	try
	{
		StoreTerrain (chunk);
		SqlExec (db, "COMMIT");
		RefreshContentHead ();
	}
	catch (...)
	{
		sqlite3_exec (db, "ROLLBACK", nullptr, nullptr, nullptr);
		throw;
	}
}
std::vector<uint8_t> Descriptor () const
{
	std::vector<uint8_t> bytes{'Q', 'W', 'G', 1};
	Put64 (bytes, 1); // Deterministic integer lattice generator implementation version.
	Put64 (bytes, uint64_t (config.generator));
	Put64 (bytes, config.seed);
	Put64 (bytes, ChunkSize);
	Put64 (bytes, VoxelSize);
	Put64 (bytes, 1); // Material definitions version.
	if (config.generator == Generator::Planet)
	{
		auto settings = EncodePlanetSettings (config.planetSettings);
		bytes.insert (bytes.end (), settings.begin (), settings.end ());
	}
	return bytes;
}
#include "world_private_store.inc"

void OpenMerkle ()
{
	SqlExec (
		db, "CREATE TABLE IF NOT EXISTS world_objects(hash BLOB PRIMARY KEY CHECK(length(hash)=32),type INTEGER NOT NULL,data BLOB NOT NULL) WITHOUT ROWID");
	SqlExec (
		db, "CREATE TABLE IF NOT EXISTS terrain_state(id INTEGER PRIMARY KEY CHECK(id=1),root BLOB NOT NULL,epoch INTEGER NOT NULL,descriptor BLOB NOT NULL)");
	SqlExec (db, "CREATE TABLE IF NOT EXISTS terrain_revisions(x INTEGER,y INTEGER,z INTEGER,revision INTEGER NOT NULL,PRIMARY KEY(x,y,z)) WITHOUT ROWID");
	SqlExec (
		db, "CREATE TABLE IF NOT EXISTS world_snapshots(name TEXT PRIMARY KEY,root BLOB NOT NULL,bytes INTEGER NOT NULL,checksum INTEGER NOT NULL,checkpoint "
			"BLOB NOT NULL,signature BLOB NOT NULL) WITHOUT ROWID");
	SqlExec (
		db, "CREATE TABLE IF NOT EXISTS snapshot_pages(name TEXT,id BLOB,revision INTEGER NOT NULL,payload BLOB NOT NULL,x INTEGER,y INTEGER,z INTEGER,deleted "
			"INTEGER NOT NULL,PRIMARY KEY(name,id)) WITHOUT ROWID");
	SqlExec (
		db, "CREATE TABLE IF NOT EXISTS private_objects(hash BLOB PRIMARY KEY CHECK(length(hash)=32),kind INTEGER NOT NULL,data BLOB NOT NULL) WITHOUT ROWID");
	Statement existing (db, "SELECT descriptor FROM terrain_state WHERE id=1");
	if (sqlite3_step (existing.statement) == SQLITE_ROW)
	{
		auto		expected = Descriptor ();
		int			size = sqlite3_column_bytes (existing.statement, 0);
		const auto *p = static_cast<const uint8_t *> (sqlite3_column_blob (existing.statement, 0));
		if (size != int (expected.size ()) || !std::equal (expected.begin (), expected.end (), p))
			throw std::runtime_error ("unsupported generator descriptor");
		return;
	}
	SqlExec (db, "BEGIN IMMEDIATE");
	try
	{
		Statement state (db, "INSERT INTO terrain_state VALUES(1,?,1,?)");
		BindHash (state, 1, {});
		auto descriptor = Descriptor ();
		SqlCheck (sqlite3_bind_blob (state.statement, 2, descriptor.data (), int (descriptor.size ()), SQLITE_TRANSIENT), db, "bind generator descriptor");
		SqlCheck (sqlite3_step (state.statement), db, "initialize terrain root");
		Statement chunks (db, "SELECT x,y,z,revision,data FROM chunks");
		for (int status; (status = sqlite3_step (chunks.statement)) != SQLITE_DONE;)
		{
			SqlCheck (status, db, "read legacy terrain");
			Chunk chunk;
			for (int axis = 0; axis < 3; ++axis)
				chunk.key[axis] = sqlite3_column_int64 (chunks.statement, axis);
			int64_t revision = sqlite3_column_int64 (chunks.statement, 3);
			int		size = sqlite3_column_bytes (chunks.statement, 4);
			if (revision < 1 || size < 5 || size > int (MaxEncodedBytes))
				throw std::runtime_error ("invalid legacy terrain");
			const auto *p = static_cast<const uint8_t *> (sqlite3_column_blob (chunks.statement, 4));
			chunk.revision = uint64_t (revision);
			if (!chunk.cells.Decode ({p, p + size}))
				throw std::runtime_error ("invalid legacy octree");
			StoreTerrain (chunk);
		}
		// Existing saves loaded current terrain and checkpoint page heads.
		Statement checkpoint (db, "SELECT bytes,checksum,data FROM world_checkpoint WHERE id=1");
		if (sqlite3_step (checkpoint.statement) == SQLITE_ROW)
		{
			int size = sqlite3_column_bytes (checkpoint.statement, 2);
			if (size < 0 || size > int (MaxCheckpointBytes) || sqlite3_column_int64 (checkpoint.statement, 0) != size)
				throw std::runtime_error ("invalid migrated checkpoint");
			std::vector<uint8_t> bytes;
			if (size)
			{
				const auto *p = static_cast<const uint8_t *> (sqlite3_column_blob (checkpoint.statement, 2));
				bytes.assign (p, p + size);
			}
			if (uint64_t (sqlite3_column_int64 (checkpoint.statement, 1)) != CheckpointChecksum (bytes))
				throw std::runtime_error ("corrupt migrated checkpoint");
			PinSnapshot ("latest", bytes);
			PinPages ("latest", true);
			SignSnapshot ("latest");
		}
		SqlExec (db, "DELETE FROM chunks");
		SqlExec (db, "COMMIT");
	}
	catch (...)
	{
		sqlite3_exec (db, "ROLLBACK", nullptr, nullptr, nullptr);
		throw;
	}
}
void PinSnapshot (const std::string &name, const std::vector<uint8_t> &bytes)
{
	Statement save (db, "INSERT OR REPLACE INTO world_snapshots VALUES(?,?,?,?,?,zeroblob(32))");
	SqlCheck (sqlite3_bind_text (save.statement, 1, name.c_str (), -1, SQLITE_TRANSIENT), db, "bind snapshot name");
	BindHash (save, 2, Root ());
	SqlCheck (sqlite3_bind_int64 (save.statement, 3, int64_t (bytes.size ())), db, "bind snapshot size");
	SqlCheck (sqlite3_bind_int64 (save.statement, 4, CheckpointChecksum (bytes)), db, "bind snapshot checksum");
	BindHash (save, 5, StorePrivate (bytes));
	SqlCheck (sqlite3_step (save.statement), db, "pin snapshot");
	PinPages (name);
	SignSnapshot (name);
}
std::vector<uint8_t> SnapshotBytes (const std::string &name)
{
	VerifySnapshot (name);
	Statement s (db, "SELECT bytes,checksum,checkpoint FROM world_snapshots WHERE name=?");
	sqlite3_bind_text (s.statement, 1, name.c_str (), -1, SQLITE_TRANSIENT);
	if (sqlite3_step (s.statement) != SQLITE_ROW)
		throw std::runtime_error ("snapshot does not exist");
	auto bytes = ReadPrivate (ColumnHash (s.statement, 2), MaxCheckpointBytes);
	if (sqlite3_column_int64 (s.statement, 0) != int64_t (bytes.size ()) || uint64_t (sqlite3_column_int64 (s.statement, 1)) != CheckpointChecksum (bytes))
		throw std::runtime_error ("snapshot checksum mismatch");
	return bytes;
}
void RestoreSnapshotState (const std::string &name)
{
	auto restoredBytes = SnapshotBytes (name); // Validate before any mutation.
	SqlExec (db, "BEGIN IMMEDIATE");
	try
	{
		Statement root (
			db, "UPDATE terrain_state SET root=(SELECT root FROM world_snapshots WHERE name=?),epoch=epoch+1 WHERE id=1 AND epoch<9223372036854775807");
		sqlite3_bind_text (root.statement, 1, name.c_str (), -1, SQLITE_TRANSIENT);
		SqlCheck (sqlite3_step (root.statement), db, "restore terrain root");
		if (sqlite3_changes (db) != 1)
			throw std::runtime_error ("authority epoch exhausted");
		SqlExec (db, "UPDATE terrain_revisions SET revision=revision+1 WHERE revision<9223372036854775807");
		MaterializePages (name);
		SqlExec (db, "DELETE FROM entity_page_heads");
		Statement pages (db, "INSERT INTO entity_page_heads SELECT id,revision FROM snapshot_pages WHERE name=?");
		sqlite3_bind_text (pages.statement, 1, name.c_str (), -1, SQLITE_TRANSIENT);
		SqlCheck (sqlite3_step (pages.statement), db, "restore snapshot pages");
		Statement checkpoint (db, "INSERT OR REPLACE INTO world_checkpoint VALUES(1,1,?,?,?)");
		sqlite3_bind_int64 (checkpoint.statement, 1, int64_t (restoredBytes.size ()));
		sqlite3_bind_int64 (checkpoint.statement, 2, CheckpointChecksum (restoredBytes));
		SqlCheck (
			restoredBytes.empty () ? sqlite3_bind_zeroblob (checkpoint.statement, 3, 0)
								   : sqlite3_bind_blob (checkpoint.statement, 3, restoredBytes.data (), int (restoredBytes.size ()), SQLITE_TRANSIENT),
			db, "restore checkpoint bytes");
		SqlCheck (sqlite3_step (checkpoint.statement), db, "restore snapshot checkpoint");
		SqlExec (db, "DELETE FROM entity_page_checkpoint");
		SqlExec (db, "INSERT INTO entity_page_checkpoint SELECT * FROM entity_page_heads");
		// Make the normal checkpoint reload refer to the restored terrain, too.
		if (name != "latest")
			PinSnapshot ("latest", SnapshotBytes (name));
		GarbageCollect (false);
		SqlExec (db, "COMMIT");
		RefreshContentHead ();
	}
	catch (...)
	{
		sqlite3_exec (db, "ROLLBACK", nullptr, nullptr, nullptr);
		throw;
	}
}
void GarbageCollect (bool ownTransaction = true)
{
	if (ownTransaction)
		SqlExec (db, "BEGIN IMMEDIATE");
	try
	{
		SqlExec (db, "CREATE TEMP TABLE IF NOT EXISTS reachable_objects(hash BLOB PRIMARY KEY) WITHOUT ROWID");
		SqlExec (db, "DELETE FROM reachable_objects");
		std::vector<ContentHash> pending{Root ()};
		{
			Statement roots (db, "SELECT root,name FROM world_snapshots");
			for (int s; (s = sqlite3_step (roots.statement)) != SQLITE_DONE;)
			{
				SqlCheck (s, db, "enumerate snapshot roots");
				VerifySnapshot (reinterpret_cast<const char *> (sqlite3_column_text (roots.statement, 1)));
				pending.push_back (ColumnHash (roots.statement, 0));
			}
		}
		while (!pending.empty ())
		{
			auto hash = pending.back ();
			pending.pop_back ();
			if (hash == ContentHash{})
				continue;
			Statement mark (db, "INSERT OR IGNORE INTO reachable_objects VALUES(?)");
			BindHash (mark, 1, hash);
			SqlCheck (sqlite3_step (mark.statement), db, "mark reachable object");
			if (!sqlite3_changes (db))
				continue;
			auto object = Object (hash);
			if (object.type == ContentObjectType::SpatialNode)
			{
				auto n = DecodeSpatial (object);
				if (n.level == 64)
					pending.push_back (n.value);
				else
					for (auto &c : n.children)
						pending.push_back (c);
			}
			else if (object.type == ContentObjectType::TerrainOverride)
			{
				ValidateOverride (object);
				if (object.bytes[0] == 3)
				{
					size_t cursor = 1;
					ApplyOverride (nullptr, object.bytes, cursor, 0);
					for (auto &ref : DecodeSolidReferences (object.bytes, cursor))
						pending.push_back (ref.hash);
				}
			}
			else
			{
				SolidDefinition solid;
				std::string		error;
				if (!DecodeSolid (object.bytes, solid, &error))
					throw std::runtime_error (error);
			}
		}
		SqlExec (db, "DELETE FROM world_objects WHERE hash NOT IN(SELECT hash FROM reachable_objects)");
		CollectPrivate ();
		PrunePages ();
		if (ownTransaction)
			SqlExec (db, "COMMIT");
	}
	catch (...)
	{
		if (ownTransaction)
			sqlite3_exec (db, "ROLLBACK", nullptr, nullptr, nullptr);
		throw;
	}
}
