# ZOZO Contact Solver Integration Analysis

## Executive Summary
This document analyzes Jolt Physics v5.3.0's current contact constraint solver and identifies integration points for the ZOZO cubic barrier contact solver from the paper "A Cubic Barrier with Elasticity-Inclusive Dynamic Stiffness" by Ryoichi Ando (ACM Trans. Graph. 2024).

## Current Jolt Physics Contact Solver Architecture

### Core Approach: Sequential Impulse Method
Jolt uses Erin Catto's Sequential Impulse (SI) approach from GDC 2009, which solves contact constraints through iterative velocity and position corrections.

**Key Equations:**
```
Effective Mass:  K = J M^-1 J^T
Lagrange Multiplier: lambda = -K^-1 (J v + b)
Impulse: P = J^T lambda
Velocity Update: v' = v + M^-1 P
```

### Architecture Components

#### 1. **ContactConstraintManager** (`ContactConstraintManager.h/cpp`)
- **Location:** `Jolt/Physics/Constraints/ContactConstraintManager.{h,cpp}`
- **Role:** Manages all contact constraints for the physics system
- **Key Methods:**
  - `AddContactConstraint()` - Creates contact constraints from collision manifolds
  - `WarmStartVelocityConstraints()` - Applies previous frame impulses
  - `SolveVelocityConstraints()` - Iteratively solves velocity constraints (line 1643)
  - `SolvePositionConstraints()` - Baumgarte stabilization for penetration (line 1721)
  - `StoreAppliedImpulses()` - Caches impulses for next frame warm start

#### 2. **AxisConstraintPart** (`Jolt/Physics/Constraints/ConstraintPart/AxisConstraintPart.h`)
- **Role:** Represents a single 1-axis constraint (normal or tangent/friction)
- **Key Data:**
  ```cpp
  Float3 mR1PlusUxAxis;           // Cross product terms for body 1
  Float3 mR2xAxis;                // Cross product terms for body 2
  Float3 mInvI1_R1PlusUxAxis;     // Inertia-scaled angular terms
  Float3 mInvI2_R2xAxis;          // Inertia-scaled angular terms
  float mEffectiveMass;           // Cached 1/K (inverse of effective mass)
  float mTotalLambda;             // Accumulated impulse
  ```

- **Key Methods:**
  - `TemplatedCalculateInverseEffectiveMass()` - Computes K^-1 = J M^-1 J^T (line 76)
  - `TemplatedSolveVelocityConstraint()` - Solves lambda and applies impulse (line 433)
  - `SolvePositionConstraint()` - Baumgarte position correction (line 554)

#### 3. **WorldContactPoint** (`ContactConstraintManager.h` line 420)
- **Role:** Stores constraint data for a single contact point
- **Contains:**
  ```cpp
  AxisConstraintPart mNonPenetrationConstraint;  // Normal constraint
  AxisConstraintPart mFrictionConstraint1;       // Tangent 1
  AxisConstraintPart mFrictionConstraint2;       // Tangent 2
  CachedContactPoint* mContactPoint;             // Cache linkage
  ```

#### 4. **ContactConstraint** (`ContactConstraintManager.h` line 441)
- **Role:** Manages a complete contact manifold (up to 4 points)
- **Contains:**
  ```cpp
  Body *mBody1, *mBody2;
  Float3 mWorldSpaceNormal;
  float mCombinedFriction;
  float mInvMass1, mInvMass2;
  float mInvInertiaScale1, mInvInertiaScale2;
  WorldContactPoints mContactPoints;  // Max 4 points
  ```

### Solving Pipeline

#### Velocity Solving (10 iterations by default)
1. **For each contact constraint:**
   - Calculate Jacobian: `J = [-n, -(r1+u)×n, n, r2×n]`
   - Compute: `jv = J·v` (constraint violation rate)
   - Calculate: `lambda = mEffectiveMass * (jv - bias)`
   - Clamp lambda for friction: `|lambda_T| <= mu * |lambda_N|`
   - Apply impulse: `P = J^T * lambda`
   - Update velocities: `v += M^-1 * P`

2. **Friction handling:**
   - Uses Coulomb friction model
   - Two orthogonal tangent constraints
   - Friction limit depends on normal impulse from *same iteration*

