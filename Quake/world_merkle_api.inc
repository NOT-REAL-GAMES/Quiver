/* SPDX-License-Identifier: GPL-2.0-or-later */
// Included after World::Impl, inside namespace quiver.
std::vector<uint8_t> World::DescriptorBytes () const
{
	return impl->Descriptor ();
}
ContentHash World::CurrentRoot ()
{
	std::lock_guard lock (impl->contentMutex);
	return impl->cachedRoot;
}
uint64_t World::AuthorityEpoch ()
{
	std::lock_guard lock (impl->contentMutex);
	return impl->cachedEpoch;
}
bool World::ReadObject (const ContentHash &hash, ContentObject &object, std::string *error)
{
	try
	{
		auto result = impl->Submit ([this, hash] { return impl->Object (hash); }, true).get ();
		object = std::move (result);
		return true;
	}
	catch (const std::exception &e)
	{
		return impl->PageFailure (e.what (), error);
	}
}
bool World::GetTerrainProof (ChunkKey key, TerrainProof &proof, std::string *error)
{
 key=CanonicalWorldKey(impl->config,key);
	try
	{
		auto result = impl->Submit ([this, key] { return impl->Proof (key); }, true).get ();
		proof = std::move (result);
		return true;
	}
	catch (const std::exception &e)
	{
		return impl->PageFailure (e.what (), error);
	}
}
bool World::InstallTerrainProof (const TerrainProof &proof, std::string *error)
{
	if(CanonicalWorldKey(impl->config,proof.key)!=proof.key)
		return impl->PageFailure("terrain proof key is not canonical for this world",error);
	if (!ValidateTerrainProof (proof, error))
		return false;
	auto previous = Find (proof.key);
	if (previous && previous->revision > proof.revision)
		return true;
	try
	{
		auto chunk = impl->Submit (
							 [this, &proof]
							 {
								 auto result = impl->Generate (proof.key);
								 result->revision = proof.revision;
								 if (proof.overrideObject)
								 {
									 size_t cursor = 1;
									 ApplyOverride (&result->cells.impl->root, proof.overrideObject->bytes, cursor, 0);
									 result->solids = DecodeOverrideSolids (proof.overrideObject->bytes, cursor, proof.assets);
								 }
								 return result;
							 },
							 true)
						 .get ();
		return Install (*chunk, error);
	}
	catch (const std::exception &e)
	{
		return impl->PageFailure (e.what (), error);
	}
}
namespace
{
bool ValidSnapshotName (const std::string &name)
{
	return !name.empty () && name.size () <= 64 &&
		   std::all_of (
			   name.begin (), name.end (),
			   [] (unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
}
} // namespace
bool World::CreateSnapshot (const std::string &name, const std::vector<uint8_t> &checkpoint, std::string *error)
{
	if (!ValidSnapshotName (name) || name == "latest")
		return impl->PageFailure ("invalid or reserved snapshot name", error);
	if (checkpoint.size () > MaxCheckpointBytes)
		return impl->PageFailure ("snapshot checkpoint exceeds size limit", error);
	if (!Flush (error))
		return false;
	Pump (impl->edits.size () + impl->pageJobs.size () + impl->pending.size () + impl->meshes.size ());
	if (!impl->edits.empty () || !impl->pageWrites.empty ())
		return impl->PageFailure ("snapshot callbacks queued new writes", error);
	try
	{
		impl->Submit (
				[this, &name, &checkpoint]
				{
					SqlExec (impl->db, "BEGIN IMMEDIATE");
					try
					{
						Statement exists (impl->db, "SELECT 1 FROM world_snapshots WHERE name=?");
						sqlite3_bind_text (exists.statement, 1, name.c_str (), -1, SQLITE_TRANSIENT);
						if (sqlite3_step (exists.statement) == SQLITE_ROW)
							throw std::runtime_error ("snapshot name already exists");
						impl->PinSnapshot (name, checkpoint);
						impl->GarbageCollect (false);
						SqlExec (impl->db, "COMMIT");
					}
					catch (...)
					{
						sqlite3_exec (impl->db, "ROLLBACK", nullptr, nullptr, nullptr);
						throw;
					}
				},
				true)
			.get ();
		return true;
	}
	catch (const std::exception &e)
	{
		return impl->PageFailure (e.what (), error);
	}
}
bool World::LoadSnapshot (const std::string &name, std::vector<uint8_t> &checkpoint, std::string *error)
{
	try
	{
		auto bytes = impl->Submit ([this, &name] { return impl->SnapshotBytes (name); }, true).get ();
		checkpoint = std::move (bytes);
		return true;
	}
	catch (const std::exception &e)
	{
		return impl->PageFailure (e.what (), error);
	}
}
bool World::RestoreSnapshot (const std::string &name, std::string *error)
{
	if (!Flush (error))
		return false;
	Pump (impl->edits.size () + impl->pageJobs.size () + impl->pending.size () + impl->meshes.size ());
	if (!impl->edits.empty () || !impl->pageWrites.empty ())
		return impl->PageFailure ("restore callbacks queued new writes", error);
	try
	{
		impl->Submit ([this, &name] { impl->RestoreSnapshotState (name); }, true).get ();
		impl->pending.clear ();
		impl->meshes.clear ();
		impl->resident.clear ();
		return true;
	}
	catch (const std::exception &e)
	{
		return impl->PageFailure (e.what (), error);
	}
}
std::vector<std::string> World::ListSnapshots (std::string *error)
{
	try
	{
		return impl
			->Submit (
				[this]
				{
					std::vector<std::string> names;
					Statement				 s (impl->db, "SELECT name FROM world_snapshots ORDER BY name");
					for (int status; (status = sqlite3_step (s.statement)) != SQLITE_DONE;)
					{
						SqlCheck (status, impl->db, "list snapshots");
						names.emplace_back (reinterpret_cast<const char *> (sqlite3_column_text (s.statement, 0)));
					}
					return names;
				},
				true)
			.get ();
	}
	catch (const std::exception &e)
	{
		impl->PageFailure (e.what (), error);
		return {};
	}
}
bool World::DeleteSnapshot (const std::string &name, std::string *error)
{
	if (!ValidSnapshotName (name) || name == "latest")
		return impl->PageFailure ("invalid or reserved snapshot name", error);
	try
	{
		impl->Submit (
				[this, &name]
				{
					SqlExec (impl->db, "BEGIN IMMEDIATE");
					try
					{
						Statement s (impl->db, "DELETE FROM world_snapshots WHERE name=?");
						sqlite3_bind_text (s.statement, 1, name.c_str (), -1, SQLITE_TRANSIENT);
						SqlCheck (sqlite3_step (s.statement), impl->db, "delete snapshot");
						if (!sqlite3_changes (impl->db))
							throw std::runtime_error ("snapshot does not exist");
						Statement pages (impl->db, "DELETE FROM snapshot_pages WHERE name=?");
						sqlite3_bind_text (pages.statement, 1, name.c_str (), -1, SQLITE_TRANSIENT);
						SqlCheck (sqlite3_step (pages.statement), impl->db, "delete snapshot page pins");
						impl->GarbageCollect (false);
						SqlExec (impl->db, "COMMIT");
					}
					catch (...)
					{
						sqlite3_exec (impl->db, "ROLLBACK", nullptr, nullptr, nullptr);
						throw;
					}
				},
				true)
			.get ();
		return true;
	}
	catch (const std::exception &e)
	{
		return impl->PageFailure (e.what (), error);
	}
}
bool World::CollectGarbage (std::string *error)
{
	try
	{
		impl->Submit ([this] { impl->GarbageCollect (); }).get ();
		return true;
	}
	catch (const std::exception &e)
	{
		return impl->PageFailure (e.what (), error);
	}
}
