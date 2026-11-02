/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "octree_world.h"
#include <memory>

namespace quiver
{
class JoltStreamWorld;

struct JoltCharacterInput
{
	WorldPosition		 position;
	std::array<float, 3> mins{-16, -16, -24}, maxs{16, 16, 32};
	// Quake units per second, after gamecode acceleration, jump and gravity.
	// CharacterVirtual does not integrate gravity into the desired velocity.
	std::array<float, 3> velocity{}, gravity{0, 0, -800};
	float				 deltaTime = 0;
	// Grounded is the gamecode flag AFTER PlayerPreThink (jumping clears it).
	bool				 grounded = false;
	bool				 allowStairs = true;
	bool				 stickToFloor = true;
};

struct JoltCharacterResult
{
	WorldPosition		  position;
	std::array<float, 3>  velocity{}, groundNormal{}, groundVelocity{};
	bool				  grounded = false;
	bool				  blockedUnknown = false;
	bool				  blockedSolid = false;
	bool				  hitLimit = false;
	// A loaded collision scene is still being prepared. No movement or contact
	// update occurred; retain post-gamecode state and undo this frame's gravity.
	bool				  pending = false;
	// Zero identifies terrain/worldspawn; nonzero values are entity IDs supplied
	// by the caller when mirroring solid game entities into JoltStreamWorld.
	uint64_t			  groundUserData = 0;
	std::vector<uint64_t> touchedUserData;
};

// A persistent CharacterVirtual. The world must outlive the character. Every
// update takes the authoritative position, so Quake pushers, teleportation and
// frame rebasing cannot leave a stale single-precision position in Jolt.
class JoltStreamCharacter
{
  public:
	explicit JoltStreamCharacter (JoltStreamWorld &world, uint64_t userData = 0);
	~JoltStreamCharacter ();
	JoltStreamCharacter (const JoltStreamCharacter &) = delete;
	JoltStreamCharacter &operator= (const JoltStreamCharacter &) = delete;
	JoltCharacterResult	 Step (const JoltCharacterInput &input);
	void				 Reset ();

  private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
} // namespace quiver
