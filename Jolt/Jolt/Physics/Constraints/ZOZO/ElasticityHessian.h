// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-License-Identifier: MIT
//
// ZOZO Contact Solver - Elasticity Hessian Computation
// Based on: "A Cubic Barrier with Elasticity-Inclusive Dynamic Stiffness"
// by Ryoichi Ando (ACM Trans. Graph. 2024)

#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>

JPH_NAMESPACE_BEGIN

/// Computes elasticity-inclusive dynamic stiffness for contacts
///
/// This implements Equation 4 from the paper:
/// κ̄ = m/g² + n·(Hn)
///
/// Where:
/// - m = element/vertex mass
/// - g = geometric gap distance
/// - n = contact normal (unit vector)
/// - H = elasticity Hessian matrix (3x3 in world space)
/// - κ̄ = semi-implicitly evaluated stiffness
class ElasticityHessian
{
public:
	/// Compute elasticity-inclusive dynamic stiffness (Equation 4)
	/// @param inMass Mass of the contact element (kg)
	/// @param inGap Current gap distance (m)
	/// @param inNormal Contact normal (must be normalized)
	/// @param inElasticityHessian 3x3 elasticity Hessian matrix in world space
	/// @return Dynamic stiffness κ̄ (N/m)
	static float ComputeDynamicStiffness(
		float inMass,
		float inGap,
		Vec3Arg inNormal,
		Mat44Arg inElasticityHessian);

	/// Build elasticity Hessian from body properties
	/// For rigid bodies, this is typically zero (only inertial stiffness)
	/// For deformable bodies, this would involve material stiffness
	/// @param inBody The body to compute elasticity Hessian for
	/// @param inWorldSpacePosition Contact point in world space
	/// @return 3x3 elasticity Hessian matrix (upper-left 3x3 of Mat44)
	static Mat44 BuildElasticityHessian(
		const Body &inBody,
		RVec3Arg inWorldSpacePosition);

	/// Compute contact stiffness for a contact pair (Equation 5 from paper)
	/// κ̄^contact_i = m_i/g²_i + (w_i/||w_i||)·(H(w_i/||w_i||))
	/// @param inBody1 First body in contact
	/// @param inBody2 Second body in contact
	/// @param inGap Contact gap distance
	/// @param inContactNormal Contact normal (pointing from body1 to body2)
	/// @param inContactPoint1 Contact point on body 1 (world space)
	/// @param inContactPoint2 Contact point on body 2 (world space)
	/// @return Combined contact stiffness
	static float ComputeContactStiffness(
		const Body &inBody1,
		const Body &inBody2,
		float inGap,
		Vec3Arg inContactNormal,
		RVec3Arg inContactPoint1,
		RVec3Arg inContactPoint2);

	/// Compute extended contact direction w_i = W^T_i (p_i - q_i)
	/// This redistributes the contact direction to vertices forming the contact
	/// @param inContactNormal Contact normal
	/// @param inBody The body for which to compute extended direction
	/// @param inContactPoint Contact point in world space
	/// @return Extended contact direction vector
	static Vec3 ComputeExtendedContactDirection(
		Vec3Arg inContactNormal,
		const Body &inBody,
		RVec3Arg inContactPoint);

	/// Validate that elasticity Hessian is symmetric positive semi-definite
	/// @param inHessian The Hessian matrix to validate
	/// @return true if valid (symmetric and positive semi-definite)
	static bool ValidateHessian(Mat44Arg inHessian);

private:
	/// Minimum gap threshold to avoid division by zero
	static constexpr float cMinGap = 1.0e-6f;

	/// Helper: Extract 3x3 from Mat44
	static inline Vec3 Multiply3x3(Mat44Arg inMatrix, Vec3Arg inVector)
	{
		return Vec3(
			inMatrix.GetColumn3(0).Dot(inVector),
			inMatrix.GetColumn3(1).Dot(inVector),
			inMatrix.GetColumn3(2).Dot(inVector));
	}
};

JPH_NAMESPACE_END
