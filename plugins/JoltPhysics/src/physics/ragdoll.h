// Death ragdolls on Jolt Physics 5.6.0 (third_party/JoltPhysics, MIT): milestone 1 of the port of SA's "[SA] Jolt
// Physics" mod (docs/game-internals.md 12f, 12h; the game side is game/vcs_jolt.cpp).
//
// A ragdoll is built from a ped's 25 bone world matrices as the game keeps them (RslTAnimTree +0x28, VC's skeleton:
// 0 root, 1 pelvis, 2-3 spine, 4 neck, 5-6 head, 7-11 left arm, 12-16 right arm, 17-20 left leg, 21-24 right leg) and
// its velocity: 11 bodies - hips (bone 2), chest (3), head (5), upper arms (8, 13), forearms (9, 14), thighs (17, 21),
// calves (18, 22) - with swing-twist joints at the waist, neck, shoulders and hips and hinges at the elbows and knees,
// in the pose the ped had. Its pose goes back as the 25 matrices: the bodies' bones from the bodies (a SkeletonMapper),
// the others (root, pelvis, neck, clavicles, hands, feet) carried rigidly with their parent as they were at death.
// The ragdolls land on and hit the world the game side puts in (milestone 2: the game's col models near them as static
// or moving bodies, vehicles as boxes); where it has nothing under a ped, a ground plane of the ragdoll's own.
//
// This library is the only code that sees Jolt (its own flags and defines stay here: src/physics/CMakeLists.txt). All
// calls come from the game thread. Jolt runs with round-to-nearest: every call sets it and gives the caller's MXCSR back
// (the VU paths round toward zero).
#pragma once

#include <cstdint>

namespace vcs::physics
{
    constexpr int kPedBones = 25;

    // A bone's world matrix as the game keeps it: rows X, Y, Z, position - 4 floats each, the w lanes left alone.
    struct BoneMatrix
    {
        float m[16];
    };

    struct RagdollDesc
    {
        const BoneMatrix *bones = nullptr; // the ped's 25 world matrices (its last draw)
        const int *parents = nullptr;      // their hierarchy (from the tree's node flags; -1: the root)
        float velocity[3] = {};            // m/s, every part
        bool active = false;               // its hips moved by drive() (kinematic), not thrown: a hit reaction
        bool groundPlane = true;           // a plane of its own to land on (the world has nothing under it) ...
        float groundZ = 0.0f;              // ... at this height
    };

    struct Stats
    {
        int ragdolls = 0, shapes = 0, props = 0;
        unsigned bodies = 0, activeBodies = 0;
        float stepMs = 0.0f; // the last step
    };

    // The physics system (Jolt's allocator, factory and types on the first call). False: it could not be made.
    bool init();

    // A ragdoll for this pose; 0 when refused (not the ped skeleton this was written for, no room in the system).
    int create(const RagdollDesc &desc);

    // The ragdoll and its ground out of the world.
    void destroy(int id);

    // The ragdoll's parts, for push(): the hips, chest, head, upper arms, forearms, thighs, calves.
    enum Part
    {
        kHips, kChest, kHead, kUpperArmL, kForearmL, kUpperArmR, kForearmR, kThighL, kCalfL, kThighR, kCalfR, kPartTotal
    };

    // A blow on one part (kg m/s, world space): a shot, a hit.
    void push(int id, int part, const float impulse[3]);

    // One part's speed changed by `dv` (m/s, world space), whatever its mass: a vehicle's bumper on the legs.
    void kick(int id, int part, const float dv[3]);

    // A bullet from `from` to `to`: the first ragdoll part on its line takes `push` (kg m/s) along it, at the point the
    // line meets it (it turns the body too) - unless the static world in the physics (a wall) is in the way first. That
    // ragdoll's id; 0 when the line meets none.
    int shoot(const float from[3], const float to[3], float push, int *part = nullptr); // part: the one it met (Part)

    // A limb (or the head) taken off: that part and the ones below it no longer meet anything (they hang on, unseen:
    // the game side draws the stump).
    void sever(int id, int part);

    // A blast at `centre`: each part within `radius` thrown away from it (and up) at up to `speed` m/s, less the farther
    // it is - the parts nearest the blast fastest, so the body tumbles.
    void blast(int id, const float centre[3], float radius, float speed);

