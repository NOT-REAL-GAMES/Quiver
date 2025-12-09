# ZOZO Contact Solver - Architecture Design

## Overview

This document describes the detailed architecture for integrating the ZOZO cubic barrier contact solver into Jolt Physics v5.3.0.

## Directory Structure

```
Jolt/Physics/Constraints/ZOZO/
├── CubicBarrier.h              # Cubic barrier energy functions (Eq. 1)
├── CubicBarrier.cpp
├── ElasticityHessian.h         # Dynamic stiffness computation (Eq. 4)
├── ElasticityHessian.cpp
├── ZOZOContactSolver.h         # Main solver (Algorithm 1)
├── ZOZOContactSolver.cpp
├── ZOZOSettings.h              # Settings integration
└── README.md                   # ZOZO solver documentation
```

## Class Hierarchy

```
┌─────────────────────────────────────────────────────────────┐
│                    PhysicsSystem                            │
│  ┌──────────────────────────────────────────────────────┐  │
│  │         ContactConstraintManager                     │  │
│  │  ┌────────────────┐    ┌──────────────────────────┐ │  │
│  │  │ SI Solver      │    │  ZOZOContactSolver        │ │  │
│  │  │ (original)     │ OR │  • InnerStep()            │ │  │
│  │  │ • WarmStart    │    │  • ConstraintLineSearch() │ │  │
│  │  │ • SolveVel     │    │  • ErrorReductionPass()   │ │  │
│  │  │ • SolvePos     │    │  • UpdateStiffness()      │ │  │
│  │  └────────────────┘    │                           │ │  │
│  │                        │  Uses:                     │ │  │
│  │                        │  • CubicBarrier           │ │  │
│  │                        │  • ElasticityHessian      │ │  │
│  │                        └──────────────────────────┘ │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

## Data Flow

### Contact Resolution Pipeline (ZOZO Enabled)

```
PhysicsSystem::Update()
    │
    ├─► Broad Phase (unchanged)
    │
    ├─► Narrow Phase (unchanged)
    │   └─► ContactConstraintManager::AddContactConstraint()
    │
    └─► Contact Solving
        │
        ├─► if (mUseZOZOSolver)
        │   │
        │   └─► ZOZOContactSolver::SolveContacts(Δt, constraints)
        │       │
        │       ├─► β = 0
        │       │
        │       └─► while (β < β_max) [Main Newton Loop]
        │           │
        │           ├─► InnerStep(Δt, α)
        │           │   │
        │           │   ├─► UpdateSemiImplicitStiffness()
        │           │   │   └─► For each contact:
        │           │   │       └─► κ̄ = m/g² + n·(Hn)
        │           │   │
        │           │   ├─► ComputeGradientAndHessian()
        │           │   │   └─► For each contact:
        │           │   │       ├─► ∇ψ = ∂(cubic_barrier)/∂x
        │           │   │       └─► H = ∂²(cubic_barrier)/∂x²
        │           │   │
        │           │   ├─► SolveLinearSystem()  [PCG Solver]
        │           │   │   └─► H·d = -∇ψ
        │           │   │
        │           │   ├─► ConstraintLineSearch(d)
        │           │   │   └─► α = max{α | g(x + 1.25αd) ≥ 0}
        │           │   │
        │           │   └─► ApplySearchStep(d, α)
        │           │       └─► x ← x + α·d
        │           │
        │           └─► β ← β + (1-β)α
        │
        └─► ErrorReductionPass(βΔt)
            └─► One final Newton step with adjusted timestep
```

## Key Components

### 1. CubicBarrier Class

**Purpose:** Implements cubic barrier energy function (Equation 1)

**Interface:**
```cpp
class CubicBarrier {
    // Energy: ψ(g) = -2κ/(3ĝ) * (g - ĝ)³
    static float EvaluateEnergy(float g, float ĝ, float κ̄);

