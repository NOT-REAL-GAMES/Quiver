/* SPDX-License-Identifier: GPL-2.0-or-later */
// Private snapshot data never enters the peer-visible terrain object namespace.
ContentHash PutPrivateObject (int kind, const std::vector<uint8_t> &bytes)
{
	auto	  hash = HashContent (ContentObjectType (kind), bytes);
	Statement s (db, "INSERT OR IGNORE INTO private_objects VALUES(?,?,?)");
	BindHash (s, 1, hash);
	SqlCheck (sqlite3_bind_int (s.statement, 2, kind), db, "bind private kind");
	SqlCheck (
		bytes.empty () ? sqlite3_bind_zeroblob (s.statement, 3, 0) : sqlite3_bind_blob (s.statement, 3, bytes.data (), int (bytes.size ()), SQLITE_TRANSIENT),
		db, "bind private bytes");
	SqlCheck (sqlite3_step (s.statement), db, "store private object");
	return hash;
}
std::vector<uint8_t> PrivateObject (ContentHash hash, int kind)
{
	Statement s (db, "SELECT kind,data FROM private_objects WHERE hash=?");
	BindHash (s, 1, hash);
	if (sqlite3_step (s.statement) != SQLITE_ROW || sqlite3_column_int (s.statement, 0) != kind || sqlite3_column_bytes (s.statement, 1) > 65536)
		throw std::runtime_error ("missing or invalid private object");
	std::vector<uint8_t> bytes;
	int					 size = sqlite3_column_bytes (s.statement, 1);
	if (size)
	{
		const auto *p = static_cast<const uint8_t *> (sqlite3_column_blob (s.statement, 1));
		bytes.assign (p, p + size);
	}
	if (HashContent (ContentObjectType (kind), bytes) != hash)
		throw std::runtime_error ("private object checksum mismatch");
	return bytes;
}
ContentHash StorePrivate (const std::vector<uint8_t> &bytes)
{
	if (bytes.size () > MaxCheckpointBytes)
		throw std::runtime_error ("private payload exceeds limit");
	std::vector<uint8_t> manifest;
	Put64 (manifest, bytes.size ());
	for (size_t offset = 0; offset < bytes.size (); offset += 65536)
	{
		auto end = std::min (offset + 65536, bytes.size ());
		auto piece = PutPrivateObject (128, {bytes.begin () + offset, bytes.begin () + end});
		manifest.insert (manifest.end (), piece.begin (), piece.end ());
	}
	return PutPrivateObject (129, manifest);
}
std::vector<uint8_t> ReadPrivate (ContentHash hash, size_t limit)
{
	auto	 manifest = PrivateObject (hash, 129);
	uint64_t size = Get64 (manifest, 0);
	if (size > limit || manifest.size () != 8 + 32 * ((size + 65535) / 65536))
		throw std::runtime_error ("invalid private manifest");
	std::vector<uint8_t> bytes;
	bytes.reserve (size_t (size));
	for (size_t offset = 8; offset < manifest.size (); offset += 32)
	{
		ContentHash piece;
		std::copy_n (manifest.begin () + offset, 32, piece.begin ());
		auto block = PrivateObject (piece, 128);
		if (block.size () != std::min (size_t (65536), size_t (size) - bytes.size ()))
			throw std::runtime_error ("invalid private piece length");
		bytes.insert (bytes.end (), block.begin (), block.end ());
	}
	return bytes;
}
void PinPages (const std::string &name, bool legacyCheckpoint = false)
{
	Statement erase (db, "DELETE FROM snapshot_pages WHERE name=?");
	sqlite3_bind_text (erase.statement, 1, name.c_str (), -1, SQLITE_TRANSIENT);
	SqlCheck (sqlite3_step (erase.statement), db, "replace snapshot pages");
	Statement pages (
		db, legacyCheckpoint ? "SELECT v.id,v.revision,v.x,v.y,v.z,v.deleted,v.bytes,v.checksum,v.data FROM entity_page_versions v JOIN entity_page_checkpoint "
							   "h ON h.id=v.id AND h.revision=v.revision ORDER BY v.id"
							 : "SELECT v.id,v.revision,v.x,v.y,v.z,v.deleted,v.bytes,v.checksum,v.data FROM entity_page_versions v JOIN entity_page_heads h ON "
							   "h.id=v.id AND h.revision=v.revision ORDER BY v.id");
	for (int status; (status = sqlite3_step (pages.statement)) != SQLITE_DONE;)
	{
		SqlCheck (status, db, "read snapshot page");
		int size = sqlite3_column_bytes (pages.statement, 8);
		if (size < 0 || size > int (MaxEntityPageBytes) || sqlite3_column_int64 (pages.statement, 6) != size)
			throw std::runtime_error ("invalid snapshot page size");
		std::vector<uint8_t> bytes;
		if (size)
		{
			const auto *p = static_cast<const uint8_t *> (sqlite3_column_blob (pages.statement, 8));
			bytes.assign (p, p + size);
		}
		if (uint64_t (sqlite3_column_int64 (pages.statement, 7)) != CheckpointChecksum (bytes))
			throw std::runtime_error ("invalid snapshot page checksum");
		auto	  payload = StorePrivate (bytes);
		Statement pin (db, "INSERT INTO snapshot_pages VALUES(?,?,?,?,?,?,?,?)");
		sqlite3_bind_text (pin.statement, 1, name.c_str (), -1, SQLITE_TRANSIENT);
		pin.Id (2, ReadPageId (pages.statement, 0));
		sqlite3_bind_int64 (pin.statement, 3, sqlite3_column_int64 (pages.statement, 1));
		BindHash (pin, 4, payload);
		for (int i = 0; i < 4; ++i)
			sqlite3_bind_int64 (pin.statement, 5 + i, sqlite3_column_int64 (pages.statement, 2 + i));
		SqlCheck (sqlite3_step (pin.statement), db, "pin private page payload");
	}
}
void MaterializePages (const std::string &name)
{
	Statement pages (db, "SELECT id,revision,payload,x,y,z,deleted FROM snapshot_pages WHERE name=?");
	sqlite3_bind_text (pages.statement, 1, name.c_str (), -1, SQLITE_TRANSIENT);
	for (int status; (status = sqlite3_step (pages.statement)) != SQLITE_DONE;)
	{
		SqlCheck (status, db, "read pinned pages");
		auto	  bytes = ReadPrivate (ColumnHash (pages.statement, 2), MaxEntityPageBytes);
		Statement version (db, "INSERT OR REPLACE INTO entity_page_versions VALUES(?,?,1,?,?,?,?,?,?,?)");
		version.Id (1, ReadPageId (pages.statement, 0));
		sqlite3_bind_int64 (version.statement, 2, sqlite3_column_int64 (pages.statement, 1));
		for (int i = 0; i < 3; ++i)
			sqlite3_bind_int64 (version.statement, 3 + i, sqlite3_column_int64 (pages.statement, 3 + i));
		sqlite3_bind_int64 (version.statement, 6, int64_t (bytes.size ()));
		sqlite3_bind_int64 (version.statement, 7, CheckpointChecksum (bytes));
		sqlite3_bind_int64 (version.statement, 8, sqlite3_column_int64 (pages.statement, 6));
		SqlCheck (
			bytes.empty () ? sqlite3_bind_zeroblob (version.statement, 9, 0)
						   : sqlite3_bind_blob (version.statement, 9, bytes.data (), int (bytes.size ()), SQLITE_TRANSIENT),
			db, "restore page bytes");
		SqlCheck (sqlite3_step (version.statement), db, "materialize pinned page");
	}
}
void CollectPrivate ()
{
	SqlExec (db, "CREATE TEMP TABLE IF NOT EXISTS reachable_private(hash BLOB PRIMARY KEY) WITHOUT ROWID");
	SqlExec (db, "DELETE FROM reachable_private");
	Statement roots (db, "SELECT checkpoint FROM world_snapshots UNION SELECT payload FROM snapshot_pages");
	for (int status; (status = sqlite3_step (roots.statement)) != SQLITE_DONE;)
	{
		SqlCheck (status, db, "enumerate private roots");
		auto	 root = ColumnHash (roots.statement, 0);
		auto	 manifest = PrivateObject (root, 129);
		uint64_t size = Get64 (manifest, 0);
		if (size > MaxCheckpointBytes || manifest.size () != 8 + 32 * ((size + 65535) / 65536))
			throw std::runtime_error ("invalid private GC manifest");
		Statement mark (db, "INSERT OR IGNORE INTO reachable_private VALUES(?)");
		BindHash (mark, 1, root);
		SqlCheck (sqlite3_step (mark.statement), db, "mark private manifest");
		for (size_t offset = 8; offset < manifest.size (); offset += 32)
		{
			ContentHash hash;
			std::copy_n (manifest.begin () + offset, 32, hash.begin ());
			PrivateObject (hash, 128);
			Statement piece (db, "INSERT OR IGNORE INTO reachable_private VALUES(?)");
			BindHash (piece, 1, hash);
			SqlCheck (sqlite3_step (piece.statement), db, "mark private piece");
		}
	}
	SqlExec (db, "DELETE FROM private_objects WHERE hash NOT IN(SELECT hash FROM reachable_private)");
}

