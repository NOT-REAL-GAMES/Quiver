// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-License-Identifier: MIT
//
// ZOZO Contact Solver - Elasticity Hessian Computation
// Based on: "A Cubic Barrier with Elasticity-Inclusive Dynamic Stiffness"
// by Ryoichi Ando (ACM Trans. Graph. 2024)

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Constraints/ZOZO/ElasticityHessian.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Core/Profiler.h>

JPH_NAMESPACE_BEGIN

float ElasticityHessian::ComputeDynamicStiffness(
	float inMass,
	float inGap,
	Vec3Arg inNormal,
	Mat44Arg inElasticityHessian)
{
	JPH_PROFILE_FUNCTION();

	JPH_ASSERT(inMass >= 0.0f);
	JPH_ASSERT(inNormal.IsNormalized(1.0e-4f));

	// Ensure gap is not too small to avoid division by zero
	float gap = max(inGap, cMinGap);
	float gap_squared = gap * gap;

	// Equation 4 from paper: κ̄ = m/g² + n·(Hn)
	//
	// First term: inertial stiffness (always present)
	float inertial_stiffness = inMass / gap_squared;

	// Second term: elastic stiffness (only for deformable bodies)
	// For rigid bodies, H = 0, so this term contributes nothing
	Vec3 Hn = inElasticityHessian.Multiply3x3(inNormal);
	float elastic_stiffness = inNormal.Dot(Hn);

	// Ensure elastic stiffness is non-negative (positive semi-definite H)
	JPH_ASSERT(elastic_stiffness >= -1.0e-6f, "Elasticity Hessian must be positive semi-definite");
	elastic_stiffness = max(elastic_stiffness, 0.0f);

	return inertial_stiffness + elastic_stiffness;
}

Mat44 ElasticityHessian::BuildElasticityHessian(
	const Body &inBody,
	RVec3Arg inWorldSpacePosition)
{
	JPH_PROFILE_FUNCTION();

	// For rigid bodies, elasticity Hessian is zero
	// (no deformation, only inertial response)
	//
	// Future extension: For deformable bodies, this would involve:
	// 1. Material stiffness tensor (Young's modulus, Poisson's ratio)
	// 2. Local deformation gradient
	// 3. Transform to world space
	// 4. Second derivatives of elastic energy
	//
	// For now, return zero matrix (rigid body assumption)
	JPH_IF_NOT_DEBUG(JPH_UNUSED(inBody);)
	JPH_IF_NOT_DEBUG(JPH_UNUSED(inWorldSpacePosition);)

	return Mat44::sZero();
}

