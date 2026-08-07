# Kraken multiplayer networking

This directory contains the engine-independent networking surface. Code here
must not call ODE (`dBody*`, `dWorld*`) or Jolt APIs directly; gameplay state is
read and applied through the HTA `PhysicObj` interface by the replication layer.
