// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-License-Identifier: MIT
//
// ZOZO Contact Solver - Main Newton Solver Implementation
// Based on: "A Cubic Barrier with Elasticity-Inclusive Dynamic Stiffness"
// by Ryoichi Ando (ACM Trans. Graph. 2024)

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Constraints/ZOZO/ZOZOContactSolver.h>
#include <Jolt/Physics/Constraints/ZOZO/CubicBarrier.h>
#include <Jolt/Physics/Constraints/ZOZO/ElasticityHessian.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Core/Profiler.h>
#include <Jolt/Core/TempAllocator.h>

JPH_NAMESPACE_BEGIN

ZOZOContactSolver::ZOZOContactSolver(const PhysicsSettings &inPhysicsSettings) :
	mPhysicsSettings(inPhysicsSettings)
{
}

ZOZOContactSolver::~ZOZOContactSolver()
{
	// Note: All memory is allocated via TempAllocator,
	// so no manual cleanup needed here
}

bool ZOZOContactSolver::SolveContacts(
	float inDeltaTime,
	ContactConstraintManager::ContactConstraint *inConstraints,
	uint32 inNumConstraints,
	TempAllocator *inTempAllocator)
{
	JPH_PROFILE_FUNCTION();

	if (inNumConstraints == 0)
		return false;

	// Store constraint data
	mConstraints = inConstraints;
	mNumConstraints = inNumConstraints;
	mTempAllocator = inTempAllocator;

	// Reset statistics
	mStatistics = Statistics();

	// Count unique dynamic bodies to determine system size
	// We only need to track dynamic bodies (static/kinematic are fixed)
	Array<const Body *> unique_bodies;
	unique_bodies.reserve(inNumConstraints * 2);

	for (uint32 i = 0; i < inNumConstraints; ++i)
	{
		const auto &constraint = inConstraints[i];

		if (constraint.mBody1->IsDynamic())
		{
			if (std::find(unique_bodies.begin(), unique_bodies.end(), constraint.mBody1) == unique_bodies.end())
				unique_bodies.push_back(constraint.mBody1);
		}

		if (constraint.mBody2->IsDynamic())
		{
			if (std::find(unique_bodies.begin(), unique_bodies.end(), constraint.mBody2) == unique_bodies.end())
				unique_bodies.push_back(constraint.mBody2);
		}
	}

	mNumActiveBodies = (uint32)unique_bodies.size();
	mSystemSize = mNumActiveBodies * 3;  // 3 DOF per body (x, y, z)

	if (mSystemSize == 0)
		return false;  // No dynamic bodies to solve

	// Allocate system vectors
	mActiveBodies = (const Body **)inTempAllocator->Allocate(mNumActiveBodies * sizeof(const Body *));
	memcpy(mActiveBodies, unique_bodies.data(), mNumActiveBodies * sizeof(const Body *));

	mGradient = (float *)inTempAllocator->Allocate(mSystemSize * sizeof(float));
	mSearchDirection = (float *)inTempAllocator->Allocate(mSystemSize * sizeof(float));
	mResidual = (float *)inTempAllocator->Allocate(mSystemSize * sizeof(float));
	mPreconditioned = (float *)inTempAllocator->Allocate(mSystemSize * sizeof(float));

	// Allocate sparse Hessian storage
	// Diagonal blocks (one per body) + contact coupling blocks
	mNumHessianBlocks = mNumActiveBodies + (inNumConstraints * 2);  // Upper bound
	mHessianBlocks = (SparseBlock *)inTempAllocator->Allocate(mNumHessianBlocks * sizeof(SparseBlock));

	// Allocate preconditioner storage (diagonal blocks only)
	mPreconditionerBlocks = (Mat44 *)inTempAllocator->Allocate(mNumActiveBodies * sizeof(Mat44));

	// Algorithm 1: Substep-scaled Newton's method
	//
	// β = 0
	// while β < β_max:
	//     x, α ← inner_step(Δt, x)
	//     β ← β + (1 - β)α
	// error_reduction_pass(βΔt, x)

	float beta = 0.0f;
	uint newton_iters = 0;

	while (beta < mSettings.mBetaMax && newton_iters < mSettings.mMaxNewtonSteps)
	{
		float alpha = 0.0f;
		if (!InnerStep(inDeltaTime, alpha))
			break;  // Failed to make progress

		// Update accumulated step size
		// β ← β + (1 - β)α
		beta = beta + (1.0f - beta) * alpha;

		++newton_iters;
	}

	mStatistics.mNewtonIterations = newton_iters;
	mStatistics.mFinalBeta = beta;
	mStatistics.mConverged = (beta >= mSettings.mBetaMax);

	// Error reduction pass
	if (mSettings.mUseErrorReduction && beta > 0.0f)
	{
		ErrorReductionPass(beta * inDeltaTime);
	}

	// Compute diagnostics
	mStatistics.mMaxPenetration = ValidateContactGaps();

	// Cleanup (TempAllocator will handle deallocation)
	mConstraints = nullptr;
	mNumConstraints = 0;
	mTempAllocator = nullptr;

	return (newton_iters > 0);
}

