// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-License-Identifier: MIT
//
// ZOZO Contact Solver - Settings Integration
// Extends PhysicsSettings with ZOZO-specific parameters

#pragma once

#include <Jolt/Jolt.h>

JPH_NAMESPACE_BEGIN

/// Additional settings for ZOZO contact solver
/// These extend the standard PhysicsSettings when ZOZO solver is enabled
struct ZOZOPhysicsSettings
{
	JPH_OVERRIDE_NEW_DELETE

	/// Enable ZOZO solver instead of Sequential Impulse solver
	bool		mUseZOZOSolver = false;

	/// Maximum gap distance where cubic barrier is active (ĝ in paper)
	/// Contacts beyond this distance have zero barrier energy
	/// Default: 2cm (matches mSpeculativeContactDistance in PhysicsSettings)
	float		mZOZOMaxGap = 0.02f;								// meters

	/// Target for accumulated step size β before exiting Newton loop
	/// Higher values = more accurate but slower
	/// Lower values = faster but less accurate
	/// Default: 0.25 (advance 25% of full timestep before error reduction)
	float		mZOZOBetaMax = 0.25f;								// dimensionless

	/// Search direction extension factor for safety margin
	/// Line search computes max α for x + 1.25·α·d instead of x + α·d
	/// This prevents settling too close to constraint boundaries
	/// Default: 1.25 (25% extension)
	float		mZOZOSearchExtension = 1.25f;						// multiplier

	/// Maximum Newton iterations per timestep
	/// If β_max not reached after this many iterations, solver exits anyway
	/// Default: 32 (sufficient for most contact scenarios)
	uint		mZOZOMaxNewtonSteps = 32;

	/// PCG solver tolerance for linear system solve
	/// Convergence criterion: ||residual||_∞ / ||rhs||_∞ < tolerance
	/// Default: 1e-3 (0.1% relative error)
	float		mZOZOPCGTolerance = 1.0e-3f;

	/// Maximum PCG iterations per Newton step
	/// Default: 100 (usually converges much faster with good preconditioner)
	uint		mZOZOMaxPCGIterations = 100;

	/// Whether to perform error reduction pass at end
	/// Reconfigures objective with βΔt and runs one final Newton step
	/// Default: true (recommended for accuracy)
	bool		mZOZOUseErrorReduction = true;

	/// Whether to use extended search direction (1.25× extension)
	/// Prevents vertices from settling too close to constraint limits
	/// Default: true (prevents instability in subsequent steps)
	bool		mZOZOUseExtendedSearch = true;

	/// Whether to enable friction using quadratic potential (Eq. 10 from paper)
	/// Note: Friction is expensive and may require more Newton iterations
	/// Default: false (start with frictionless contacts)
	bool		mZOZOEnableFriction = false;

	/// Friction coefficient μ (only used if mZOZOEnableFriction = true)
	/// Default: 0.5 (moderate friction)
	float		mZOZOFrictionCoefficient = 0.5f;

	/// Minimum tangential velocity for friction activation (ε in Eq. 11)
	/// Prevents division by zero when tangent velocity is nearly zero
	/// Default: 0.01 m/s (1 cm/s)
	float		mZOZOFrictionEpsilon = 0.01f;								// m/s

	/// Whether to enable strain limiting (requires mesh deformation)
	/// Default: false (only supported for deformable bodies)
	bool		mZOZOEnableStrainLimiting = false;

	/// Maximum allowed strain (1 + τ + ε̂ in paper)
	/// Only used if mZOZOEnableStrainLimiting = true
	/// Default: 1.05 (5% stretch limit)
	float		mZOZOMaxStrain = 1.05f;

	/// Whether to use warm starting (previous frame's solution as initial guess)
	/// Default: true (significantly improves convergence)
	bool		mZOZOUseWarmStart = true;

	///@name Debugging and diagnostics
	///@{

	/// Print statistics after each solve
	bool		mZOZOPrintStatistics = false;

	/// Validate contact gaps after each step (expensive)
	bool		mZOZOValidateGaps = false;

	/// Check Hessian positive-definiteness (very expensive)
	bool		mZOZOValidateHessian = false;

	///@}
};

JPH_NAMESPACE_END