    // Force: -∂ψ/∂g = 2κ(g - ĝ)²/ĝ
    static float EvaluateForce(float g, float ĝ, float κ̄);

    // Stiffness: ∂²ψ/∂g² = 4κ(ĝ - g)/ĝ
    static float EvaluateStiffness(float g, float ĝ, float κ̄);
};
```

**Properties Guaranteed:**
- ✓ C² continuity at g = ĝ
- ✓ Energy → ∞ as g → 0
- ✓ No division by zero (cMinGap = 1e-6m threshold)
- ✓ Symmetric (same for approach/separation)

### 2. ElasticityHessian Class

**Purpose:** Computes elasticity-inclusive dynamic stiffness (Equation 4)

**Interface:**
```cpp
class ElasticityHessian {
    // κ̄ = m/g² + n·(Hn)
    static float ComputeDynamicStiffness(
        float m, float g, Vec3 n, Mat44 H);

    // Build H from body properties
    static Mat44 BuildElasticityHessian(
        const Body &body, RVec3 contactPoint);

    // Combined stiffness for contact pair
    static float ComputeContactStiffness(
        const Body &b1, const Body &b2,
        float g, Vec3 n, RVec3 p1, RVec3 p2);
};
```

**For Rigid Bodies:**
- Elasticity Hessian H = 0 (no deformation)
- Stiffness = m/g² (purely inertial)

**For Deformable Bodies (future extension):**
- H derived from material stiffness tensor
- Includes both inertial and elastic components

### 3. ZOZOContactSolver Class

**Purpose:** Main solver orchestrator (Algorithm 1)

**Key Methods:**

#### Main Solve Loop
```cpp
bool SolveContacts(
    float Δt,
    ContactConstraint *constraints,
    uint32 numConstraints,
    TempAllocator *allocator)
{
    β = 0.0f;

    while (β < β_max) {
        float α;
        InnerStep(Δt, α);
        β += (1 - β) * α;
    }

    if (mUseErrorReduction)
        ErrorReductionPass(β * Δt);

    return true;
}
```

#### Newton Step
```cpp
bool InnerStep(float Δt, float &α)
{
    // 1. Update all κ̄ values (semi-implicit)
    UpdateSemiImplicitStiffness();

    // 2. Compute gradient and Hessian
    ComputeGradientAndHessian(∇ψ, H);

    // 3. Solve linear system: H·d = -∇ψ
    SolveLinearSystem(∇ψ, d, pcg_iters);

    // 4. Constraint-aware line search
    α = ConstraintLineSearch(d);

    // 5. Apply step
    ApplySearchStep(d, α);

    return α > 0;
}
```

#### Line Search
```cpp
float ConstraintLineSearch(const float *d)
{
    float α_max = 1.25f;  // Extended search

    for (each contact i) {
        // Find max α where g_i(x + αd) ≥ 0
        float α_i = ComputeMaxSafeAlpha(contact_i, d);
        α_max = min(α_max, α_i);
    }

    return α_max;
}
```

### 4. Sparse Matrix Structures

**Block-Diagonal + Contact Blocks:**

```
         Body 0    Body 1    Body 2
Body 0  [  D0  ]  [ C01  ]  [      ]
Body 1  [ C01ᵀ ]  [  D1  ]  [ C12  ]
Body 2  [      ]  [ C12ᵀ ]  [  D2  ]

