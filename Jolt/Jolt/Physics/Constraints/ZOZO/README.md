# ZOZO Contact Solver

Implementation of the cubic barrier contact solver from:

**"A Cubic Barrier with Elasticity-Inclusive Dynamic Stiffness"**
Ryoichi Ando (ZOZO, Japan)
ACM Transactions on Graphics, Vol. 43, No. 6, December 2024

## Overview

The ZOZO solver provides an alternative to Jolt's Sequential Impulse (SI) contact solver, offering:

- **Cubic barrier energies** - No search direction locking (vs. logarithmic barriers)
- **Dynamic stiffness** - Parameter-free stiffness selection using elasticity
- **Newton solver** - Global optimization vs. local sequential updates
- **Guaranteed penetration-free** - Via constraint-aware line search

## Quick Start

### Enable in Code

```cpp
#include <Jolt/Physics/PhysicsSettings.h>

PhysicsSettings settings;
settings.mUseZOZOSolver = true;           // Enable ZOZO
settings.mZOZOMaxGap = 0.02f;             // 2cm contact detection
settings.mZOZOBetaMax = 0.25f;            // Convergence target
settings.mZOZOMaxNewtonSteps = 32;        // Max iterations

PhysicsSystem physics(settings);
```

### Runtime Toggle

```cpp
// Switch from SI to ZOZO
physics.GetPhysicsSettings().mUseZOZOSolver = true;
```

## Files

| File | Purpose |
|------|---------|
| `CubicBarrier.h/cpp` | Cubic barrier energy functions (Eq. 1) |
| `ElasticityHessian.h/cpp` | Dynamic stiffness computation (Eq. 4) |
| `ZOZOContactSolver.h/cpp` | Main solver (Algorithm 1) |
| `ZOZOSettings.h` | Settings integration |

## Key Equations

### Cubic Barrier Energy (Equation 1)
```
ψ(g, ĝ, κ̄) = -2κ̄/(3ĝ) · (g - ĝ)³    if g ≤ ĝ
            = 0                        otherwise
```

### Dynamic Stiffness (Equation 4)
```
κ̄ = m/g² + n·(Hn)
```

Where:
- `m` = mass
- `g` = gap distance
- `n` = contact normal
- `H` = elasticity Hessian

### Algorithm 1: Main Solver Loop
```
β = 0
while β < β_max:
    x, α ← inner_step(Δt, x)     # Newton + line search
    β ← β + (1-β)α                # Accumulated step size
error_reduction_pass(βΔt, x)     # Final adjustment
```

## Settings Reference

| Setting | Default | Description |
|---------|---------|-------------|
| `mZOZOMaxGap` | 0.02m | Maximum barrier activation distance |
| `mZOZOBetaMax` | 0.25 | Target accumulated step size |
| `mZOZOSearchExtension` | 1.25 | Search direction safety factor |
| `mZOZOMaxNewtonSteps` | 32 | Maximum iterations per timestep |
| `mZOZOPCGTolerance` | 1e-3 | Linear solver convergence tolerance |
| `mZOZOMaxPCGIterations` | 100 | Maximum PCG iterations |
| `mZOZOUseErrorReduction` | true | Enable final error reduction pass |
| `mZOZOUseExtendedSearch` | true | Use 1.25× search extension |

## Performance

### When ZOZO Wins
- ✓ Stiff contacts (high material stiffness)
- ✓ Tight stacking (many contact layers)
- ✓ Large time steps
- ✓ Strain limiting scenarios

### When SI Wins
- ✓ Sparse contacts (few simultaneous)
- ✓ Soft materials
- ✓ Small time steps
- ✓ Real-time fixed budgets

### Typical Performance
- **Memory:** ~500 bytes/body + sparse Hessian
- **Iterations:** 4-8 Newton steps (vs. 12 SI iterations)
- **Per-iteration cost:** 5-10× more expensive than SI
- **Overall:** Similar or better for challenging scenarios

## Statistics

Query solver statistics after each step:

```cpp
const auto &stats = physics_system
    .GetContactConstraintManager()
    .GetZOZOSolver()
    .GetStatistics();

printf("Newton iters: %d\n", stats.mNewtonIterations);
printf("PCG iters: %d\n", stats.mTotalPCGIterations);
printf("Beta: %.3f\n", stats.mFinalBeta);
printf("Max penetration: %.6f m\n", stats.mMaxPenetration);
```

## Architecture

```
ZOZOContactSolver
├── InnerStep()
│   ├── UpdateSemiImplicitStiffness()
│   ├── ComputeGradientAndHessian()
│   ├── SolveLinearSystem()  (PCG with block-Jacobi)
│   ├── ConstraintLineSearch()
│   └── ApplySearchStep()
└── ErrorReductionPass()
```

## Limitations (Current)

- **Rigid bodies only** - No deformable mesh support yet
- **Frictionless** - Friction planned for future release
- **Single-threaded** - Parallelization planned
- **CPU-only** - GPU acceleration planned

## Future Roadmap

### Phase 2: Friction
- Implement quadratic friction potential (Eq. 10)
- Coulomb friction model
- Friction stiffness computation

### Phase 3: Strain Limiting
- Deformable mesh support
- Strain barrier energies (Eq. 8)
- Material-aware limits

### Phase 4: GPU
- CUDA/Vulkan compute shaders
- Parallel Hessian assembly
- GPU PCG solver

## References

**Main Paper:**
- Ando, R. 2024. "A Cubic Barrier with Elasticity-Inclusive Dynamic Stiffness."
  ACM Trans. Graph. 43, 6, Article 224 (December 2024), 13 pages.
  https://doi.org/10.1145/3687908

**Related:**
- IPC (Incremental Potential Contact) - Li et al. 2020
- CIPC (Codimensional IPC) - Li et al. 2021
- Sequential Impulse Method - Catto, GDC 2009

## License

Same as Jolt Physics: MIT License

## Support

For issues specific to ZOZO solver:
- Check `ZOZO_INTEGRATION_ANALYSIS.md` for detailed analysis
- Check `ZOZO_ARCHITECTURE_DESIGN.md` for architecture details
- Report bugs to Jolt Physics fork repository

For questions about the original paper:
- Author: Ryoichi Ando (ryoichi.ando@zozo.com)
- ACM Digital Library: https://doi.org/10.1145/3687908
