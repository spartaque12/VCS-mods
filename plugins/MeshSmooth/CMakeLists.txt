# MeshSmooth.asi: smoother peds and vehicles - lit model batches refined on the host (PN triangles with crack-free
# shared normals), cached per batch, drawn in the original's place through the mod API's lit refiner.
vcs_add_mod(MeshSmooth
    src/main.cpp
    src/meshsmooth.cpp
)
