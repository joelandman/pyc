# N-Body benchmark from Python benchmark suite
# Simulates gravitational interaction between bodies

def main():
    bodies = []
    n = 50
    
    # Initialize bodies
    i = 0
    while i < n:
        x = (i % 5) * 2.0 - 5.0
        y = (i % 3) * 2.0 - 3.0
        z = (i // 3) * 2.0 - 3.0
        vx = 0.0
        vy = 0.0
        vz = 0.0
        mass = 1.0
        bodies.append([x, y, z, vx, vy, vz, mass])
        i = i + 1
    
    # Compute center of mass momentum (should be ~0)
    px = 0.0
    py = 0.0
    pz = 0.0
    i = 0
    while i < n:
        px = px + bodies[i][3] * bodies[i][6]
        py = py + bodies[i][4] * bodies[i][6]
        pz = pz + bodies[i][5] * bodies[i][6]
        i = i + 1
    print("COM momentum: " + str(px) + " " + str(py) + " " + str(pz))
    
    # Advance simulation for 100 steps
    steps = 100
    dt = 0.01
    step = 0
    while step < steps:
        # Compute accelerations
        i = 0
        while i < n:
            ax = 0.0
            ay = 0.0
            az = 0.0
            j = 0
            while j < n:
                if i != j:
                    dx = bodies[j][0] - bodies[i][0]
                    dy = bodies[j][1] - bodies[i][1]
                    dz = bodies[j][2] - bodies[i][2]
                    distSq = dx * dx + dy * dy + dz * dz
                    softening = 0.1
                    dist3 = distSq * distSq * distSq
                    if dist3 > 0.0001:
                        invDist3 = 1.0 / dist3
                        fx = dx * invDist3
                        fy = dy * invDist3
                        fz = dz * invDist3
                        ax = ax + bodies[j][6] * fx
                        ay = ay + bodies[j][6] * fy
                        az = az + bodies[j][6] * fz
                j = j + 1
            bodies[i][3] = bodies[i][3] + ax * dt
            bodies[i][4] = bodies[i][4] + ay * dt
            bodies[i][5] = bodies[i][5] + az * dt
            i = i + 1
        
        # Update positions
        i = 0
        while i < n:
            bodies[i][0] = bodies[i][0] + bodies[i][3] * dt
            bodies[i][1] = bodies[i][1] + bodies[i][4] * dt
            bodies[i][2] = bodies[i][2] + bodies[i][5] * dt
            i = i + 1
        
        step = step + 1
    
    # Compute total energy
    kinetic = 0.0
    potential = 0.0
    i = 0
    while i < n:
        kinetic = kinetic + 0.5 * bodies[i][6] * (bodies[i][3] * bodies[i][3] + bodies[i][4] * bodies[i][4] + bodies[i][5] * bodies[i][5])
        j = i + 1
        while j < n:
            dx = bodies[j][0] - bodies[i][0]
            dy = bodies[j][1] - bodies[i][1]
            dz = bodies[j][2] - bodies[i][2]
            dist = dx * dx + dy * dy + dz * dz
            if dist > 0.0001:
                potential = potential - (bodies[i][6] * bodies[j][6]) / dist
            j = j + 1
        i = i + 1
    
    energy = kinetic + potential
    print("Energy: " + str(energy))
    print("Steps: " + str(steps))