D_i  = 3×3 diagonal block for body i (inertia + self-contact)
C_ij = 3×3 contact coupling block between bodies i and j
```

**Storage Format:**
```cpp
struct SparseBlock {
    uint32 mRow;      // Body index (row)
    uint32 mCol;      // Body index (col)
    Mat44  mBlock;    // 3×3 stored in Mat44 (upper-left)
};
```

**Assembly:**
- Diagonal blocks: inertia + sum of contact stiffnesses
- Off-diagonal: contact coupling terms
- Sparse storage: only non-zero blocks
- Symmetry: C_ij = C_ji^T (store upper triangle)

### 5. PCG Solver

**Preconditioned Conjugate Gradient:**

```cpp
bool SolveLinearSystem(const float *b, float *x)
{
    r = b - H·x
    z = M^{-1}·r    // Apply preconditioner
    p = z

    for (k = 0; k < max_iters; k++) {
        α = (r·z) / (p·H·p)
        x = x + α·p
        r_new = r - α·H·p

        if (||r_new||_∞ < tol * ||b||_∞)
            return true;  // Converged

        β = (r_new·z_new) / (r·z)
        p = z_new + β·p
        r = r_new
    }

    return false;  // Did not converge
}
```

**Preconditioner:** Block-Jacobi (per-body 3×3 blocks)
```cpp
M = diag(D0, D1, D2, ...)
M^{-1} = diag(D0^{-1}, D1^{-1}, D2^{-1}, ...)
```

## Integration with Existing Jolt Code

### Modified: PhysicsSettings.h

**Add at end of PhysicsSettings struct:**
```cpp
///@name ZOZO Contact Solver Settings
///@{

/// Enable ZOZO solver instead of Sequential Impulse
bool        mUseZOZOSolver = false;

/// Maximum gap distance for cubic barrier (ĝ)
float       mZOZOMaxGap = 0.02f;

/// Target β before exiting Newton loop
float       mZOZOBetaMax = 0.25f;

/// Search direction extension factor
float       mZOZOSearchExtension = 1.25f;

/// Maximum Newton iterations
uint        mZOZOMaxNewtonSteps = 32;

/// PCG solver tolerance
float       mZOZOPCGTolerance = 1.0e-3f;

/// Maximum PCG iterations
uint        mZOZOMaxPCGIterations = 100;

/// Use error reduction pass
bool        mZOZOUseErrorReduction = true;

/// Use extended search direction
bool        mZOZOUseExtendedSearch = true;

///@}
```

### Modified: ContactConstraintManager.h

**Add private member:**
```cpp
private:
    // ZOZO solver (allocated if mUseZOZOSolver = true)
    ZOZOContactSolver *mZOZOSolver = nullptr;
```

**Modify constructor:**
```cpp
ContactConstraintManager::ContactConstraintManager(
    const PhysicsSettings &settings)
    : mPhysicsSettings(settings)
{
    if (settings.mUseZOZOSolver)
        mZOZOSolver = new ZOZOContactSolver(settings);
}
```

**Modify destructor:**
```cpp
ContactConstraintManager::~ContactConstraintManager()
{
    delete mZOZOSolver;
}
```

### Modified: PhysicsSystem.cpp

**In collision resolution step:**
```cpp
// Solve velocity constraints
bool impulse_applied;
if (mPhysicsSettings.mUseZOZOSolver)
{
    // ZOZO solver (replaces both velocity and position steps)
    impulse_applied = contact_manager.SolveContactsZOZO(
        mUpdateContext, delta_time);
}
else
{
    // Original Sequential Impulse solver
    impulse_applied = contact_manager.WarmStartVelocityConstraints(...);

    for (uint i = 0; i < mPhysicsSettings.mNumVelocitySteps; ++i)
        impulse_applied |= contact_manager.SolveVelocityConstraints(...);

    contact_manager.StoreAppliedImpulses(...);

    for (uint i = 0; i < mPhysicsSettings.mNumPositionSteps; ++i)
        impulse_applied |= contact_manager.SolvePositionConstraints(...);
}
```

### New: ContactConstraintManager method

**Add to ContactConstraintManager class:**
```cpp
bool ContactConstraintManager::SolveContactsZOZO(
    PhysicsUpdateContext *context,
    float delta_time)
{
    JPH_ASSERT(mZOZOSolver != nullptr);

    return mZOZOSolver->SolveContacts(
        delta_time,
        mConstraints,
        mNumConstraints,
        context->mTempAllocator);
}
```

## Memory Layout

### Per-Solve Allocations (from TempAllocator)

**For n active bodies:**
```
Gradient:           3n floats     (12n bytes)
Search Direction:   3n floats     (12n bytes)
Residual:           3n floats     (12n bytes)
Preconditioned:     3n floats     (12n bytes)
PCG aux vectors:    6n floats     (24n bytes)
────────────────────────────────────────────
Total vectors:                     72n bytes
```

**For m contacts:**
```
Hessian blocks:     ~(n + 2m) blocks × 36 bytes
                    = (n + 2m) × 36 bytes