bool ZOZOContactSolver::InnerStep(float inDeltaTime, float &outAlpha)
{
	JPH_PROFILE_FUNCTION();

	// Algorithm 1, line 4: inner_step
	//
	// 1. Update semi-implicit stiffness: κ̄ ← m/g² + n·(Hn)
	// 2. Compute gradient and Hessian: ∇ψ, H
	// 3. Solve linear system: H·d = -∇ψ
	// 4. Constraint-aware line search: α ← max{α : g(x + αd) ≥ 0}
	// 5. Apply step: x ← x + αd

	// Step 1: Update stiffness values (treating them as constant for derivatives)
	UpdateSemiImplicitStiffness();

	// Step 2: Compute gradient and Hessian
	ComputeGradientAndHessian(mGradient, mSystemSize);
	BuildSparseHessian();

	// Step 3: Solve linear system using PCG
	uint32 pcg_iterations = 0;
	if (!SolveLinearSystem(mGradient, mSearchDirection, pcg_iterations))
	{
		outAlpha = 0.0f;
		return false;  // Linear solve failed
	}

	mStatistics.mTotalPCGIterations += pcg_iterations;

	// Step 4: Constraint-aware line search
	outAlpha = ConstraintLineSearch(mSearchDirection);

	if (outAlpha <= 0.0f)
	{
		return false;  // No valid step found
	}

	// Step 5: Apply search step
	ApplySearchStep(mSearchDirection, outAlpha);

	return true;
}

void ZOZOContactSolver::ErrorReductionPass(float inEffectiveTimeStep)
{
	JPH_PROFILE_FUNCTION();

	// Reconfigure objective with βΔt and run one more Newton step
	// This helps reduce any remaining error from the approximate β accumulation

	float alpha = 0.0f;
	InnerStep(inEffectiveTimeStep, alpha);

	// Apply step (alpha already applied in InnerStep)
}

void ZOZOContactSolver::UpdateSemiImplicitStiffness()
{
	JPH_PROFILE_FUNCTION();

	// Update dynamic stiffness κ̄ for all contacts
	// κ̄ = m/g² + n·(Hn)
	//
	// This is called "semi-implicit" because we treat κ̄ as constant
	// when computing derivatives, but update it each Newton iteration

	for (uint32 i = 0; i < mNumConstraints; ++i)
	{
		auto &constraint = mConstraints[i];
		Vec3 normal = constraint.GetWorldSpaceNormal();

		// Process each contact point in the manifold
		for (auto &wcp : constraint.mContactPoints)
		{
			// Get contact point positions from cached data
			if (wcp.mContactPoint == nullptr)
				continue;

			// Calculate current gap from body positions
			// Note: In Jolt, contact points are stored in local space in the cache
			// We need to transform them to world space
			RVec3 p1 = constraint.mBody1->GetCenterOfMassPosition() +
			           constraint.mBody1->GetRotation() * Vec3::sLoadFloat3Unsafe(wcp.mContactPoint->mPosition1);
			RVec3 p2 = constraint.mBody2->GetCenterOfMassPosition() +
			           constraint.mBody2->GetRotation() * Vec3::sLoadFloat3Unsafe(wcp.mContactPoint->mPosition2);

			float gap = (float)Vec3(p1 - p2).Dot(normal);

			// Note: Stiffness will be recalculated in ComputeGradientAndHessian
			// to avoid modifying Jolt's core structures
			(void)gap;  // Mark as used for gap calculation
		}
	}
}