3. **Restitution:**
   - Velocity bias: `b = e * v_n^-` (where e is restitution coefficient)
   - Only applied when closing velocity exceeds threshold

#### Position Solving (2 iterations by default)
1. **For each penetrating contact:**
   - Calculate separation: `C = (p2 - p1) · n`
   - Clamp to `[-mMaxPenetrationDistance, mPenetrationSlop]`
   - Compute position impulse: `lambda = -mEffectiveMass * mBaumgarte * C`
   - Directly update positions (Euler integration without velocity accumulation)

### PhysicsSettings Parameters

**File:** `Jolt/Physics/PhysicsSettings.h`

**Relevant Parameters:**
```cpp
float mBaumgarte = 0.2f;                    // Position error correction factor
float mSpeculativeContactDistance = 0.02f;   // Speculative contact radius
float mPenetrationSlop = 0.02f;             // Allowed penetration
float mMaxPenetrationDistance = 0.2f;        // Max position correction per iteration
uint mNumVelocitySteps = 10;                // Velocity solver iterations
uint mNumPositionSteps = 2;                 // Position solver iterations
float mMinVelocityForRestitution = 1.0f;    // Restitution threshold
bool mConstraintWarmStart = true;           // Use previous frame impulses
```

---

## ZOZO Solver Core Concepts

### 1. Cubic Barrier Energy (Equation 1 from paper)
```
ψ_weak(g_i, ĝ, κ̄_i) = {
    -2κ_i/(3ĝ_i) * (g_i - ĝ_i)³,  if g ≤ ĝ
    0,                              otherwise
}
```

**Properties:**
- C² continuous at g = ĝ
- Energy → ∞ as g → 0
- Search direction doesn't lock (unlike logarithmic barriers)
- Force: `f = -∂ψ/∂g = 2κ(g - ĝ)²/ĝ`
- Stiffness (Hessian): `∂²ψ/∂g² = 4κ(ĝ - g)/ĝ → 2κ as g → 0`

### 2. Elasticity-Inclusive Dynamic Stiffness (Equation 4)
```
κ̄ = m/g² + n·(Hn)
```

**Where:**
- `m` = element/vertex mass
- `g` = geometric gap distance
- `n` = contact normal
- `H` = elasticity Hessian matrix
- `κ̄` = semi-implicitly evaluated (treated as constant during derivative computation)

**Purpose:** Ensures contact stiffness matches system dynamics, preventing overly stiff/weak contacts

### 3. Custom Newton Solver (Algorithm 1)
```
while β < β_max:
    x, α ← inner_step(Δt, x)     # Newton step with line search
    β ← β + (1 - β)α              # Accumulated step size
```

**Features:**
- Constraint-aware line search (prevents penetration)
- Substep-scaled time stepping
- Extended search direction (1.25× for safety margin)
- Error reduction pass at end

---

## Integration Strategy

### Option A: Parallel Implementation (Recommended)
Create ZOZO as an **alternative contact solver** alongside Jolt's existing Sequential Impulse solver.

**Advantages:**
- Preserves existing solver for compatibility
- Allows A/B testing and benchmarking
- Gradual migration path
- Can fall back to SI solver if ZOZO fails

**Implementation:**
1. Create new solver class: `ZOZOContactSolver`
2. Add physics settings flag: `bool mUseZOZOSolver`
3. Dispatch in `PhysicsSystem::Update()`

### Option B: Replacement Implementation
Completely replace the Sequential Impulse solver with ZOZO.

**Advantages:**
- Simpler codebase
- No dual-solver maintenance
- Forces full commitment to ZOZO approach

**Disadvantages:**
- No fallback mechanism
- Breaking change for existing Jolt users
- Harder to validate correctness

---

## Detailed Integration Points

### 1. New Files to Create

#### `Jolt/Physics/Constraints/ZOZO/ZOZOContactSolver.h/cpp`
**Purpose:** Main ZOZO solver implementation

**Key Methods:**
```cpp
class ZOZOContactSolver {
public:
    // Algorithm 1: Main solver loop
    void SolveContacts(float inDeltaTime,
                      ContactConstraint* inConstraints,
                      uint32 inNumConstraints);

private:
    // Newton step (Algorithm 1, line 4)
    float InnerStep(float inDeltaTime, float inBeta);

    // Constraint-aware line search
    float ConstraintLineSearch(const Vec3* inSearchDirection);

    // Compute gradient and Hessian
    void ComputeGradientAndHessian(Vec3* outGradient, Mat44* outHessian);
};
```

