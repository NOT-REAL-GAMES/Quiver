// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-License-Identifier: MIT
//
// ZOZO Contact Solver - Main Solver Implementation
// Based on: "A Cubic Barrier with Elasticity-Inclusive Dynamic Stiffness"
// by Ryoichi Ando (ACM Trans. Graph. 2024)

#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Constraints/ContactConstraintManager.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Core/TempAllocator.h>

JPH_NAMESPACE_BEGIN

/// ZOZO contact solver using cubic barriers and elasticity-inclusive dynamic stiffness
///
/// Implements Algorithm 1 from the paper:
/// ```
/// while β < β_max:
///     x, α ← inner_step(Δt, x)
///     β ← β + (1 - β)α
/// error_reduction_pass(βΔt, x)
/// ```
///
/// Key features:
/// - Cubic barrier energy (no search direction locking)
/// - Elasticity-inclusive dynamic stiffness (parameter-free)
/// - Newton solver with constraint-aware line search
/// - Substep-scaled time integration
class JPH_EXPORT ZOZOContactSolver
{
public:
	JPH_OVERRIDE_NEW_DELETE

	/// Constructor
	/// @param inPhysicsSettings Reference to physics settings
	explicit ZOZOContactSolver(const PhysicsSettings &inPhysicsSettings);

	/// Destructor
	~ZOZOContactSolver();

	/// Settings specific to ZOZO solver
	struct Settings
	{
		/// Maximum gap distance where barrier is active (ĝ in paper)
		float		mMaxGap = 0.02f;								// meters

		/// Target maximum β before exiting solver loop
		float		mBetaMax = 0.25f;								// fraction of timestep

		/// Search direction extension factor (for safety margin)
		float		mSearchExtension = 1.25f;						// multiplier

		/// Maximum Newton iterations per timestep
		uint		mMaxNewtonSteps = 32;

		/// PCG solver tolerance (relative residual in L∞ norm)
		float		mPCGTolerance = 1.0e-3f;

		/// Maximum PCG iterations
		uint		mMaxPCGIterations = 100;

		/// Whether to perform error reduction pass
		bool		mUseErrorReduction = true;

		/// Whether to use extended search direction (1.25×)
		bool		mUseExtendedSearch = true;
	};

	/// Set ZOZO-specific settings
	void SetSettings(const Settings &inSettings)					{ mSettings = inSettings; }

	/// Get current ZOZO settings
	const Settings &GetSettings() const								{ return mSettings; }

	/// Main solve function - replaces Sequential Impulse solver
	/// @param inDeltaTime Time step for this frame
	/// @param inConstraints Array of contact constraints
	/// @param inNumConstraints Number of constraints in array
	/// @param inTempAllocator Temporary allocator for scratch memory
	/// @return true if any changes were made
	bool SolveContacts(
		float inDeltaTime,
		ContactConstraintManager::ContactConstraint *inConstraints,
		uint32 inNumConstraints,
		TempAllocator *inTempAllocator);

	/// Statistics from last solve
	struct Statistics
	{
		uint		mNewtonIterations = 0;							// Actual Newton steps taken
		uint		mTotalPCGIterations = 0;						// Sum of all PCG iterations
		float		mFinalBeta = 0.0f;								// Final accumulated β
		float		mMaxPenetration = 0.0f;							// Worst penetration depth
		float		mAvgGap = 0.0f;									// Average contact gap
		bool		mConverged = false;								// Whether β_max was reached
	};

	/// Get statistics from last solve
	const Statistics &GetStatistics() const							{ return mStatistics; }

private:
	/// Perform one Newton iteration (Algorithm 1, line 4: inner_step)
	/// @param inDeltaTime Effective time step (β * Δt)
	/// @param outAlpha Line search scaling factor
	/// @return true if iteration succeeded
	bool InnerStep(float inDeltaTime, float &outAlpha);

	/// Error reduction pass (Algorithm 1, line 7)
	/// Reconfigures objective with βΔt and runs one more Newton step
	/// @param inEffectiveTimeStep β * Δt
	void ErrorReductionPass(float inEffectiveTimeStep);

	/// Compute global gradient and Hessian
	/// @param outGradient Output gradient vector (3n entries for n bodies)
	/// @param outHessian Output sparse Hessian matrix
	void ComputeGradientAndHessian(
		float *outGradient,
		uint32 inGradientSize);

	/// Solve linear system: H·d = -∇ψ using PCG
	/// @param inGradient Gradient vector (negative for descent direction)
	/// @param outSearchDirection Output search direction
	/// @param outPCGIterations Number of PCG iterations performed
	/// @return true if solve succeeded
	bool SolveLinearSystem(
		const float *inGradient,
		float *outSearchDirection,
		uint32 &outPCGIterations);

	/// Constraint-aware line search (Algorithm 1, line 13)
	/// Finds maximum α where no contacts penetrate
	/// @param inSearchDirection Search direction vector
	/// @return Maximum safe step size α ∈ [0, 1.25]
	float ConstraintLineSearch(const float *inSearchDirection);

	/// Apply search step to all bodies
	/// @param inSearchDirection Direction vector
	/// @param inAlpha Step size multiplier
	void ApplySearchStep(const float *inSearchDirection, float inAlpha);

	/// Update all semi-implicit stiffness values (κ̄ updates)
	/// Called at start of each Newton iteration
	void UpdateSemiImplicitStiffness();

	/// Build sparse Hessian from contact constraints
	/// Uses block-diagonal structure for efficiency
	void BuildSparseHessian();

	/// Apply block-Jacobi preconditioner for PCG
	/// @param inVector Input vector
	/// @param outResult Output: M^-1 · inVector
	void ApplyPreconditioner(const float *inVector, float *outResult);

	/// Compute total barrier energy (for diagnostics)
	float ComputeTotalEnergy() const;

	/// Validate all contact gaps (no penetration check)
	/// @return Maximum penetration depth (negative if all valid)
	float ValidateContactGaps() const;

private:
	/// Reference to physics settings
	const PhysicsSettings &		mPhysicsSettings;

	/// ZOZO-specific settings
	Settings					mSettings;

	/// Current constraints being solved
	ContactConstraintManager::ContactConstraint *mConstraints = nullptr;
	uint32						mNumConstraints = 0;

	/// Temporary allocator for scratch memory
	TempAllocator *				mTempAllocator = nullptr;

	/// Global system vectors (allocated per solve)
	float *						mGradient = nullptr;
	float *						mSearchDirection = nullptr;
	float *						mResidual = nullptr;
	float *						mPreconditioned = nullptr;

	/// Sparse Hessian storage (block-diagonal + contact blocks)
	struct SparseBlock
	{
		uint32					mRow;
		uint32					mCol;
		Mat44					mBlock;									// 3x3 stored in Mat44
	};
	SparseBlock *				mHessianBlocks = nullptr;
	uint32						mNumHessianBlocks = 0;

	/// Block-diagonal preconditioner (per-body 3x3 blocks)
	Mat44 *						mPreconditionerBlocks = nullptr;

	/// Statistics
	Statistics					mStatistics;

	/// System size (3 * number of dynamic bodies)
	uint32						mSystemSize = 0;

	/// Body index mapping
	const Body **				mActiveBodies = nullptr;
	uint32						mNumActiveBodies = 0;
};

JPH_NAMESPACE_END