Example: n=1000 bodies, m=5000 contacts
  Vectors:  72KB
  Hessian:  ~432KB
  Total:    ~500KB
```

**Comparison to SI solver:**
- SI: ~100 bytes per contact (minimal)
- ZOZO: ~500 bytes per body + sparse Hessian
- Tradeoff: More memory for better convergence

## Performance Characteristics

### Computational Complexity

**Per Newton Iteration:**
```
Operation                   Complexity      Cost (n bodies, m contacts)
─────────────────────────────────────────────────────────────────────
Update stiffness            O(m)            ~100m flops
Compute gradient            O(m)            ~200m flops
Assemble Hessian            O(m)            ~500m flops
PCG solve                   O(k·m)          ~1000km flops (k iterations)
Line search                 O(m)            ~50m flops
─────────────────────────────────────────────────────────────────────
Total per iteration:        O(k·m)          ~(850 + 1000k)m flops
```

**Typical values:**
- PCG iterations k ≈ 10-20 (with good preconditioner)
- Newton iterations ≈ 4-8 (to reach β_max = 0.25)
- Total: ~40-160 Newton iterations worth
- Compare to SI: 12 iterations (10 velocity + 2 position)

**Performance factors:**
- ✓ No search direction locking → fewer iterations
- ✓ Better convergence per iteration
- ✗ More expensive per iteration (global solve)
- ✗ Sparse matrix overhead

### Expected Performance

**Scenarios where ZOZO wins:**
- Stiff contacts (high material stiffness)
- Tight stacking (many contact layers)
- Strain limiting (if extended beyond rigid bodies)
- Large time steps

**Scenarios where SI wins:**
- Sparse contacts (few simultaneous contacts)
- Soft materials (low stiffness)
- Small time steps
- Real-time constraints (fixed iteration budget)

## Testing Strategy

### Unit Tests

**CubicBarrier:**
```cpp
TEST(CubicBarrier, C2Continuity)
TEST(CubicBarrier, EnergyDivergence)
TEST(CubicBarrier, ForceDerivative)
TEST(CubicBarrier, StiffnessDerivative)
```

**ElasticityHessian:**
```cpp
TEST(ElasticityHessian, RigidBodyStiffness)
TEST(ElasticityHessian, SymmetricHessian)
TEST(ElasticityHessian, PositiveSemiDefinite)
```

**ZOZOSolver:**
```cpp
TEST(ZOZOSolver, SingleContact)
TEST(ZOZOSolver, BetaConvergence)
TEST(ZOZOSolver, LineSearchSafety)
TEST(ZOZOSolver, NoPenetration)
```

### Integration Tests

```cpp
TEST(Integration, SphereSphereContact)
TEST(Integration, BoxStack)
TEST(Integration, HighStiffness)
TEST(Integration, CompareWithSI)
```

### Benchmark Suite

```cpp
BENCHMARK(ZOZO_1000Bodies_5000Contacts)
BENCHMARK(ZOZO_vs_SI_Stacking)
BENCHMARK(ZOZO_vs_SI_HighStiffness)
BENCHMARK(Memory_Usage_Scaling)
```

## Build System Integration

### CMakeLists.txt modifications

**Add ZOZO files to Jolt target:**
```cmake
set(JOLT_PHYSICS_FILES
    ...
    ${PHYSICS_DIR}/Constraints/ZOZO/CubicBarrier.cpp
    ${PHYSICS_DIR}/Constraints/ZOZO/CubicBarrier.h
    ${PHYSICS_DIR}/Constraints/ZOZO/ElasticityHessian.cpp
    ${PHYSICS_DIR}/Constraints/ZOZO/ElasticityHessian.h
    ${PHYSICS_DIR}/Constraints/ZOZO/ZOZOContactSolver.cpp
    ${PHYSICS_DIR}/Constraints/ZOZO/ZOZOContactSolver.h
    ${PHYSICS_DIR}/Constraints/ZOZO/ZOZOSettings.h
)
```

**Optional: Feature flag:**
```cmake
option(JPH_ENABLE_ZOZO_SOLVER "Enable ZOZO contact solver" ON)