#### `Jolt/Physics/Constraints/ZOZO/CubicBarrier.h/cpp`
**Purpose:** Cubic barrier energy functions

**Key Functions:**
```cpp
// Evaluate barrier energy (Eq. 1)
float EvaluateBarrierEnergy(float inGap, float inMaxGap, float inStiffness);

// First derivative (force)
float EvaluateBarrierForce(float inGap, float inMaxGap, float inStiffness);

// Second derivative (stiffness/Hessian diagonal)
float EvaluateBarrierStiffness(float inGap, float inMaxGap, float inStiffness);
```

#### `Jolt/Physics/Constraints/ZOZO/ElasticityHessian.h/cpp`
**Purpose:** Compute elasticity Hessian for dynamic stiffness

**Key Functions:**
```cpp
// Compute elasticity-inclusive stiffness (Eq. 4)
float ComputeDynamicStiffness(
    float inMass,
    float inGap,
    Vec3Arg inNormal,
    const Mat44& inElasticityHessian);

// Build elasticity Hessian from body elastic properties
Mat44 BuildElasticityHessian(const Body& inBody);
```

### 2. Modifications to Existing Files

#### `PhysicsSettings.h`
Add ZOZO-specific parameters:
```cpp
// ZOZO Solver Settings
bool mUseZOZOSolver = false;           // Enable ZOZO solver
float mZOZOMaxGap = 0.02f;             // ĝ: max contact gap
float mZOZOBetaMax = 0.25f;            // Maximum β before exit
float mZOZOSearchExtension = 1.25f;    // Search direction extension factor
uint mZOZOMaxNewtonSteps = 32;         // Max Newton iterations
```

#### `ContactConstraintManager.h`
Add ZOZO solver integration:
```cpp
class ContactConstraintManager {
private:
    // ZOZO solver instance (if enabled)
    ZOZOContactSolver* mZOZOSolver = nullptr;

public:
    // Modified to support both solvers
    bool SolveVelocityConstraints(...);
    bool SolvePositionConstraints(...);
};
```

#### `PhysicsSystem.cpp`
Dispatch to appropriate solver:
```cpp
if (mPhysicsSettings.mUseZOZOSolver)
    contact_manager.SolveContactsZOZO(...);
else
    contact_manager.SolveContactsSI(...);  // Original solver
```

### 3. Data Structure Extensions

#### Enhanced Contact Point Storage
```cpp
struct ZOZOContactData {
    float mCurrentGap;              // Current gap distance g_i
    float mDynamicStiffness;        // κ̄_i (semi-implicit)
    float mBarrierEnergy;           // Cached barrier energy
    Mat44 mElasticityHessian;       // H for this contact
    Vec3 mExtendedDirection;        // 1.25× search direction
};
```

---

## Implementation Phases

### Phase 1: Core Barrier Implementation (Week 1-2)
- [ ] Implement `CubicBarrier.h/cpp`
- [ ] Unit tests for barrier energy, force, stiffness
- [ ] Validate C² continuity at g = ĝ
- [ ] Verify energy → ∞ as g → 0

### Phase 2: Dynamic Stiffness (Week 2-3)
- [ ] Implement elasticity Hessian computation
- [ ] Implement Equation 4 (κ̄ calculation)
- [ ] Test with various material stiffnesses
- [ ] Validate scaling with mass and gap

### Phase 3: Newton Solver (Week 3-5)
- [ ] Implement basic Newton solver loop
- [ ] Add constraint-aware line search
- [ ] Implement substep-scaled integration (Algorithm 1)
- [ ] Add extended search direction
- [ ] Error reduction pass

### Phase 4: Integration (Week 5-6)
- [ ] Create `ZOZOContactSolver` class
- [ ] Integrate with `ContactConstraintManager`
- [ ] Add physics settings parameters
- [ ] Implement solver dispatch logic

### Phase 5: Testing & Validation (Week 6-8)
- [ ] Port paper examples to Jolt
- [ ] Compare with Sequential Impulse solver
- [ ] Performance benchmarks
- [ ] Stability testing

