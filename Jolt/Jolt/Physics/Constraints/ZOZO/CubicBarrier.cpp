// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-License-Identifier: MIT
//
// ZOZO Contact Solver - Cubic Barrier Energy Functions
// Based on: "A Cubic Barrier with Elasticity-Inclusive Dynamic Stiffness"
// by Ryoichi Ando (ACM Trans. Graph. 2024)

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Constraints/ZOZO/CubicBarrier.h>
#include <Jolt/Core/Profiler.h>

JPH_NAMESPACE_BEGIN

float CubicBarrier::EvaluateEnergy(float inGap, float inMaxGap, float inStiffness)
{
	JPH_PROFILE_FUNCTION();

	// Barrier is only active when g ≤ ĝ
	if (inGap > inMaxGap)
		return 0.0f;

	// Ensure gap is not too small to avoid numerical issues
	float gap = max(inGap, cMinGap);
	float max_gap = max(inMaxGap, cMinGap);

	JPH_ASSERT(inStiffness >= 0.0f);
	JPH_ASSERT(max_gap > 0.0f);

	// ψ_weak(g, ĝ, κ̄) = -2κ̄/(3ĝ) * (g - ĝ)³
	float delta = gap - max_gap;  // Always ≤ 0
	float delta_cubed = delta * delta * delta;

	return (-2.0f * inStiffness / (3.0f * max_gap)) * delta_cubed;
}

float CubicBarrier::EvaluateForce(float inGap, float inMaxGap, float inStiffness)
{
	JPH_PROFILE_FUNCTION();

	// Barrier is only active when g ≤ ĝ
	if (inGap > inMaxGap)
		return 0.0f;

	// Ensure gap is not too small to avoid numerical issues
	float gap = max(inGap, cMinGap);
	float max_gap = max(inMaxGap, cMinGap);

	JPH_ASSERT(inStiffness >= 0.0f);
	JPH_ASSERT(max_gap > 0.0f);

	// Force: -∂ψ/∂g = 2κ̄(g - ĝ)²/ĝ
	//
	// Derivation:
	// ψ = -2κ̄/(3ĝ) * (g - ĝ)³
	// ∂ψ/∂g = -2κ̄/(3ĝ) * 3(g - ĝ)²
	//       = -2κ̄(g - ĝ)²/ĝ
	// Force = -∂ψ/∂g = 2κ̄(g - ĝ)²/ĝ
	float delta = gap - max_gap;  // Always ≤ 0
	float delta_squared = delta * delta;

	return (2.0f * inStiffness / max_gap) * delta_squared;
}

float CubicBarrier::EvaluateStiffness(float inGap, float inMaxGap, float inStiffness)
{
	JPH_PROFILE_FUNCTION();

	// Barrier is only active when g ≤ ĝ
	if (inGap > inMaxGap)
		return 0.0f;

	// Ensure gap is not too small to avoid numerical issues
	float gap = max(inGap, cMinGap);
	float max_gap = max(inMaxGap, cMinGap);

	JPH_ASSERT(inStiffness >= 0.0f);
	JPH_ASSERT(max_gap > 0.0f);

	// Stiffness (curvature): ∂²ψ/∂g² = 4κ̄(ĝ - g)/ĝ
	//
	// Derivation:
	// Force = 2κ̄(g - ĝ)²/ĝ
	// ∂F/∂g = 2κ̄ * 2(g - ĝ)/ĝ
	//       = 4κ̄(g - ĝ)/ĝ
	//       = -4κ̄(ĝ - g)/ĝ  (flip sign for standard form)
	//
	// Note: As g → 0, stiffness → 4κ̄ĝ/ĝ = 4κ̄
	//       But the paper states it approaches 2κ̄
	//       Let me recalculate...
	//
	// Actually, taking second derivative of energy directly:
	// ψ = -2κ̄/(3ĝ) * (g - ĝ)³
	// ∂ψ/∂g = -2κ̄/(3ĝ) * 3(g - ĝ)² = -2κ̄(g - ĝ)²/ĝ
	// ∂²ψ/∂g² = -2κ̄/ĝ * 2(g - ĝ) = -4κ̄(g - ĝ)/ĝ = 4κ̄(ĝ - g)/ĝ
	//
	// As g → 0: ∂²ψ/∂g² = 4κ̄ĝ/ĝ = 4κ̄
	// Hmm, paper says 2κ̄. Let me check if there's a coefficient issue...
	//
	// From paper page 2, equation after (1):
	// "the second derivative (curvature) of our cubic barrier, ψ_weak,
	//  linearly approaches 2κ as g → 0"
	//
	// But with ψ = -2κ/(3ĝ) * (g - ĝ)³, I get 4κ.
	// Perhaps there's a different normalization in the original?
	// For now, implementing as derived. May need adjustment.

	float delta = gap - max_gap;  // Always ≤ 0

	return (4.0f * inStiffness / max_gap) * (-delta);  // = 4κ̄(ĝ - g)/ĝ
}

bool CubicBarrier::ValidateParameters(float inGap, float inMaxGap, float inStiffness)
{
	// Check for valid gap values
	if (inGap < 0.0f || inMaxGap <= 0.0f)
		return false;

	// Check for valid stiffness
	if (inStiffness < 0.0f)
		return false;

	// Check that max gap is not too small
	if (inMaxGap < cMinGap)
		return false;

	// Check for NaN or infinity
	if (!isfinite(inGap) || !isfinite(inMaxGap) || !isfinite(inStiffness))
		return false;

	return true;
}

JPH_NAMESPACE_END
