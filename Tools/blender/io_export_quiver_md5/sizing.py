# SPDX-License-Identifier: GPL-2.0-or-later
"""Choose square bake sizes that hold texel density steady across surfaces.

Pure Python so it can be tested without Blender. A bake spends its pixels on the
part of the tile that UVs cover, so density needs both areas:

    size² × uv_coverage = density² × world_area

``world_area`` is in game units², after object transforms and the export scale.
"""

from dataclasses import dataclass
import math

from .model import ExportError

MIN_BAKE_SIZE = 32
MAX_BAKE_SIZE = 4096
DEFAULT_DENSITY = 4.0

# Where the search for a generated layout's size starts. Only a starting point:
# measured Smart Project coverage runs from under 1% on dense surfaces to over 90%.
GENERATED_UV_COVERAGE = 0.5


@dataclass(frozen=True)
class SizePolicy:
    mode: str = 'AUTO'
    fixed_size: int = 2048
    density: float = DEFAULT_DENSITY
    minimum: int = 64
    maximum: int = 2048

    def __post_init__(self):
        if self.mode not in {'AUTO', 'FIXED'}:
            raise ExportError("Bake Size must be AUTO or FIXED")
        _check_size(self.fixed_size, "Bake Resolution")
        _check_size(self.minimum, "Min Bake Size")
        _check_size(self.maximum, "Max Bake Size")
        if isinstance(self.density, bool) or not math.isfinite(self.density) or self.density <= 0:
            raise ExportError("Texel Density must be finite and greater than zero")
        if self.minimum > self.maximum:
            raise ExportError("Min Bake Size cannot exceed Max Bake Size")


@dataclass(frozen=True)
class SizeChoice:
    size: int
    ideal: float | None = None  # Unrounded size; None when a person chose it.
    limit: str | None = None    # 'MIN' or 'MAX' when a bound overrode the ideal.
    source: str = 'AUTO'        # 'AUTO', 'FIXED', or 'MATERIAL'.


def _check_size(value, label):
    if isinstance(value, bool) or not isinstance(value, int) or not MIN_BAKE_SIZE <= value <= MAX_BAKE_SIZE:
        raise ExportError(f"{label} must be an integer between {MIN_BAKE_SIZE} and {MAX_BAKE_SIZE}")


def nearest_power_of_two(value):
    """Round in log space, keeping density within a factor of √2 of the target."""
    if not math.isfinite(value) or value <= 0:
        raise ValueError("Size must be finite and positive")
    return 1 << max(0, round(math.log2(value)))


def _power_bounds(minimum, maximum):
    """Largest power-of-two range inside the limits, or the nearest one if none fits."""
    low = 1 << (minimum - 1).bit_length()
    high = 1 << (maximum.bit_length() - 1)
    return (low, high) if low <= high else (high, high)


def choose_size(policy, world_area, uv_coverage, override=0):
    """Return the bake size. A nonzero material ``override`` always wins."""
    if override:
        _check_size(override, "Material Bake Size")
        return SizeChoice(override, source='MATERIAL')
    if policy.mode == 'FIXED':
        return SizeChoice(policy.fixed_size, source='FIXED')
    if not math.isfinite(world_area) or world_area < 0:
        raise ExportError("Surface area must be finite")
    if not math.isfinite(uv_coverage) or uv_coverage < 0:
        raise ExportError("UV coverage must be finite")
    low, high = _power_bounds(policy.minimum, policy.maximum)
    if world_area == 0 or uv_coverage == 0:
        return SizeChoice(low, 0.0, 'MIN')
    ideal = policy.density * math.sqrt(world_area / min(1.0, uv_coverage))
    rounded = nearest_power_of_two(ideal)
    limit = 'MIN' if rounded < low else 'MAX' if rounded > high else None
    return SizeChoice(min(high, max(low, rounded)), ideal, limit)


def settle_size(policy, world_area, coverage_at, expected_coverage=GENERATED_UV_COVERAGE):
    """Size a generated layout, whose coverage depends on the size it is built for.

    Island margins are a fixed number of pixels, so coverage rises with size and
    delivered density with it. Return the smallest power of two within √2 of the
    target, matching ``choose_size``. Each new size passed to ``coverage_at`` is
    a real UV projection, so jump once to where the first measurement points,
    then bisect what remains rather than walking every size.
    """
    low, high = _power_bounds(policy.minimum, policy.maximum)
    if world_area <= 0:
        return SizeChoice(low, 0.0, 'MIN')
    too_small, enough = low // 2, None  # Largest failing and smallest passing sizes.
    size = choose_size(policy, world_area, expected_coverage).size
    first = True
    while True:
        coverage = coverage_at(size)
        if achieved_density(size, world_area, coverage) * math.sqrt(2) >= policy.density:
            enough = size
        else:
            too_small = size
        ceiling = enough if enough is not None else high * 2
        if ceiling <= too_small * 2:
            break
        lowest, highest = too_small * 2, ceiling // 2
        if first and coverage > 0:
            size = min(highest, max(lowest, choose_size(policy, world_area, coverage).size))
        else:
            size = 1 << ((lowest.bit_length() + highest.bit_length()) // 2 - 1)
        first = False
    size = high if enough is None else enough
    coverage = coverage_at(size)
    ideal = policy.density * math.sqrt(world_area / coverage) if coverage > 0 else math.inf
    limit = 'MAX' if enough is None else 'MIN' if size == low and ideal < low / math.sqrt(2) else None
    return SizeChoice(size, ideal, limit)


def achieved_density(size, world_area, uv_coverage):
    """Texels per game unit that a size delivers; zero for an empty surface."""
    if world_area <= 0 or uv_coverage <= 0:
        return 0.0
    return size * math.sqrt(min(1.0, uv_coverage) / world_area)
