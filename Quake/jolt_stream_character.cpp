/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "jolt_stream_character.h"
#include "jolt_stream_world.h"
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace quiver
{
namespace
{
constexpr float Scale = float (JoltStreamWorld::Scale);
constexpr float StepHeight = 18 * Scale;
constexpr float CollisionTolerance = (1.0f / 128.0f) * Scale;
// Keep the predictive margin well above the allowed penetration. A thinner
// margin can miss the next steeper terrain face until the sweep reaches it at
// fraction zero, repeatedly stopping on an otherwise walkable triangle edge.
// This is 1/8 Quake unit; Jolt offsets the shape upward by the same padding,
// preserving the exact flat-floor height and the original box dimensions.
constexpr float Padding = 16 * CollisionTolerance;

JPH::Vec3 Scaled (const std::array<float, 3> &p)
{
	return JPH::Vec3 (p[0], p[1], p[2]) * Scale;
}
std::array<float, 3> Unscaled (JPH::Vec3Arg p)
{
	return {p.GetX () / Scale, p.GetY () / Scale, p.GetZ () / Scale};
}
class IgnoreSelf final : public JPH::BodyFilter
{
  public:
	explicit IgnoreSelf (uint64_t id) : id (id) {}
	bool ShouldCollideLocked (const JPH::Body &body) const override
	{
		return !id || body.GetUserData () != id;
	}

  private:
	uint64_t id;
};
} // namespace

struct JoltStreamCharacter::Impl : JPH::CharacterContactListener
{
	JoltStreamWorld				   &world;
	uint64_t						userData;
	JPH::Ref<JPH::CharacterVirtual> character;
	std::array<float, 3>			mins{}, maxs{};
	std::vector<uint64_t>			contacts;
	bool							allowSliding = true;

	Impl (JoltStreamWorld &world, uint64_t userData) : world (world), userData (userData) {}
	void Record (const JPH::BodyID &id)
	{
		if (!id.IsInvalid ())
			contacts.push_back (world.Physics ().GetBodyInterface ().GetUserData (id));
	}
	void OnContactAdded (
		const JPH::CharacterVirtual *, const JPH::BodyID &id, const JPH::SubShapeID &, JPH::RVec3Arg, JPH::Vec3Arg,
		JPH::CharacterContactSettings &settings) override
	{
		settings.mCanReceiveImpulses = false; // Game entities retain their Quake movement/think ownership.
		Record (id);
	}
	void OnContactPersisted (
		const JPH::CharacterVirtual *c, const JPH::BodyID &id, const JPH::SubShapeID &subshape, JPH::RVec3Arg p, JPH::Vec3Arg n,
		JPH::CharacterContactSettings &settings) override
	{
		OnContactAdded (c, id, subshape, p, n, settings);
	}
	void OnContactSolve (
		const JPH::CharacterVirtual *c, const JPH::BodyID &id, const JPH::SubShapeID &, JPH::RVec3Arg, JPH::Vec3Arg normal, JPH::Vec3Arg contactVelocity,
		const JPH::PhysicsMaterial *, JPH::Vec3Arg, JPH::Vec3 &newVelocity) override
	{
		Record (id);
		// Avoid idle gravity turning into downhill drift on a walkable surface.
		// Jumping and moving platforms retain their actual solver velocities.
		if (!allowSliding && contactVelocity.IsNearZero () && !c->IsSlopeTooSteep (normal))
			newVelocity = JPH::Vec3::sZero ();
	}
	void Create (const JoltCharacterInput &input)
	{
		mins = input.mins;
		maxs = input.maxs;
		JPH::CharacterVirtualSettings settings;
		settings.mUp = JPH::Vec3::sAxisZ ();
		settings.mShape = new JPH::BoxShape ((Scaled (maxs) - Scaled (mins)) * .5f, 0);
		settings.mShapeOffset = (Scaled (maxs) + Scaled (mins)) * .5f;
		// Only contacts near the feet can support the character, never a wall
		// touching its shoulders. The collision hull itself stays a Quake box.
		settings.mSupportingVolume = JPH::Plane (JPH::Vec3::sAxisZ (), -(mins[2] + 4) * Scale);
		settings.mMaxSlopeAngle = std::acos (.7f);
		settings.mCharacterPadding = Padding;
		settings.mPredictiveContactDistance = .25f * Scale;
		settings.mCollisionTolerance = CollisionTolerance;
		settings.mMaxCollisionIterations = 8;
		settings.mMaxConstraintIterations = 20;
		// Use the mesh's cooked active-edge flags. Enhanced face filtering can
		// discard the next steeper face while the box still touches its neighbor,
		// leaving the sweep to stop at zero distance without a matching predictive
		// constraint. Native active-edge contacts preserve both walkable faces.
		settings.mEnhancedInternalEdgeRemoval = false;
		settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
		settings.mMaxStrength = 0;
		character = new JPH::CharacterVirtual (&settings, world.ToLocal (input.position), JPH::Quat::sIdentity (), userData, &world.Physics ());
		character->SetListener (this);
	}
};

JoltStreamCharacter::JoltStreamCharacter (JoltStreamWorld &world, uint64_t userData) : impl (std::make_unique<Impl> (world, userData)) {}
JoltStreamCharacter::~JoltStreamCharacter () = default;
void JoltStreamCharacter::Reset ()
{
	impl->character = nullptr;
	impl->contacts.clear ();
}

JoltCharacterResult JoltStreamCharacter::Step (const JoltCharacterInput &input)
{
	JoltCharacterResult result;
	result.position = input.position;
	auto blocked = [&] (bool unknown = true)
	{
		result.blockedUnknown = unknown;
		result.blockedSolid = !unknown;
		Reset ();
		return result;
	};
	auto deferred = [&]
	{
		result.pending = true;
		result.velocity = input.velocity;
		result.grounded = input.grounded;
		if (impl->character)
			result.groundUserData = impl->character->GetGroundUserData ();
		return result;
	};
	if (!std::isfinite (input.deltaTime) || input.deltaTime < 0 || input.deltaTime > .25f)
		return blocked ();
	std::array<double, 3> offset;
	std::array<float, 3>  lower, upper;
	const float slopeTravel = input.grounded ? float (std::hypot (double (input.velocity[0]), double (input.velocity[1])) * input.deltaTime * 1.1) : 0;
	for (int a = 0; a < 3; ++a)
	{
		if (!std::isfinite (input.mins[a]) || !std::isfinite (input.maxs[a]) || input.mins[a] >= input.maxs[a] || !std::isfinite (input.velocity[a]) ||
			!std::isfinite (input.gravity[a]))
			return blocked ();
		offset[a] = double (input.velocity[a]) * input.deltaTime;
		// WalkStairs scans above and beyond the main sweep; StickToFloor scans
		// down. Publish their geometry too before any CharacterVirtual query.
		lower[a] = input.mins[a] - (a == 2 ? 20 + slopeTravel : 8);
		upper[a] = input.maxs[a] + (a == 2 ? 20 + slopeTravel : 8);
	}
	// Queue extraction/cooking before checking occupancy. A deferred scene
	// keeps the last character state and the game's pending jump impulse.
	if (!impl->world.SyncCharacter (input.position, offset, lower, upper))
		return impl->world.Pending () ? deferred () : blocked ();
	WorldPosition center;
	if (!Add (
			input.position,
			{(double (input.mins[0]) + input.maxs[0]) * .5, (double (input.mins[1]) + input.maxs[1]) * .5, (double (input.mins[2]) + input.maxs[2]) * .5},
			center))
		return blocked ();
	if (impl->world.Source ().Sample (center).contents == Unknown)
		return impl->world.IsAsync () ? deferred () : blocked ();
	const auto centerContents = impl->world.Contents (center);
	if (centerContents.contents == Unknown && impl->world.IsAsync ())
		return deferred ();
	if (centerContents.IsSolid ())
	{
		// A mesh contains only the boundary. A character entirely inside a
		// deep solid slab must not move freely because its local bubble has no
		// triangles. Leave recovery to Jolt whenever a boundary is close enough
		// to reach inside its expanded stair/penetration-query neighborhood.
		bool interior = true;
		for (int corner = 0; corner < 8 && interior; ++corner)
		{
			WorldPosition		  point;
			std::array<double, 3> delta;
			for (int a = 0; a < 3; ++a)
				delta[a] = ((corner >> a) & 1) ? upper[a] : lower[a];
			interior = Add (input.position, delta, point) && impl->world.Contents (point).IsSolid ();
		}
		if (interior)
			return blocked (false);
	}
	if (!impl->character || impl->mins != input.mins || impl->maxs != input.maxs)
		impl->Create (input);
	else
		impl->character->SetPosition (impl->world.ToLocal (input.position));
	auto	  &character = *impl->character;
	auto	  &physics = impl->world.Physics ();
	auto	  &allocator = impl->world.Allocator ();
	IgnoreSelf filter (impl->userData);
	impl->contacts.clear ();
	character.RefreshContacts (physics.GetDefaultBroadPhaseLayerFilter (0), physics.GetDefaultLayerFilter (0), filter, {}, allocator);
	JPH::Vec3  velocity = Scaled (input.velocity);
	const bool supported = input.grounded && input.stickToFloor && character.GetGroundState () == JPH::CharacterBase::EGroundState::OnGround;
	if (supported)
	{
		const auto normal = character.GetGroundNormal (), ground = character.GetGroundVelocity ();
		if (normal.GetZ () > .7f)
			velocity.SetZ (
				ground.GetZ () -
				((velocity.GetX () - ground.GetX ()) * normal.GetX () + (velocity.GetY () - ground.GetY ()) * normal.GetY ()) / normal.GetZ ());
	}
	impl->allowSliding = !supported || std::hypot (input.velocity[0], input.velocity[1]) > .01f;
	character.SetLinearVelocity (velocity);
	if (input.deltaTime > 0)
	{
		JPH::CharacterVirtual::ExtendedUpdateSettings settings;
		settings.mWalkStairsStepUp = input.allowStairs && input.grounded ? JPH::Vec3 (0, 0, StepHeight) : JPH::Vec3::sZero ();
		settings.mStickToFloorStepDown = input.stickToFloor && input.grounded ? JPH::Vec3 (0, 0, -StepHeight) : JPH::Vec3::sZero ();
		settings.mWalkStairsMinStepForward = .5f * Scale;
		settings.mWalkStairsStepForwardTest = 4 * Scale;
		settings.mWalkStairsStepDownExtra = JPH::Vec3::sZero ();
		character.ExtendedUpdate (
			input.deltaTime, Scaled (input.gravity), settings, physics.GetDefaultBroadPhaseLayerFilter (0), physics.GetDefaultLayerFilter (0), filter, {},
			allocator);
		// ExtendedUpdate only sticks after a downward move. Uphill tangent
		// motion can cross a flatter triangle while still moving upward; keep
		// walking attached across that small crest instead of launching a hop.
		// Jumping clears grounded in QC, and swimming disables stickToFloor.
		if (input.grounded && input.stickToFloor && !character.IsSupported ())
			character.StickToFloor (
				JPH::Vec3 (0, 0, -StepHeight), physics.GetDefaultBroadPhaseLayerFilter (0), physics.GetDefaultLayerFilter (0), filter, {}, allocator);
	}
	result.position = impl->world.FromLocal (character.GetPosition ());
	const auto normal = character.GetGroundNormal ();
	result.groundNormal = {normal.GetX (), normal.GetY (), normal.GetZ ()};
	result.groundVelocity = Unscaled (character.GetGroundVelocity ());
	result.grounded = character.GetGroundState () == JPH::CharacterBase::EGroundState::OnGround;
	// CharacterVirtual retains its requested velocity after resolving a hit.
	// Publish the supported velocity to QC on the landing frame itself: QC
	// adds its jump impulse to velocity_z, so a stale fall speed would cancel
	// a jump pressed immediately after landing. Preserve horizontal intent:
	// converting the fall speed into slope-tangent velocity would cause lateral
	// drift and could still leave enough downward speed to cancel the jump.
	auto resolvedVelocity = character.GetLinearVelocity ();
	if (result.grounded && !input.grounded && normal.GetZ () > .7f)
	{
		const auto	ground = character.GetGroundVelocity ();
		const float into = (resolvedVelocity - ground).Dot (normal);
		if (into < 0)
			resolvedVelocity.SetZ (
				ground.GetZ () +
				std::max (
					0.f, -((resolvedVelocity.GetX () - ground.GetX ()) * normal.GetX () + (resolvedVelocity.GetY () - ground.GetY ()) * normal.GetY ()) /
							 normal.GetZ ()));
	}
	result.velocity = Unscaled (resolvedVelocity);
	result.groundUserData = character.GetGroundUserData ();
	result.hitLimit = character.GetMaxHitsExceeded ();
	for (const auto &contact : character.GetActiveContacts ())
		if (contact.mHadCollision && !contact.mWasDiscarded)
			impl->contacts.push_back (contact.mUserData);
	std::sort (impl->contacts.begin (), impl->contacts.end ());
	impl->contacts.erase (std::unique (impl->contacts.begin (), impl->contacts.end ()), impl->contacts.end ());
	for (uint64_t id : impl->contacts)
	{
		if (id == std::numeric_limits<uint64_t>::max ())
			result.blockedUnknown = true;
		else
			result.touchedUserData.push_back (id);
	}
	return result;
}
} // namespace quiver