void ZOZOContactSolver::ComputeGradientAndHessian(
	float *outGradient,
	uint32 inGradientSize)
{
	JPH_PROFILE_FUNCTION();

	// Initialize gradient to zero
	memset(outGradient, 0, inGradientSize * sizeof(float));

	// Compute gradient: ∇ψ = ∂ψ/∂x
	//
	// For cubic barrier:
	// ∂ψ/∂x = ∂ψ/∂g · ∂g/∂x
	//
	// where:
	// ∂ψ/∂g = -2κ̄(g - ĝ)²/ĝ  (from CubicBarrier::EvaluateForce)
	// ∂g/∂x = n  (contact normal)

	for (uint32 i = 0; i < mNumConstraints; ++i)
	{
		auto &constraint = mConstraints[i];
		Vec3 normal = constraint.GetWorldSpaceNormal();

		for (auto &wcp : constraint.mContactPoints)
		{
			if (wcp.mContactPoint == nullptr)
				continue;

			// Get world space contact points
			RVec3 p1 = constraint.mBody1->GetCenterOfMassPosition() +
			           constraint.mBody1->GetRotation() * Vec3::sLoadFloat3Unsafe(wcp.mContactPoint->mPosition1);
			RVec3 p2 = constraint.mBody2->GetCenterOfMassPosition() +
			           constraint.mBody2->GetRotation() * Vec3::sLoadFloat3Unsafe(wcp.mContactPoint->mPosition2);

			float gap = (float)Vec3(p1 - p2).Dot(normal);

			// Compute stiffness
			float stiffness = ElasticityHessian::ComputeContactStiffness(
				*constraint.mBody1, *constraint.mBody2, gap, normal, p1, p2);

			// Compute force: -∂ψ/∂g
			float force = CubicBarrier::EvaluateForce(gap, mSettings.mMaxGap, stiffness);

			// Apply gradient contribution: ∂ψ/∂x = -force · n
			// (negative because force opposes penetration)
			Vec3 gradient_contrib = -force * normal;

			// Add to global gradient for both bodies
			// Find body indices in active body list
			int body1_idx = -1;
			int body2_idx = -1;

			if (constraint.mBody1->IsDynamic())
			{
				for (uint32 j = 0; j < mNumActiveBodies; ++j)
				{
					if (mActiveBodies[j] == constraint.mBody1)
					{
						body1_idx = (int)j;
						break;
					}
				}
			}

			if (constraint.mBody2->IsDynamic())
			{
				for (uint32 j = 0; j < mNumActiveBodies; ++j)
				{
					if (mActiveBodies[j] == constraint.mBody2)
					{
						body2_idx = (int)j;
						break;
					}
				}
			}

			// Add gradient contributions
			if (body1_idx >= 0)
			{
				outGradient[body1_idx * 3 + 0] += gradient_contrib.GetX();
				outGradient[body1_idx * 3 + 1] += gradient_contrib.GetY();
				outGradient[body1_idx * 3 + 2] += gradient_contrib.GetZ();
			}

			if (body2_idx >= 0)
			{
				outGradient[body2_idx * 3 + 0] -= gradient_contrib.GetX();
				outGradient[body2_idx * 3 + 1] -= gradient_contrib.GetY();
				outGradient[body2_idx * 3 + 2] -= gradient_contrib.GetZ();
			}
		}
	}
}