// Canonical whole-snapshot identity binds generator, terrain, opaque checkpoint,
// and sorted page identities/locations/revisions. A hash chain keeps validation
// memory bounded regardless of how many entities have been paged out.
ContentHash SnapshotSignature (const std::string &name)
{
	Statement snapshot (db, "SELECT root,bytes,checksum,checkpoint FROM world_snapshots WHERE name=?");
	sqlite3_bind_text (snapshot.statement, 1, name.c_str (), -1, SQLITE_TRANSIENT);
	if (sqlite3_step (snapshot.statement) != SQLITE_ROW)
		throw std::runtime_error ("snapshot does not exist");
	std::vector<uint8_t> canonical = Descriptor ();
	auto				 root = ColumnHash (snapshot.statement, 0), checkpoint = ColumnHash (snapshot.statement, 3);
	canonical.insert (canonical.end (), root.begin (), root.end ());
	Put64 (canonical, uint64_t (sqlite3_column_int64 (snapshot.statement, 1)));
	Put64 (canonical, uint64_t (sqlite3_column_int64 (snapshot.statement, 2)));
	canonical.insert (canonical.end (), checkpoint.begin (), checkpoint.end ());
	ContentHash signature = HashContent (ContentObjectType (130), canonical);
	Statement	pages (db, "SELECT id,revision,payload,x,y,z,deleted FROM snapshot_pages WHERE name=? ORDER BY id");
	sqlite3_bind_text (pages.statement, 1, name.c_str (), -1, SQLITE_TRANSIENT);
	for (int status; (status = sqlite3_step (pages.statement)) != SQLITE_DONE;)
	{
		SqlCheck (status, db, "hash snapshot page manifest");
		canonical.assign (signature.begin (), signature.end ());
		Put64 (canonical, ReadPageId (pages.statement, 0));
		Put64 (canonical, uint64_t (sqlite3_column_int64 (pages.statement, 1)));
		auto payload = ColumnHash (pages.statement, 2);
		canonical.insert (canonical.end (), payload.begin (), payload.end ());
		for (int i = 3; i < 7; ++i)
			Put64 (canonical, uint64_t (sqlite3_column_int64 (pages.statement, i)));
		signature = HashContent (ContentObjectType (131), canonical);
	}
	return signature;
}
void SignSnapshot (const std::string &name)
{
	auto	  signature = SnapshotSignature (name);
	Statement s (db, "UPDATE world_snapshots SET signature=? WHERE name=?");
	BindHash (s, 1, signature);
	sqlite3_bind_text (s.statement, 2, name.c_str (), -1, SQLITE_TRANSIENT);
	SqlCheck (sqlite3_step (s.statement), db, "sign snapshot manifest");
}
void VerifySnapshot (const std::string &name)
{
	auto	  signature = SnapshotSignature (name);
	Statement s (db, "SELECT signature FROM world_snapshots WHERE name=?");
	sqlite3_bind_text (s.statement, 1, name.c_str (), -1, SQLITE_TRANSIENT);
	if (sqlite3_step (s.statement) != SQLITE_ROW || ColumnHash (s.statement, 0) != signature)
		throw std::runtime_error ("snapshot manifest checksum mismatch");
}