    // A moving obstacle the ragdolls hit and are hit by (a vehicle): kinematic - nothing pushes it back -, the box given
    // by its min / max corners in its own frame (`transform`: rows X, Y, Z, position; Y ahead, Z up). car: not one block
    // but a car's - its lower body the box's whole length up to the bonnet, the cabin set back from the front - so a
    // body it runs into folds onto the bonnet. 0 when refused.
    int addObstacle(const BoneMatrix &transform, const float boxMin[3], const float boxMax[3], bool car);

    // Where it is now; the next step takes it there over `seconds` (its speed for the contacts).
    void moveObstacle(int id, const BoneMatrix &transform, float seconds);

    // Obstacles and world bodies (addWorld) alike.
    void removeObstacle(int id);

    // The world's collision: one col model's primitives in its model's frame - triangles (three indices into `vertices`,
    // xyz each, counter-clockwise seen from the side they face), boxes (min xyz, max xyz) and spheres (centre xyz,
    // radius) - as one shape, kept until removeShape (bodies made with it keep it). 0: nothing usable in it.
    struct ColShape
    {
        const float *vertices = nullptr;
        int vertexCount = 0;
        const int *triangles = nullptr;
        int triangleCount = 0;
        const float *boxes = nullptr;   // 6 floats each
        int boxCount = 0;
        const float *spheres = nullptr; // 4 floats each
        int sphereCount = 0;
    };
    int addShape(const ColShape &col);
    void removeShape(int shape);

    // A body of the world with that shape where `transform` puts it (rows X, Y, Z, position): static (a building) or,
    // moving, kinematic - moveObstacle takes it along (an object). 0 when refused.
    int addWorld(int shape, const BoneMatrix &transform, bool moving);

    // The height of the world's surface under `from` (the world's bodies and the obstacles, not ragdolls or their own
    // grounds) within `depth` below it; false when there is none.
    bool ground(const float from[3], float depth, float &z);

    // Whether the world's bodies or the obstacles (not ragdolls or their own grounds) are in the way from `from` to `to`:
    // how far along it (0..1) the first is, 1 when nothing is.
    float clear(const float from[3], const float to[3]);

    // One step of `seconds` (collision steps of at most 1/60 s).
    void step(float seconds);

    // The ragdoll's pose as the ped's 25 world matrices (rows X, Y, Z and position; out[i].m's w lanes are 0).
    bool pose(int id, BoneMatrix *out);

    // Muscles: every joint pulled toward this pose (the ped's 25 bones as the animation has them) with `strength` (0 limp,
    // 1 a standing ped's tone); an active ragdoll's hips are taken there over `seconds` as well.
    void drive(int id, const BoneMatrix *bones, float seconds, float strength);

    // Catching a fall: both arms' muscles reach along `dir` (world space: toward the ground the body falls to), elbows
    // straight, with `strength`; 0 lets them go limp again.
    void brace(int id, const float dir[3], float strength);

    // True while any of its bodies moves (Jolt puts settled ones to sleep).
    bool awake(int id);

    // A loose prop (a dropped gun, a wad of cash): a box `half` (half extents) around `centre` in its model's frame,
    // `mass` kg, placed by `transform` (rows X, Y, Z, position; scale taken out), thrown at `velocity` (m/s) spinning
    // at `spin` (rad/s, world axes). It meets the world, vehicles and ragdolls. 0 when refused.
    int addProp(const BoneMatrix &transform, const float centre[3], const float half[3], float mass, const float velocity[3],
                const float spin[3]);
    // Its model's matrix now (rows X, Y, Z, position; w lanes 0); false when there is no such prop.
    bool propPose(int id, BoneMatrix &out);
    bool propAwake(int id);
    void removeProp(int id);

    // `count` bone matrices part of the way (t: 0 `from`, 1 `to`) from one pose to another: positions along a line,
    // rotations along the shortest arc - a ragdoll's last pose eased into the game's animation (a get-up).
    void blendPose(const BoneMatrix *from, const BoneMatrix *to, float t, BoneMatrix *out, int count);

    Stats stats();
}