void ZOZOContactSolver::BuildSparseHessian()
{
	JPH_PROFILE_FUNCTION();

	// Build sparse Hessian: H = ∂²ψ/∂x²
	//
	// For cubic barrier:
	// ∂²ψ/∂x² = ∂²ψ/∂g² · (∂g/∂x ⊗ ∂g/∂x)
	//         = k · (n ⊗ n)
	//
	// where k = ∂²ψ/∂g² is the barrier stiffness (curvature)
	//
	// Structure:
	// - Diagonal blocks: sum of all contacts involving each body
	// - Off-diagonal blocks: coupling between bodies in contact

	uint32 block_idx = 0;

	// Initialize diagonal blocks to zero
	for (uint32 i = 0; i < mNumActiveBodies; ++i)
	{
		mPreconditionerBlocks[i] = Mat44::sZero();
	}

	// Process each contact constraint
	for (uint32 i = 0; i < mNumConstraints; ++i)
	{
		auto &constraint = mConstraints[i];
		Vec3 normal = constraint.GetWorldSpaceNormal();

		for (auto &wcp : constraint.mContactPoints)
		{
			if (wcp.mContactPoint == nullptr)
				continue;

			// Get world space contact points
			RVec3 p1 = constraint.mBody1->GetCenterOfMassPosition() +
			           constraint.mBody1->GetRotation() * Vec3::sLoadFloat3Unsafe(wcp.mContactPoint->mPosition1);
			RVec3 p2 = constraint.mBody2->GetCenterOfMassPosition() +
			           constraint.mBody2->GetRotation() * Vec3::sLoadFloat3Unsafe(wcp.mContactPoint->mPosition2);

			float gap = (float)Vec3(p1 - p2).Dot(normal);

			// Compute stiffness
			float stiffness = ElasticityHessian::ComputeContactStiffness(
				*constraint.mBody1, *constraint.mBody2, gap, normal, p1, p2);

			// Compute Hessian curvature: ∂²ψ/∂g²
			float curvature = CubicBarrier::EvaluateStiffness(gap, mSettings.mMaxGap, stiffness);

			// Build 3x3 Hessian block: k · (n ⊗ n)
			Mat44 hessian_block = Mat44::sZero();
			hessian_block.SetColumn3(0, curvature * normal.GetX() * normal);
			hessian_block.SetColumn3(1, curvature * normal.GetY() * normal);
			hessian_block.SetColumn3(2, curvature * normal.GetZ() * normal);

			// Find body indices
			int body1_idx = -1;
			int body2_idx = -1;

			if (constraint.mBody1->IsDynamic())
			{
				for (uint32 j = 0; j < mNumActiveBodies; ++j)
				{
					if (mActiveBodies[j] == constraint.mBody1)
					{
						body1_idx = (int)j;
						break;
					}
				}
			}

			if (constraint.mBody2->IsDynamic())
			{
				for (uint32 j = 0; j < mNumActiveBodies; ++j)
				{
					if (mActiveBodies[j] == constraint.mBody2)
					{
						body2_idx = (int)j;
						break;
					}
				}
			}

			// Add to diagonal blocks (for preconditioner)
			if (body1_idx >= 0)
			{
				// Extract 3x3 upper-left block and add
				for (int row = 0; row < 3; ++row)
				{
					Vec3 col_vec = hessian_block.GetColumn3(row);
					Vec3 existing = mPreconditionerBlocks[body1_idx].GetColumn3(row);
					mPreconditionerBlocks[body1_idx].SetColumn3(row, existing + col_vec);
				}
			}

			if (body2_idx >= 0)
			{
				// Extract 3x3 upper-left block and add
				for (int row = 0; row < 3; ++row)
				{
					Vec3 col_vec = hessian_block.GetColumn3(row);
					Vec3 existing = mPreconditionerBlocks[body2_idx].GetColumn3(row);
					mPreconditionerBlocks[body2_idx].SetColumn3(row, existing + col_vec);
				}
			}

			// Store off-diagonal coupling blocks
			// (Full sparse Hessian assembly - needed for Hessian-vector products in PCG)
			if (body1_idx >= 0 && body2_idx >= 0 && block_idx < mNumHessianBlocks - 2)
			{
				// Block (i, j)
				mHessianBlocks[block_idx].mRow = body1_idx;
				mHessianBlocks[block_idx].mCol = body2_idx;
				mHessianBlocks[block_idx].mBlock = hessian_block;
				++block_idx;

				// Block (j, i) - symmetric
				mHessianBlocks[block_idx].mRow = body2_idx;
				mHessianBlocks[block_idx].mCol = body1_idx;
				mHessianBlocks[block_idx].mBlock = hessian_block;
				++block_idx;
			}
		}
	}

	// Invert diagonal blocks for preconditioner
	// M^-1 ≈ diag(H)^-1
	for (uint32 i = 0; i < mNumActiveBodies; ++i)
	{
		// Extract 3x3 block and invert
		Mat44 block_3x3 = mPreconditionerBlocks[i];

		// Add small regularization to ensure invertibility
		block_3x3.SetColumn3(0, block_3x3.GetColumn3(0) + Vec3(1.0e-6f, 0, 0));
		block_3x3.SetColumn3(1, block_3x3.GetColumn3(1) + Vec3(0, 1.0e-6f, 0));
		block_3x3.SetColumn3(2, block_3x3.GetColumn3(2) + Vec3(0, 0, 1.0e-6f));

		// Simple 3x3 inversion
		// For now, use approximate diagonal inverse (Jacobi preconditioner)
		Vec3 diag_inv(
			block_3x3.GetColumn3(0).GetX() > 0.0f ? 1.0f / block_3x3.GetColumn3(0).GetX() : 1.0f,
			block_3x3.GetColumn3(1).GetY() > 0.0f ? 1.0f / block_3x3.GetColumn3(1).GetY() : 1.0f,
			block_3x3.GetColumn3(2).GetZ() > 0.0f ? 1.0f / block_3x3.GetColumn3(2).GetZ() : 1.0f
		);

		mPreconditionerBlocks[i] = Mat44::sZero();
		mPreconditionerBlocks[i].SetColumn3(0, Vec3(diag_inv.GetX(), 0, 0));
		mPreconditionerBlocks[i].SetColumn3(1, Vec3(0, diag_inv.GetY(), 0));
		mPreconditionerBlocks[i].SetColumn3(2, Vec3(0, 0, diag_inv.GetZ()));
	}

	mNumHessianBlocks = block_idx;
}