### Phase 6: Optimization (Week 8-10)
- [ ] SIMD vectorization
- [ ] Cache optimization
- [ ] Parallel constraint solving
- [ ] GPU preparation (analyze data layout)

---

## Key Challenges & Solutions

### Challenge 1: Elasticity Hessian Computation
**Problem:** Jolt doesn't currently expose elasticity Hessians

**Solution:**
- Extend `Shape` classes to compute local Hessians
- Transform to world space using body rotation
- Cache per-contact (updated each Newton step)
- For rigid bodies: H = 0 (only inertial stiffness)

### Challenge 2: Semi-Implicit Stiffness Evaluation
**Problem:** κ̄ depends on positions but must be treated as constant for derivatives

**Solution:**
- Evaluate κ̄ at start of each Newton step
- Freeze during gradient/Hessian computation
- Update after velocity/position changes
- This breaks chain rule intentionally (per paper)

### Challenge 3: Global Newton Solver vs. Local SI
**Problem:** SI solves each constraint independently; ZOZO needs global system

**Solution:**
- Build global position vector **x** from all bodies
- Assemble global gradient and Hessian
- Solve: **d** = H^-1 **f**
- Distribute back to individual bodies
- Use sparse matrix solvers (CG with block-Jacobi preconditioner)

### Challenge 4: Line Search Constraints
**Problem:** Need to prevent penetration during line search

**Solution:**
- For each contact, compute max α where g(x + α·d) ≥ 0
- Take minimum across all contacts
- Binary search for exact collision time (CCD)
- This is expensive - paper uses 1.25× extension instead

### Challenge 5: Friction Integration
**Problem:** Paper focuses on frictionless contacts

**Solution:**
- Use separate quadratic potential (Eq. 10 from paper)
- Stiffness: κ̄^friction = μ * f^contact / max(ε, ||tangent_velocity||)
- Update after normal constraint resolution
- May need additional Newton iterations

---

## Performance Considerations

### Jolt SI Solver:
- **Complexity:** O(n) per iteration (n = contact count)
- **Iterations:** 10 velocity + 2 position = 12 total
- **Per-contact cost:** ~100-200 flops
- **Memory:** Minimal (constraint-local)

### ZOZO Solver (estimated):
- **Complexity:** O(n²) for dense Hessian, O(n) for sparse
- **Iterations:** ~1-8 Newton steps (depends on β_max)
- **Per-iteration cost:** Gradient (O(n)), Hessian assembly (O(n)), CG solve (O(n·k))
- **Memory:** Global vectors/matrices (3n floats for gradient, sparse Hessian)

**Optimization Strategies:**
1. Sparse Hessian representation (most contacts independent)
2. Block-diagonal preconditioner (per-body blocks)
3. Warm-start CG with previous solution
4. SIMD for barrier energy evaluation
5. Parallel constraint processing (islands)

---

## Testing Strategy

### Unit Tests
1. Cubic barrier properties (C², energy → ∞)
2. Dynamic stiffness computation
3. Newton solver convergence
4. Line search correctness

### Integration Tests
1. Single contact pair (sphere-sphere)
2. Stacking (gravitational settling)
3. Friction (sliding box)
4. High-stiffness materials
5. Thin shells (if extending beyond rigid bodies)

### Stress Tests (from paper)
1. Twisted cylinders (168M contacts)
2. Ribbons with impact
3. Squishy balls
4. Strain limiting (if implementing)

### Benchmarks
- Contacts/second throughput
- Memory usage scaling
- Convergence rate comparison
- Penetration depth statistics

---

## Conclusion

The ZOZO contact solver represents a significant architectural change from Jolt's Sequential Impulse approach. The recommended integration strategy is:

1. **Implement as parallel solver** (Option A)
2. **Start with frictionless contacts**
3. **Focus on rigid bodies initially**
4. **Validate thoroughly before optimization**
5. **Maintain SI solver as fallback**

This approach minimizes risk while enabling experimentation with the new solver's benefits:
- No penetration guaranteed (via line search)
- Better handling of stiff contacts
- Parameter-free stiffness selection
- Superior strain limiting (if extended beyond rigid bodies)

The main tradeoffs are:
- Increased computational cost per iteration
- More complex implementation
- Global system assembly overhead

Performance will depend heavily on optimization quality, particularly sparse matrix handling and parallelization.
