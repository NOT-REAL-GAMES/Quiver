# Projectile and persistent model geometry

The first static-MD5 caster build put Sponza into the same dynamic BVH/BLAS as
projectiles. Adding or removing a four-triangle nail rebuilt all 262,266 model
triangles. A first new projectile material also decoded and repacked Sponza's
large image inventory. Sustained nail fire reproduced a severe slowdown.

Large supported single-frame MD5 previews now use a persistent base partition,
alongside the BSP, while small moving models remain in the mutable suffix.
CPU tracing joins the independently built trees without sorting the base on
projectile changes. Hardware tracing keeps the base BLAS and updates/builds
only the small suffix plus TLAS. Transforms, selection, topology and material
changes still invalidate the appropriate partition. No nail or beam caster
was silently removed.

Compiled editor models enter the immutable world prefix directly, outside
the dynamic selection cap. Their material bindings use frame 0. Persistent
emitter dependencies avoid scanning a large unchanged base when a projectile
adds or retires an unrelated material. Atlas repacks reuse unchanged decoded
pixels from their existing rectangles rather than reopening every Sponza PNG;
full-width rectangles can be borrowed until the transactional pack completes.

The actual Sponza/DM2 normal-clock diagnostic used quality 0, two-thirds texture
receivers, soft shadows off and a 30-FPS test cap. Original warm nail console
samples measured 132–151 ms CPU preparation; the fixed build's sampled warm
median was 6.67 ms. The first nail changed from about 1,721 ms to 398 ms, and first
lightning still incurred about 234 ms before settling around 2.5 ms. Those first-use
material repack/upload hitches remain; this is not an uncapped GPU/FPS benchmark.
The original baseline memory sampler had incorrectly typed CPU fields as
float32; its raw CPU arrays are invalid. The quoted baseline numbers come from
valid console reports, while the corrected sampler reads 64-bit doubles.

The persistent base stayed at 277,112 triangles during actual nail/lightning
fire, with no base builds. Nails used roughly 180–204 mutable triangles and
lightning at most 670, instead of a dynamic BLAS containing Sponza. The
published weapon-fix executable is preserved under
`build/radiance-nailgun-regression-20261105/published4f/`; its software-backend
actual-caster check also passed SDK synchronization validation with clean exit.