bool ZOZOContactSolver::SolveLinearSystem(
	const float *inGradient,
	float *outSearchDirection,
	uint32 &outPCGIterations)
{
	JPH_PROFILE_FUNCTION();

	// Solve: H·d = -∇ψ using Preconditioned Conjugate Gradient (PCG)
	//
	// PCG algorithm:
	// r = b - A·x  (residual)
	// z = M^-1·r   (preconditioned residual)
	// p = z        (search direction)
	// repeat:
	//   α = (r·z) / (p·A·p)
	//   x = x + α·p
	//   r_new = r - α·A·p
	//   if ||r_new|| < tolerance: break
	//   z_new = M^-1·r_new
	//   β = (r_new·z_new) / (r·z)
	//   p = z_new + β·p

	// Initialize: x = 0, r = b = -∇ψ
	memset(outSearchDirection, 0, mSystemSize * sizeof(float));

	for (uint32 i = 0; i < mSystemSize; ++i)
		mResidual[i] = -inGradient[i];

	// Apply preconditioner: z = M^-1·r
	ApplyPreconditioner(mResidual, mPreconditioned);

	// p = z
	float *p = (float *)mTempAllocator->Allocate(mSystemSize * sizeof(float));
	memcpy(p, mPreconditioned, mSystemSize * sizeof(float));

	// rz_old = r·z
	float rz_old = 0.0f;
	for (uint32 i = 0; i < mSystemSize; ++i)
		rz_old += mResidual[i] * mPreconditioned[i];

	// PCG iterations
	outPCGIterations = 0;
	float tolerance = mSettings.mPCGTolerance;

	for (uint32 iter = 0; iter < mSettings.mMaxPCGIterations; ++iter)
	{
		++outPCGIterations;

		// Compute A·p (Hessian-vector product)
		float *Ap = (float *)mTempAllocator->Allocate(mSystemSize * sizeof(float));
		memset(Ap, 0, mSystemSize * sizeof(float));

		// Diagonal contribution
		for (uint32 i = 0; i < mNumActiveBodies; ++i)
		{
			Vec3 p_vec(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]);
			Vec3 result = mPreconditionerBlocks[i].Multiply3x3(p_vec);
			Ap[i * 3 + 0] += result.GetX();
			Ap[i * 3 + 1] += result.GetY();
			Ap[i * 3 + 2] += result.GetZ();
		}

		// Off-diagonal contribution
		for (uint32 i = 0; i < mNumHessianBlocks; ++i)
		{
			uint32 row = mHessianBlocks[i].mRow;
			uint32 col = mHessianBlocks[i].mCol;
			Vec3 p_vec(p[col * 3], p[col * 3 + 1], p[col * 3 + 2]);
			Vec3 result = mHessianBlocks[i].mBlock.Multiply3x3(p_vec);
			Ap[row * 3 + 0] += result.GetX();
			Ap[row * 3 + 1] += result.GetY();
			Ap[row * 3 + 2] += result.GetZ();
		}

		// α = (r·z) / (p·A·p)
		float pAp = 0.0f;
		for (uint32 i = 0; i < mSystemSize; ++i)
			pAp += p[i] * Ap[i];

		if (abs(pAp) < 1.0e-12f)
			break;  // Breakdown

		float alpha = rz_old / pAp;

		// x = x + α·p
		for (uint32 i = 0; i < mSystemSize; ++i)
			outSearchDirection[i] += alpha * p[i];

		// r = r - α·A·p
		for (uint32 i = 0; i < mSystemSize; ++i)
			mResidual[i] -= alpha * Ap[i];

		// Check convergence
		float residual_norm = 0.0f;
		for (uint32 i = 0; i < mSystemSize; ++i)
			residual_norm = max(residual_norm, abs(mResidual[i]));

		if (residual_norm < tolerance)
			break;  // Converged

		// z = M^-1·r
		ApplyPreconditioner(mResidual, mPreconditioned);

		// β = (r_new·z_new) / (r_old·z_old)
		float rz_new = 0.0f;
		for (uint32 i = 0; i < mSystemSize; ++i)
			rz_new += mResidual[i] * mPreconditioned[i];

		float beta = rz_new / rz_old;
		rz_old = rz_new;

		// p = z + β·p
		for (uint32 i = 0; i < mSystemSize; ++i)
			p[i] = mPreconditioned[i] + beta * p[i];
	}

	return (outPCGIterations > 0);
}