if(JPH_ENABLE_ZOZO_SOLVER)
    target_compile_definitions(Jolt PUBLIC JPH_ZOZO_SOLVER_ENABLED)
endif()
```

## API Usage Example

### Enabling ZOZO Solver

```cpp
// Create physics system with ZOZO enabled
PhysicsSettings settings;
settings.mUseZOZOSolver = true;
settings.mZOZOMaxGap = 0.02f;          // 2cm contact detection
settings.mZOZOBetaMax = 0.25f;         // 25% timestep target
settings.mZOZOMaxNewtonSteps = 32;     // Max iterations
settings.mZOZOPCGTolerance = 1e-3f;    // 0.1% solver tolerance

PhysicsSystem physics_system(settings);
```

### Switching Solvers at Runtime

```cpp
// Start with Sequential Impulse
physics_system.GetPhysicsSettings().mUseZOZOSolver = false;

// Run for 100 steps
for (int i = 0; i < 100; i++)
    physics_system.Update(1.0f/60.0f, ...);

// Switch to ZOZO for high-stiffness scenario
physics_system.GetPhysicsSettings().mUseZOZOSolver = true;

// Continue simulation
for (int i = 0; i < 100; i++)
    physics_system.Update(1.0f/60.0f, ...);
```

### Querying Statistics

```cpp
// After update step
const auto &stats = physics_system
    .GetContactConstraintManager()
    .GetZOZOSolver()
    .GetStatistics();

printf("Newton iterations: %d\n", stats.mNewtonIterations);
printf("PCG iterations: %d\n", stats.mTotalPCGIterations);
printf("Final beta: %.3f\n", stats.mFinalBeta);
printf("Max penetration: %.6f m\n", stats.mMaxPenetration);
printf("Converged: %s\n", stats.mConverged ? "yes" : "no");
```

## Future Extensions

### Phase 2: Friction Support
- Implement Equation 10 (quadratic friction potential)
- Update Newton solver to handle tangential forces
- Add friction stiffness computation (Equation 11)

### Phase 3: Strain Limiting
- Implement Equation 8 (strain limiting energy)
- Extend to deformable mesh support
- Add SVD computation for deformation gradient

### Phase 4: GPU Acceleration
- Port PCG solver to GPU
- Parallel gradient/Hessian assembly
- CUDA/Vulkan compute shader implementation

### Phase 5: Advanced Features
- Bilateral constraints (hinges, sliders)
- Contact manifold reduction
- Adaptive time stepping based on β convergence

## Conclusion

This architecture provides:

✓ **Clean separation**: ZOZO as parallel solver, SI preserved
✓ **Minimal invasiveness**: Only 3 files modified in core Jolt
✓ **Runtime switching**: Can toggle solvers via settings
✓ **Extensible design**: Easy to add friction, strain limiting
✓ **Performance transparency**: Comprehensive statistics
✓ **Testing infrastructure**: Unit, integration, and benchmarks

The design balances innovation (ZOZO solver) with pragmatism (preserve existing solver), enabling gradual adoption and thorough validation before full commitment.
