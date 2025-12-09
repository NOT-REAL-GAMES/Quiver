// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-License-Identifier: MIT
//
// ZOZO Contact Solver - Cubic Barrier Energy Functions
// Based on: "A Cubic Barrier with Elasticity-Inclusive Dynamic Stiffness"
// by Ryoichi Ando (ACM Trans. Graph. 2024)

#pragma once

#include <Jolt/Jolt.h>

JPH_NAMESPACE_BEGIN

/// Cubic barrier energy function for contact and constraint handling
///
/// This implements Equation 1 from the paper:
/// ψ_weak(g_i, ĝ, κ̄_i) = -2κ_i/(3ĝ_i) * (g_i - ĝ_i)³  if g ≤ ĝ
///                      = 0                             otherwise
///
/// Properties:
/// - C² continuous at g = ĝ
/// - Energy → ∞ as g → 0
/// - No search direction locking (unlike logarithmic barriers)
class CubicBarrier
{
public:
	/// Evaluate the cubic barrier energy
	/// @param inGap Current gap distance (g_i)
	/// @param inMaxGap Maximum gap where barrier is active (ĝ_i)
	/// @param inStiffness Semi-implicit stiffness parameter (κ̄_i)
	/// @return Barrier energy value
	static float EvaluateEnergy(float inGap, float inMaxGap, float inStiffness);

	/// Evaluate the first derivative (force) of the cubic barrier
	/// @param inGap Current gap distance (g_i)
	/// @param inMaxGap Maximum gap where barrier is active (ĝ_i)
	/// @param inStiffness Semi-implicit stiffness parameter (κ̄_i)
	/// @return Force value: -∂ψ/∂g = 2κ(g - ĝ)²/ĝ
	static float EvaluateForce(float inGap, float inMaxGap, float inStiffness);

	/// Evaluate the second derivative (stiffness/curvature) of the cubic barrier
	/// @param inGap Current gap distance (g_i)
	/// @param inMaxGap Maximum gap where barrier is active (ĝ_i)
	/// @param inStiffness Semi-implicit stiffness parameter (κ̄_i)
	/// @return Curvature value: ∂²ψ/∂g² = 4κ(ĝ - g)/ĝ → 2κ as g → 0
	static float EvaluateStiffness(float inGap, float inMaxGap, float inStiffness);

	/// Check if gap is within barrier active region
	/// @param inGap Current gap distance
	/// @param inMaxGap Maximum gap where barrier is active
	/// @return true if g ≤ ĝ (barrier is active)
	static inline bool IsActive(float inGap, float inMaxGap)
	{
		return inGap <= inMaxGap;
	}

	/// Validate barrier parameters
	/// @param inGap Current gap distance
	/// @param inMaxGap Maximum gap where barrier is active
	/// @param inStiffness Stiffness parameter
	/// @return true if parameters are valid
	static bool ValidateParameters(float inGap, float inMaxGap, float inStiffness);

private:
	/// Minimum gap threshold to avoid division by zero (1 micrometer)
	static constexpr float cMinGap = 1.0e-6f;
};

JPH_NAMESPACE_END