void ZOZOContactSolver::ApplyPreconditioner(const float *inVector, float *outResult)
{
	JPH_PROFILE_FUNCTION();

	// Apply block-Jacobi preconditioner: z = M^-1·r
	// M^-1 ≈ diag(H)^-1

	for (uint32 i = 0; i < mNumActiveBodies; ++i)
	{
		Vec3 v(inVector[i * 3], inVector[i * 3 + 1], inVector[i * 3 + 2]);
		Vec3 result = mPreconditionerBlocks[i].Multiply3x3(v);
		outResult[i * 3 + 0] = result.GetX();
		outResult[i * 3 + 1] = result.GetY();
		outResult[i * 3 + 2] = result.GetZ();
	}
}

float ZOZOContactSolver::ConstraintLineSearch(const float *inSearchDirection)
{
	JPH_PROFILE_FUNCTION();

	// Find maximum α such that g(x + α·d) ≥ 0 for all contacts
	// (all contacts remain non-penetrating)
	//
	// With optional 1.25× extension: α ∈ [0, 1.25]

	float alpha_max = mSettings.mUseExtendedSearch ? mSettings.mSearchExtension : 1.0f;

	for (uint32 i = 0; i < mNumConstraints; ++i)
	{
		auto &constraint = mConstraints[i];
		Vec3 normal = constraint.GetWorldSpaceNormal();

		for (auto &wcp : constraint.mContactPoints)
		{
			if (wcp.mContactPoint == nullptr)
				continue;

			// Get current positions
			RVec3 p1 = constraint.mBody1->GetCenterOfMassPosition() +
			           constraint.mBody1->GetRotation() * Vec3::sLoadFloat3Unsafe(wcp.mContactPoint->mPosition1);
			RVec3 p2 = constraint.mBody2->GetCenterOfMassPosition() +
			           constraint.mBody2->GetRotation() * Vec3::sLoadFloat3Unsafe(wcp.mContactPoint->mPosition2);

			float gap = (float)Vec3(p1 - p2).Dot(normal);

			// Get search direction for both bodies
			Vec3 d1 = Vec3::sZero();
			Vec3 d2 = Vec3::sZero();

			if (constraint.mBody1->IsDynamic())
			{
				for (uint32 j = 0; j < mNumActiveBodies; ++j)
				{
					if (mActiveBodies[j] == constraint.mBody1)
					{
						d1 = Vec3(inSearchDirection[j * 3], inSearchDirection[j * 3 + 1], inSearchDirection[j * 3 + 2]);
						break;
					}
				}
			}

			if (constraint.mBody2->IsDynamic())
			{
				for (uint32 j = 0; j < mNumActiveBodies; ++j)
				{
					if (mActiveBodies[j] == constraint.mBody2)
					{
						d2 = Vec3(inSearchDirection[j * 3], inSearchDirection[j * 3 + 1], inSearchDirection[j * 3 + 2]);
						break;
					}
				}
			}

			// Compute rate of change of gap: dg/dα = (d1 - d2)·n
			float gap_rate = (d1 - d2).Dot(normal);

			// Solve: g + α·gap_rate ≥ 0
			// α ≤ -g / gap_rate (if gap_rate < 0)
			if (gap_rate < -1.0e-6f)
			{
				float alpha_limit = -gap / gap_rate;
				alpha_max = min(alpha_max, alpha_limit);
			}
		}
	}

	return max(alpha_max, 0.0f);
}

