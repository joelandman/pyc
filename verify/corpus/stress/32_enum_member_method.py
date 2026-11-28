# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: enum members with methods and _member_names_.
from enum import Enum


class Planet(Enum):
    MERCURY = (1.0, 360000000000000000000000000.0)
    EARTH = (2.0, 6000000000000000000000000.0)

    def __init__(self, mass, radius):
        self.mass = mass
        self.radius = radius

    @property
    def surface_gravity(self):
        return 6.673e-11 * self.mass / (self.radius ** 2)


print(Planet.EARTH.mass)
print(round(Planet.EARTH.surface_gravity, 30))
print([p.name for p in Planet])
