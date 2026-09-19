"""Standalone crop/resolve specification checks; no engine or GPU validation.

Run: python tests/volumetric_frame_crop_math.py
Tangents use OpenXR eye space (+X right, +Y up, -Z forward).
Rectangles use top-left pixels and exclusive right/bottom edges.
"""

import math
import random
import unittest


def ray(fov, u, v):
    left, right, up, down = fov
    return left + (right - left) * u, up + (down - up) * v, -1.0


def uv(fov, direction):
    left, right, up, down = fov
    x, y, z = direction
    return (x / -z - left) / (right - left), (y / -z - up) / (down - up)


def cropped_fov(fov, rect, width, height):
    x0, y0, x1, y1 = rect
    a, b = ray(fov, x0 / width, y0 / height), ray(fov, x1 / width, y1 / height)
    return a[0], b[0], a[1], b[1]


def conservative_rect(corners, fov, width, height, guard=4):
    # Deliberately conservative first prototype: near-plane crossings fall back.
    if any(not all(math.isfinite(v) for v in p) or p[2] >= -1e-5 for p in corners):
        return 0, 0, width, height
    points = [uv(fov, p) for p in corners]
    return (max(0, min(width, math.floor(min(p[0] for p in points) * width) - guard)),
            max(0, min(height, math.floor(min(p[1] for p in points) * height) - guard)),
            max(0, min(width, math.ceil(max(p[0] for p in points) * width) + guard)),
            max(0, min(height, math.ceil(max(p[1] for p in points) * height) + guard)))


def rotate(point, yaw, roll):
    x, y, z = point
    x, z = math.cos(yaw) * x + math.sin(yaw) * z, -math.sin(yaw) * x + math.cos(yaw) * z
    return math.cos(roll) * x - math.sin(roll) * y, math.sin(roll) * x + math.cos(roll) * y, z


class CropResolveMath(unittest.TestCase):
    def assertVectorClose(self, a, b):
        for actual, expected in zip(a, b):
            self.assertAlmostEqual(actual, expected, places=11)

    def test_asymmetric_ray_roundtrip(self):
        rng = random.Random(20260918)
        for _ in range(2000):
            fov = (-rng.uniform(.2, 2), rng.uniform(.2, 2), rng.uniform(.2, 2), -rng.uniform(.2, 2))
            u, v = rng.random(), rng.random()
            self.assertVectorClose(uv(fov, ray(fov, u, v)), (u, v))

    def test_pixel_centers_survive_crop_and_resolve(self):
        width, height = 2039, 1987
        fov, crop = (-1.3, .85, 1.12, -.91), (317, 241, 1628, 1394)
        narrow = cropped_fov(fov, crop, width, height)
        x0, y0, x1, y1 = crop
        for y in range(y0, y1, 19):
            for x in range(x0, x1, 17):
                baseline = ray(fov, (x + .5) / width, (y + .5) / height)
                local_uv = ((x + .5 - x0) / (x1 - x0), (y + .5 - y0) / (y1 - y0))
                self.assertVectorClose(ray(narrow, *local_uv), baseline)
                # At scene size == integer crop size, the sample is a texel center.
                self.assertVectorClose((local_uv[0] * (x1 - x0), local_uv[1] * (y1 - y0)),
                                       (x - x0 + .5, y - y0 + .5))

    def test_integer_submission_rect_and_distinct_engine_fov(self):
        # Aperture mask sees raw runtime rays in a cropped submission rectangle;
        # the game sees engine rays across the whole eye allocation.
        width, height = 2053, 1999
        submitted = (103, 81, 1946, 1877)
        engine_fov = (-1.4, 1.4, 1.2, -1.2)
        runtime_fov = (-1.1, .9, 1.0, -.85)
        aperture = [(-.5, .3, -2), (.6, .3, -2), (.6, -.4, -2), (-.5, -.4, -2)]
        sx0, sy0, sx1, sy1 = submitted
        local = conservative_rect(aperture, runtime_fov, sx1 - sx0, sy1 - sy0)
        crop = (sx0 + local[0], sy0 + local[1], sx0 + local[2], sy0 + local[3])
        narrow = cropped_fov(engine_fov, crop, width, height)
        for p in aperture:
            u, v = uv(runtime_fov, p)
            x, y = sx0 + u * (sx1 - sx0), sy0 + v * (sy1 - sy0)
            self.assertVectorClose(ray(engine_fov, x / width, y / height),
                                   ray(narrow, (x - crop[0]) / (crop[2] - crop[0]),
                                       (y - crop[1]) / (crop[3] - crop[1])))

    def test_oblique_aperture_bounds_enclose_interior_for_both_eyes(self):
        width, height, fov = 1920, 1800, (-1.1, .95, 1.0, -.9)
        for eye_x in (-.032, .032):
            for yaw, roll in ((0, 0), (.8, .45), (-.6, -.8)):
                def eye_point(x, y):
                    a, b, c = rotate((x, y, 0), yaw, roll)
                    return a - eye_x + .17, b + .13, c - 1.8
                corners = [eye_point(x, y) for x, y in ((-1, -.5625), (1, -.5625), (1, .5625), (-1, .5625))]
                x0, y0, x1, y1 = conservative_rect(corners, fov, width, height)
                for ix in range(41):
                    for iy in range(23):
                        u, v = uv(fov, eye_point(-1 + ix / 20, -.5625 + iy * 1.125 / 22))
                        if 0 <= u <= 1 and 0 <= v <= 1:
                            self.assertTrue(x0 <= u * width <= x1)
                            self.assertTrue(y0 <= v * height <= y1)

    def test_near_crossing_and_invalid_data_fall_back(self):
        fov = (-1, 1, 1, -1)
        for corners in (((-1, 1, -2), (1, 1, 0)), ((0, 0, float('nan')),), ((0, 0, 1),)):
            self.assertEqual(conservative_rect(corners, fov, 100, 80), (0, 0, 100, 80))
        self.assertEqual(conservative_rect(((3, 0, -1), (4, .1, -1)), fov, 100, 80)[:3:2], (100, 100))

    def test_clip_space_crop_preserves_depth(self):
        # Column-vector algebra: apply C to clip coordinates after projection.
        # Only x/y change; homogeneous z/w (including reversed-Z) is untouched.
        u0, v0, u1, v1 = .13, .24, .81, .92
        for x, y, z, w in ((.2, -.7, .01, 2), (-.4, .8, .1, 10), (.1, .1, .2, .3)):
            cropped = ((x + (1 - u0 - u1) * w) / (u1 - u0),
                       (y + (v0 + v1 - 1) * w) / (v1 - v0), z, w)
            self.assertEqual(cropped[2] / cropped[3], z / w)
            before_uv = ((x / w + 1) / 2, (1 - y / w) / 2)
            after_uv = ((cropped[0] / w + 1) / 2, (1 - cropped[1] / w) / 2)
            self.assertVectorClose(after_uv, ((before_uv[0] - u0) / (u1 - u0), (before_uv[1] - v0) / (v1 - v0)))


if __name__ == '__main__':
    unittest.main(verbosity=2)