void ZOZOContactSolver::ApplySearchStep(const float *inSearchDirection, float inAlpha)
{
	JPH_PROFILE_FUNCTION();

	// Apply step: x ← x + α·d
	// Update body positions based on search direction

	for (uint32 i = 0; i < mNumActiveBodies; ++i)
	{
		Body *body = const_cast<Body *>(mActiveBodies[i]);

		if (!body->IsDynamic())
			continue;

		Vec3 delta(
			inAlpha * inSearchDirection[i * 3 + 0],
			inAlpha * inSearchDirection[i * 3 + 1],
			inAlpha * inSearchDirection[i * 3 + 2]
		);

		// Update position
		body->AddPositionStep(delta);
	}
}

float ZOZOContactSolver::ComputeTotalEnergy() const
{
	JPH_PROFILE_FUNCTION();

	float total_energy = 0.0f;

	for (uint32 i = 0; i < mNumConstraints; ++i)
	{
		auto &constraint = mConstraints[i];
		Vec3 normal = constraint.GetWorldSpaceNormal();

		for (auto &wcp : constraint.mContactPoints)
		{
			if (wcp.mContactPoint == nullptr)
				continue;

			RVec3 p1 = constraint.mBody1->GetCenterOfMassPosition() +
			           constraint.mBody1->GetRotation() * Vec3::sLoadFloat3Unsafe(wcp.mContactPoint->mPosition1);
			RVec3 p2 = constraint.mBody2->GetCenterOfMassPosition() +
			           constraint.mBody2->GetRotation() * Vec3::sLoadFloat3Unsafe(wcp.mContactPoint->mPosition2);

			float gap = (float)Vec3(p1 - p2).Dot(normal);

			float stiffness = ElasticityHessian::ComputeContactStiffness(
				*constraint.mBody1, *constraint.mBody2, gap, normal, p1, p2);

			total_energy += CubicBarrier::EvaluateEnergy(gap, mSettings.mMaxGap, stiffness);
		}
	}

	return total_energy;
}

float ZOZOContactSolver::ValidateContactGaps() const
{
	JPH_PROFILE_FUNCTION();

	float max_penetration = -FLT_MAX;

	for (uint32 i = 0; i < mNumConstraints; ++i)
	{
		auto &constraint = mConstraints[i];
		Vec3 normal = constraint.GetWorldSpaceNormal();

		for (auto &wcp : constraint.mContactPoints)
		{
			if (wcp.mContactPoint == nullptr)
				continue;

			RVec3 p1 = constraint.mBody1->GetCenterOfMassPosition() +
			           constraint.mBody1->GetRotation() * Vec3::sLoadFloat3Unsafe(wcp.mContactPoint->mPosition1);
			RVec3 p2 = constraint.mBody2->GetCenterOfMassPosition() +
			           constraint.mBody2->GetRotation() * Vec3::sLoadFloat3Unsafe(wcp.mContactPoint->mPosition2);

			float gap = (float)Vec3(p1 - p2).Dot(normal);

			// Negative gap = penetration
			if (gap < 0.0f)
				max_penetration = max(max_penetration, -gap);
		}
	}

	return max_penetration;
}

JPH_NAMESPACE_END
