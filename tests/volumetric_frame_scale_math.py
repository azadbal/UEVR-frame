"""Executable design equations, not production-hook coverage. Python stdlib only."""

import math
import random
import unittest


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def mul(s, v):
    return tuple(s * x for x in v)


def rotate(v, angle):
    c, s = math.cos(angle), math.sin(angle)
    return (c * v[0] + s * v[2], v[1], -s * v[0] + c * v[2])


def baseline(p, origin, camera, units, angle):
    return add(camera, mul(units, rotate(sub(p, origin), angle)))


def scaled_eye(eye, center, origin, camera, units, angle, scale):
    pivot = baseline(center, origin, camera, units, angle)
    normal_eye = baseline(eye, origin, camera, units, angle)
    return add(pivot, mul(1 / scale, sub(normal_eye, pivot)))


def present(world, center, origin, camera, units, angle, scale):
    normal = add(origin, rotate(mul(1 / units, sub(world, camera)), -angle))
    return add(center, mul(scale, sub(normal, center)))


class ScaleMath(unittest.TestCase):
    def assertVec(self, a, b):
        for x, y in zip(a, b):
            self.assertAlmostEqual(x, y, places=8)

    def test_inverse_mapping_and_rays(self):
        rng = random.Random(198)
        for _ in range(2000):
            vec = lambda: tuple(rng.uniform(-5, 5) for _ in range(3))
            eye, center, origin, camera, point = [vec() for _ in range(5)]
            units, angle, scale = rng.uniform(1, 500), rng.uniform(-math.pi, math.pi), rng.uniform(.1, 3)
            virtual = scaled_eye(eye, center, origin, camera, units, angle, scale)
            self.assertVec(present(virtual, center, origin, camera, units, angle, scale), eye)
            physical = present(point, center, origin, camera, units, angle, scale)
            self.assertVec(sub(point, virtual), mul(units / scale, rotate(sub(physical, eye), angle)))

    def test_pivot_stays_fixed_and_multiplier_alone_fails(self):
        origin = camera = (0, 0, 0)
        center, eye, units, scale = (0, 0, -2), (0, 0, 0), 100, .5
        pivot = baseline(center, origin, camera, units, 0)
        self.assertVec(present(pivot, center, origin, camera, units, 0, scale), center)
        self.assertVec(scaled_eye(eye, center, origin, camera, units, 0, scale), (0, 0, 200))
        # Multiplier alone leaves the virtual eye at zero, missing the +200 correction.
        self.assertVec(baseline(eye, origin, camera, units / scale, 0), (0, 0, 0))

    def test_ipd_and_head_translation_use_same_inverse_scale(self):
        center, origin, camera = (0, 0, -2), (0, 0, 0), (300, 20, 0)
        units, angle, scale = 100, .7, .4
        left, right, lean = (-.032, 0, 0), (.032, 0, 0), (.15, .03, -.1)
        virtual = lambda p: scaled_eye(p, center, origin, camera, units, angle, scale)
        self.assertVec(sub(virtual(right), virtual(left)), mul(units / scale, rotate(sub(right, left), angle)))
        self.assertVec(sub(virtual(add(left, lean)), virtual(left)), mul(units / scale, rotate(lean, angle)))

    def test_portal_plane_and_dimensions(self):
        center, origin, camera, scale = (0, 0, -2), (0, 0, 0), (0, 0, 0), .5
        # Baseline 2 m x 1.125 m aperture corners and an interior point on its plane.
        for p in [(-1, -.5625, -2), (1, .5625, -2), (.3, .1, -2)]:
            world = baseline(p, origin, camera, 100, 0)
            actual = present(world, center, origin, camera, 100, 0, scale)
            self.assertVec(actual, (scale * p[0], scale * p[1], -2))
        self.assertEqual((2 * scale) / (1.125 * scale), 16 / 9)

    def test_game_locomotion_remains_live(self):
        center, origin, camera = (0, 0, -2), (0, 0, 0), (0, 0, 0)
        movement, world, units, scale = (100, 0, 0), (300, 0, -500), 100, .5
        before = present(world, center, origin, camera, units, 0, scale)
        after = present(world, center, origin, add(camera, movement), units, 0, scale)
        self.assertVec(sub(after, before), mul(-scale / units, movement))

    def test_fixed_distance_is_not_uniform_image_resize(self):
        center, origin, camera = (0, 0, -2), (0, 0, 0), (0, 0, 0)
        before = (1, 0, -4)
        after = present(mul(100, before), center, origin, camera, 100, 0, .5)
        self.assertVec(after, (.5, 0, -3))
        self.assertAlmostEqual((after[0] / -after[2]) / (before[0] / -before[2]), 2 / 3)
        self.assertNotAlmostEqual((after[0] / -after[2]) / (before[0] / -before[2]), .5)


if __name__ == "__main__":
    unittest.main()