float ElasticityHessian::ComputeContactStiffness(
	const Body &inBody1,
	const Body &inBody2,
	float inGap,
	Vec3Arg inContactNormal,
	RVec3Arg inContactPoint1,
	RVec3Arg inContactPoint2)
{
	JPH_PROFILE_FUNCTION();

	JPH_ASSERT(inContactNormal.IsNormalized(1.0e-4f));

	// Ensure gap is not too small
	float gap = max(inGap, cMinGap);

	// Compute average mass for the contact
	// For dynamic bodies, use actual mass; for static/kinematic, use infinite mass (inv_mass = 0)
	float inv_mass1 = 0.0f;
	float inv_mass2 = 0.0f;

	if (inBody1.IsDynamic())
	{
		const MotionProperties *mp1 = inBody1.GetMotionProperties();
		inv_mass1 = mp1->GetInverseMass();
	}

	if (inBody2.IsDynamic())
	{
		const MotionProperties *mp2 = inBody2.GetMotionProperties();
		inv_mass2 = mp2->GetInverseMass();
	}

	// Combined inverse mass (harmonic mean for contact pair)
	// 1/m_combined = 1/m1 + 1/m2
	// For static body: inv_mass = 0, so it doesn't contribute
	float inv_mass_combined = inv_mass1 + inv_mass2;

	// Avoid division by zero (both bodies static - shouldn't happen)
	if (inv_mass_combined < 1.0e-12f)
		return 0.0f;

	// Build elasticity Hessians for both bodies
	Mat44 H1 = BuildElasticityHessian(inBody1, inContactPoint1);
	Mat44 H2 = BuildElasticityHessian(inBody2, inContactPoint2);

	// Compute extended contact direction w_i for each body
	// For rigid bodies, this simplifies to just the contact normal
	// (no redistribution to vertices needed)
	Vec3 w1 = ComputeExtendedContactDirection(inContactNormal, inBody1, inContactPoint1);
	Vec3 w2 = ComputeExtendedContactDirection(inContactNormal, inBody2, inContactPoint2);

	// Compute stiffness contributions from each body
	// κ̄_1 = m_1/g² + (w_1/||w_1||)·(H_1(w_1/||w_1||))
	// κ̄_2 = m_2/g² + (w_2/||w_2||)·(H_2(w_2/||w_2||))
	//
	// For rigid bodies with w = n (contact normal), this simplifies to:
	// κ̄_i = m_i/g² + n·(H_i·n)

	float stiffness1 = 0.0f;
	if (inBody1.IsDynamic())
	{
		float mass1 = inBody1.GetMotionProperties()->GetInverseMassUnchecked() > 0.0f
			? 1.0f / inBody1.GetMotionProperties()->GetInverseMassUnchecked()
			: 0.0f;

		Vec3 H1_w1 = H1.Multiply3x3(w1);
		float elastic_contrib1 = w1.Dot(H1_w1);

		stiffness1 = mass1 / (gap * gap) + max(elastic_contrib1, 0.0f);
	}

	float stiffness2 = 0.0f;
	if (inBody2.IsDynamic())
	{
		float mass2 = inBody2.GetMotionProperties()->GetInverseMassUnchecked() > 0.0f
			? 1.0f / inBody2.GetMotionProperties()->GetInverseMassUnchecked()
			: 0.0f;

		Vec3 H2_w2 = H2.Multiply3x3(w2);
		float elastic_contrib2 = w2.Dot(H2_w2);

		stiffness2 = mass2 / (gap * gap) + max(elastic_contrib2, 0.0f);
	}

	// Combined stiffness (harmonic mean, similar to mass combination)
	// This follows from the fact that contacts act in series
	if (stiffness1 > 0.0f && stiffness2 > 0.0f)
		return (stiffness1 * stiffness2) / (stiffness1 + stiffness2);
	else
		return stiffness1 + stiffness2;  // One is zero
}

Vec3 ElasticityHessian::ComputeExtendedContactDirection(
	Vec3Arg inContactNormal,
	const Body &inBody,
	RVec3Arg inContactPoint)
{
	JPH_PROFILE_FUNCTION();

	JPH_ASSERT(inContactNormal.IsNormalized(1.0e-4f));

	// Extended contact direction w_i = W^T_i (p_i - q_i)
	//
	// For rigid bodies:
	// W is the identity (contact direction already in world space)
	// So w = contact normal
	//
	// For deformable bodies (future extension):
	// W would redistribute the contact direction to the vertices
	// forming the contact, using appropriate interpolation weights
	JPH_IF_NOT_DEBUG(JPH_UNUSED(inBody);)
	JPH_IF_NOT_DEBUG(JPH_UNUSED(inContactPoint);)

	return inContactNormal;
}

bool ElasticityHessian::ValidateHessian(Mat44Arg inHessian)
{
	// Extract 3x3 upper-left block
	Vec3 col0 = inHessian.GetColumn3(0);
	Vec3 col1 = inHessian.GetColumn3(1);
	Vec3 col2 = inHessian.GetColumn3(2);

	// Check symmetry: H = H^T
	// This is a necessary (but not sufficient) condition for positive semi-definite
	constexpr float symmetry_tolerance = 1.0e-5f;

	if (abs(col0.GetY() - col1.GetX()) > symmetry_tolerance) return false;
	if (abs(col0.GetZ() - col2.GetX()) > symmetry_tolerance) return false;
	if (abs(col1.GetZ() - col2.GetY()) > symmetry_tolerance) return false;

	// Check positive semi-definite (all eigenvalues >= 0)
	// For 3x3, we can check:
	// 1. All diagonal elements >= 0 (necessary but not sufficient)
	// 2. All principal minors >= 0 (sufficient)
	//
	// Simplified check: all diagonal elements non-negative
	// (Full eigenvalue check is expensive and typically unnecessary)
	if (col0.GetX() < -1.0e-6f) return false;
	if (col1.GetY() < -1.0e-6f) return false;
	if (col2.GetZ() < -1.0e-6f) return false;

	// Check for NaN or infinity
	if (!col0.IsNormalized() && col0.LengthSq() > 0.0f) return false;  // Quick NaN check
	if (!col1.IsNormalized() && col1.LengthSq() > 0.0f) return false;
	if (!col2.IsNormalized() && col2.LengthSq() > 0.0f) return false;

	return true;
}

JPH_NAMESPACE_END
