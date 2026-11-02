#include "planet_curvature.h"
#include <iostream>
#include <stdexcept>

using namespace quiver;
static void Check (bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error (message);
}
int main ()
{
	PlanetCurvature bend;
	for (double d : {0.0, 100.0, 4096.0})
		Check (bend.Sag (d)[0] == 0 && bend.Sag (d)[1] == 0, "near play area remains flat");
	for (double d : {5000.0, 8192.0, 100000.0, 16777216.0, bend.radius, bend.radius * 2})
	{
		const auto	 sag = bend.Sag (d);
		const double numericalSlope = (bend.Sag (d + 0.1)[0] - bend.Sag (d - 0.1)[0]) / 0.2;
		Check (std::abs (sag[1] - numericalSlope) < 1e-5, "analytical slope agrees with displacement");
		const std::array<double, 3> original{d, d * 0.25, 300};
		const auto					reconstructed = bend.Inverse (bend.Forward (original));
		Check (std::abs (reconstructed[2] - original[2]) < 1e-6, "depth and targeting inverse reconstruct world position");
		const auto normal = bend.Normal ({d, 0, 0}, {0, 0, 1});
		Check (std::abs (normal[0] - normal[2] * sag[1]) < 1e-12, "normal matches bent surface tangent");
	}
	// Reconstruct camera-relative XY using only clip(x,y,w), including an
	// arbitrary camera rotation and perspective scale. This is the transport
	// used by every world/alias/particle vertex shader under the 128-byte limit.
	for (double angle : {-2.3, -0.4, 0.0, 1.7})
		for (double pitch : {-1.1, 0.0, 0.6})
		{
			const double c = std::cos (angle), s = std::sin (angle), cp = std::cos (pitch), sp = std::sin (pitch);
			const double rotation[3][3] = {{c, -s, 0}, {cp * s, cp * c, -sp}, {sp * s, sp * c, cp}};
			for (const std::array<double, 3> p : {std::array<double, 3>{3000, 4000, 100}, {100000, -80000, -3000}})
			{
				std::array<double, 3> view{};
				for (int row = 0; row < 3; ++row)
					for (int col = 0; col < 3; ++col)
						view[row] += rotation[row][col] * p[col];
				const std::array<double, 3> clip{view[0] * 1.5, view[1] * -2.0, -view[2]};
				for (int axis = 0; axis < 2; ++axis)
				{
					const double recovered = clip[0] * rotation[0][axis] / 1.5 + clip[1] * rotation[1][axis] / -2.0 - clip[2] * rotation[2][axis];
					Check (std::abs (recovered - p[axis]) < 1e-8, "clip-space bend transport matches arbitrary camera rotation");
				}
			}
		}
	std::array<double, 3> low{-80000, -60000, -500}, high{90000, 120000, 1500};
	bend.Bounds (low, high);
	for (int y = -60000; y <= 120000; y += 3000)
		for (int x = -80000; x <= 90000; x += 2000)
			for (double z : {-500.0, 1500.0})
			{
				const auto warped = bend.Forward ({double (x), double (y), z});
				Check (warped[2] >= low[2] && warped[2] <= high[2], "warped culling bound contains entire box");
			}
	std::cout << "planet curvature mathematics passed\n";
}
