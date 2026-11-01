// See ragdoll.h; the skeleton's conventions are in docs/game-internals.md 12h.
#include "physics/ragdoll.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonMapper.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#else
#include <cfenv>
#endif

namespace vcs::physics
{
    namespace
    {
        using JPH::Mat44;
        using JPH::Quat;
        using JPH::RVec3;
        using JPH::Vec3;
        using JPH::Vec4;

        // Jolt with round-to-nearest; the caller's mode (the VU's toward zero, in MXCSR bits 13-14) given back after.
        class NearestRounding
        {
        public:
#if defined(__x86_64__) || defined(_M_X64)
            NearestRounding() : m_csr(_mm_getcsr()) { _mm_setcsr(m_csr & ~0x6000u); }
            ~NearestRounding() { _mm_setcsr(m_csr); }

        private:
            unsigned m_csr;
#else
            NearestRounding() : m_mode(std::fegetround()) { std::fesetround(FE_TONEAREST); }
            ~NearestRounding() { std::fesetround(m_mode); }

        private:
            int m_mode;
#endif
        };

        constexpr float kPi = 3.14159265f;
        constexpr float deg(float d) { return d * kPi / 180.0f; }

        // VC's ped skeleton as its anim tree's node flags give it (game-internals.md 12h)
        constexpr int kPedParents[kPedBones] = {-1, 0, 1, 2, 3, 4, 5, 4, 7, 8, 9, 10, 4, 12, 13, 14, 15, 2, 17, 18, 19,
                                                2, 21, 22, 23};
        constexpr int kHipsBone = 2, kRootBone = 0;

        // The bodies. Bone frames: the torso's X up, Y forward, Z left; a limb's X along it (to its child), and the
        // elbows and knees bend about their Z, negative (checked on walking and running poses). A swing-twist joint's
        // rest is the child's X (twist) and Z (plane) at `twist` / `plane` in the parent part's bone frame: the arms
        // hang out 40 degrees from the sides, the thighs forward 50 degrees - the middle of their range, as Jolt's cone
        // limits are symmetric. planeHalf limits the turn about the plane axis (Z: forward / back for the limbs and the
        // spine), normalHalf the turn about twist x plane (sideways).
        enum class Shape { Across, Along, Fixed };
        enum class Joint { Root, Swing, Hinge };
        struct PartDef
        {
            const char *name;
            int bone, parent;          // the bone it moves; its parent part
            Shape shape;
            int tip;                   // Along: the bone its capsule reaches to
            float radius, extra;       // Along: past the tip (a hand); Across / Fixed: half the cylinder
            float centre[3];           // Across / Fixed: the capsule's middle in the bone's frame
            float mass;                // kg
            Joint joint;
            float twist[3], plane[3];  // Swing: the rest in the parent part's bone frame
            float twistLimit, planeHalf, normalHalf; // degrees
            float friction;            // N m
            float muscle;              // N m at full strength (drive)
        };
        constexpr float kC40 = 0.76604444f, kS40 = 0.64278761f, kC50 = 0.64278761f, kS50 = 0.76604444f;
        constexpr PartDef kParts[] = {
            {"hips", 2, -1, Shape::Across, -1, 0.13f, 0.06f, {-0.04f, 0.0f, 0.0f}, 16.0f, Joint::Root, {}, {}, 0, 0, 0, 0, 0},
            {"chest", 3, 0, Shape::Across, -1, 0.12f, 0.07f, {0.13f, 0.0f, 0.0f}, 16.0f, Joint::Swing, {1, 0, 0}, {0, 0, 1},
             12, 30, 20, 6.0f, 400.0f},
            {"head", 5, 1, Shape::Fixed, -1, 0.095f, 0.025f, {0.10f, 0.02f, 0.0f}, 5.0f, Joint::Swing, {1, 0, 0}, {0, 0, 1},
             50, 40, 30, 2.0f, 60.0f},
            {"upperarm_l", 8, 1, Shape::Along, 9, 0.05f, 0.0f, {}, 2.5f, Joint::Swing, {-kC40, 0, kS40}, {kS40, 0, kC40},
             60, 80, 70, 2.0f, 120.0f},
            {"forearm_l", 9, 3, Shape::Along, 10, 0.045f, 0.10f, {}, 2.0f, Joint::Hinge, {}, {}, 0, 0, 0, 1.0f, 60.0f},
            {"upperarm_r", 13, 1, Shape::Along, 14, 0.05f, 0.0f, {}, 2.5f, Joint::Swing, {-kC40, 0, -kS40}, {-kS40, 0, kC40},
             60, 80, 70, 2.0f, 120.0f},
            {"forearm_r", 14, 5, Shape::Along, 15, 0.045f, 0.10f, {}, 2.0f, Joint::Hinge, {}, {}, 0, 0, 0, 1.0f, 60.0f},
            {"thigh_l", 17, 0, Shape::Along, 18, 0.075f, 0.0f, {}, 9.0f, Joint::Swing, {-kC50, kS50, 0}, {0, 0, -1},
             25, 75, 40, 4.0f, 400.0f},
            {"calf_l", 18, 7, Shape::Along, 19, 0.06f, 0.02f, {}, 5.0f, Joint::Hinge, {}, {}, 0, 0, 0, 2.0f, 250.0f},
            {"thigh_r", 21, 0, Shape::Along, 22, 0.075f, 0.0f, {}, 9.0f, Joint::Swing, {-kC50, kS50, 0}, {0, 0, -1},
             25, 75, 40, 4.0f, 400.0f},
            {"calf_r", 22, 9, Shape::Along, 23, 0.06f, 0.02f, {}, 5.0f, Joint::Hinge, {}, {}, 0, 0, 0, 2.0f, 250.0f},
        };
        constexpr int kPartCount = int(sizeof(kParts) / sizeof(kParts[0]));
        static_assert(kPartCount == kPartTotal, "ragdoll.h's Part names the parts in kParts' order");
        constexpr float kHingeMin = deg(-150.0f), kHingeMax = deg(3.0f); // elbows and knees: bent, a little past straight
        constexpr float kFriction = 0.75f, kRestitution = 0.1f;         // SA's GroundFriction / BodyBounciness
        constexpr float kLinearDamping = 0.1f, kAngularDamping = 0.6f;
        constexpr float kMaxSpeed = 30.0f;                              // m/s: a seed above it is cut back

        constexpr JPH::ObjectLayer kStatic = 0, kMoving = 1;
        constexpr unsigned kMaxBodies = 2048, kMaxBodyPairs = 8192, kMaxContacts = 4096;
        constexpr unsigned kWorldBodies = kMaxBodies - 400; // the world's at most: room left for the ragdolls and vehicles
        constexpr JPH::CollisionGroup::SubGroupID kGroundSubGroup = 0x7FFF;

        class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
        {
        public:
            JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
            JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
            {
                return JPH::BroadPhaseLayer(layer == kMoving ? 1 : 0);
            }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
            const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
            {
                return layer.GetValue() ? "moving" : "static";
            }
#endif
        };
        class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter
        {
        public:
            bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override
            {
                return layer == kMoving || bp.GetValue() == 1;
            }
        };
        class ObjectPairs final : public JPH::ObjectLayerPairFilter
        {
        public:
            bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a == kMoving || b == kMoving; }
        };

        // A ragdoll's parts collide with other ragdolls, the world and its own ground, never with another one's ground
        // (those sit at each body's own height); among themselves as RagdollSettings decided (not parent and child, not
        // parts touching in the pose they started in). The world's bodies have no group: they meet every part.
        class DollFilter final : public JPH::GroupFilter
        {
        public:
            explicit DollFilter(const JPH::GroupFilter *parts) : m_parts(parts) {}
            bool CanCollide(const JPH::CollisionGroup &a, const JPH::CollisionGroup &b) const override
            {
                const bool groundA = a.GetSubGroupID() == kGroundSubGroup, groundB = b.GetSubGroupID() == kGroundSubGroup;
                if (a.GetGroupID() != b.GetGroupID())
                    return !groundA && !groundB;
                if (groundA || groundB)
                    return true;
                return m_parts->CanCollide(a, b);
            }

        private:
            JPH::RefConst<JPH::GroupFilter> m_parts;
        };

        struct Doll
        {
            JPH::Ref<JPH::Ragdoll> ragdoll;
            JPH::Ref<JPH::SkeletonMapper> mapper; // ragdoll parts -> the ped's 25 bones
            JPH::Ref<JPH::Skeleton> skeleton;     // the ped's
            JPH::BodyID ground;                   // its own ground plane (none where the world has a surface)
            Mat44 local[kPedBones];               // each bone in its parent's frame at the start (the root: model space)
            Mat44 hipsToBone, rootFromHips;       // the hips part -> its bone; the root bone in the hips bone's frame
            Mat44 partToBone[kPartTotal];         // each part -> its bone (the mapper's)
            bool detached[kPartTotal] = {};       // let go of the body (detach): drawn where its body is
        };

        // Everything is made once and kept to the end of the process (no teardown order at exit).
        BroadPhaseLayers g_broadPhaseLayers;
        ObjectVsBroadPhase g_objectVsBroadPhase;
        ObjectPairs g_objectPairs;
        JPH::PhysicsSystem *g_system = nullptr;
        JPH::TempAllocatorImpl *g_temp = nullptr;
        JPH::JobSystemSingleThreaded *g_jobs = nullptr;
        JPH::Skeleton *g_ragdollSkeleton = nullptr;
        std::unordered_map<int, Doll> *g_dolls = nullptr;
        std::unordered_map<int, JPH::BodyID> *g_obstacles = nullptr; // vehicles and the world's bodies
        std::unordered_map<int, JPH::Ref<JPH::Shape>> *g_shapes = nullptr; // the world's col models
        std::unordered_map<int, JPH::BodyID> *g_props = nullptr;            // loose guns and cash
        int g_nextId = 1;
        float g_stepMs = 0.0f;
        bool g_failed = false;

        void trace(const char *format, ...)
        {
            char line[512];
            va_list args;
            va_start(args, format);
            std::vsnprintf(line, sizeof(line), format, args);
            va_end(args);
            std::fprintf(stderr, "[jolt] %s\n", line);
        }

        bool finite(const float *v, int n)
        {
            for (int i = 0; i < n; ++i)
                if (!std::isfinite(v[i]))
                    return false;
            return true;
        }

        Vec3 row(const BoneMatrix &b, int r) { return Vec3(b.m[r * 4], b.m[r * 4 + 1], b.m[r * 4 + 2]); }

        // The bones' rotations made orthonormal (X kept, Z = X x Y, Y = Z x X); a bone without usable axes (first person
        // collapses the player's head to a point) takes its parent's.
        // A matrix of the game's made orthonormal (X kept, Z = X x Y, Y = Z x X); without usable axes, `fallback`'s
        // rotation at its position.
        Mat44 orthonormal(const BoneMatrix &b, const Mat44 &fallback)
        {
            Vec3 x = row(b, 0), y = row(b, 1);
            const Vec3 p = row(b, 3);
            if (x.Length() < 0.5f || y.Length() < 0.5f || x.Cross(y).Length() < 0.25f)
            {
                Mat44 m = fallback;
                m.SetTranslation(p);
                return m;
            }
            x = x.Normalized();
            const Vec3 z = x.Cross(y).Normalized();
            y = z.Cross(x);
            return Mat44(Vec4(x, 0.0f), Vec4(y, 0.0f), Vec4(z, 0.0f), Vec4(p, 1.0f));
        }

        // The bones' frames; a bone without usable axes (first person collapses the player's head to a point) takes its
        // parent's rotation.
        void boneFrames(const BoneMatrix *bones, Mat44 *out)
        {
            for (int i = 0; i < kPedBones; ++i)
                out[i] = orthonormal(bones[i], kPedParents[i] >= 0 ? out[kPedParents[i]] : Mat44::sIdentity());
        }

        // A capsule from `a` to `b` in a body's frame.
        JPH::Ref<JPH::Shape> segment(Vec3 a, Vec3 b, float radius)
        {
            Vec3 d = b - a;
            const float length = d.Length();
            d = length > 1e-4f ? d / length : Vec3::sAxisX();
            const float half = std::max(0.01f, 0.5f * length - radius);
            return new JPH::RotatedTranslatedShape(0.5f * (a + b), Quat::sFromTo(Vec3::sAxisY(), d),
                                                   new JPH::CapsuleShape(half, radius));
        }

        JPH::Ref<JPH::Shape> partShape(const PartDef &def, const Mat44 *frames)
        {
            const Mat44 &f = frames[def.bone];
            const Vec3 c(def.centre[0], def.centre[1], def.centre[2]);
            const float reach = def.extra + def.radius; // Across / Fixed: the cylinder's half and its cap
            switch (def.shape)
            {
            case Shape::Across: // a capsule across the torso (its Z)
                return segment(c - Vec3(0, 0, reach), c + Vec3(0, 0, reach), def.radius);
            case Shape::Fixed: // the head: up its X
                return segment(c - Vec3(reach, 0, 0), c + Vec3(reach, 0, 0), def.radius);
            case Shape::Along:
                break;
            }
            // a limb: from its joint to the next one (+ a hand), the length clamped to what a ped's limb can be
            const Vec3 tipLocal = f.InversedRotationTranslation() * frames[def.tip].GetTranslation();
            const float length = std::clamp(tipLocal.Length(), 0.15f, 0.65f) + def.extra;
            JPH::Ref<JPH::Shape> limb = segment(Vec3::sZero(), Vec3(length, 0, 0), def.radius);
            if (def.bone != 18 && def.bone != 22)
                return limb;
            // a calf carries its foot: a capsule from the ankle (bone 19 / 23) past the toe joint (20 / 24)
            const int ankle = def.bone + 1, toe = def.bone + 2;
            const Vec3 a = f.InversedRotationTranslation() * frames[ankle].GetTranslation();
            const Vec3 t = f.InversedRotationTranslation() * frames[toe].GetTranslation();
            Vec3 along = t - a;
            if (along.Length() < 0.03f || along.Length() > 0.4f)
                return limb;
            along = along.Normalized();
            JPH::StaticCompoundShapeSettings compound;
            compound.AddShape(Vec3::sZero(), Quat::sIdentity(), limb);
            compound.AddShape(Vec3::sZero(), Quat::sIdentity(), segment(a - along * 0.03f, t + along * 0.05f, 0.04f));
            JPH::ShapeSettings::ShapeResult result = compound.Create();
            return result.IsValid() ? result.Get() : limb;
        }

        JPH::Ref<JPH::TwoBodyConstraintSettings> joint(const PartDef &def, const Mat44 *frames)
        {
            const Mat44 &parent = frames[kParts[def.parent].bone], &child = frames[def.bone];
            const RVec3 at(child.GetTranslation());
            if (def.joint == Joint::Hinge)
            {
                JPH::Ref<JPH::HingeConstraintSettings> h = new JPH::HingeConstraintSettings;
                h->mPoint1 = h->mPoint2 = at;
                h->mHingeAxis1 = parent.GetAxisZ();
                h->mNormalAxis1 = parent.GetAxisX();
                h->mHingeAxis2 = child.GetAxisZ();
                h->mNormalAxis2 = child.GetAxisX();
                h->mLimitsMin = kHingeMin;
                h->mLimitsMax = kHingeMax;
                h->mMaxFrictionTorque = def.friction;
                h->mMotorSettings = JPH::MotorSettings(8.0f, 1.0f); // off until drive()
                return h.GetPtr();
            }
            JPH::Ref<JPH::SwingTwistConstraintSettings> s = new JPH::SwingTwistConstraintSettings;
            s->mPosition1 = s->mPosition2 = at;
            s->mTwistAxis1 = parent.Multiply3x3(Vec3(def.twist[0], def.twist[1], def.twist[2])).Normalized();
            s->mPlaneAxis1 = parent.Multiply3x3(Vec3(def.plane[0], def.plane[1], def.plane[2])).Normalized();
            s->mTwistAxis2 = child.GetAxisX();
            s->mPlaneAxis2 = child.GetAxisZ();
            s->mTwistMinAngle = -deg(def.twistLimit);
            s->mTwistMaxAngle = deg(def.twistLimit);
            s->mPlaneHalfConeAngle = deg(def.planeHalf);
            s->mNormalHalfConeAngle = deg(def.normalHalf);
            s->mMaxFrictionTorque = def.friction;
            s->mSwingMotorSettings = s->mTwistMotorSettings = JPH::MotorSettings(8.0f, 1.0f); // off until drive()
            return s.GetPtr();
        }

        Doll *find(int id)
        {
            if (!g_dolls)
                return nullptr;
            const auto it = g_dolls->find(id);
            return it == g_dolls->end() ? nullptr : &it->second;
        }

        // A box or sphere of a col model: a shape placed in the model's frame.
        JPH::Ref<JPH::Shape> placed(Vec3 at, JPH::Ref<JPH::Shape> shape)
        {
            return at.LengthSq() > 1e-8f ? new JPH::RotatedTranslatedShape(at, Quat::sIdentity(), shape) : shape;
        }

        // A ragdoll's own ground plane (not a world surface).
        bool isGroundPlane(const JPH::Body &body) { return body.GetCollisionGroup().GetSubGroupID() == kGroundSubGroup; }
    }

    bool init()
    {
        if (g_system || g_failed)
            return g_system != nullptr;
        NearestRounding nearest;
        JPH::RegisterDefaultAllocator();
        JPH::Trace = &trace;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        g_temp = new JPH::TempAllocatorImpl(8u << 20);
        g_jobs = new JPH::JobSystemSingleThreaded(JPH::cMaxPhysicsJobs);
        g_system = new JPH::PhysicsSystem();
        g_system->Init(kMaxBodies, 0, kMaxBodyPairs, kMaxContacts, g_broadPhaseLayers, g_objectVsBroadPhase, g_objectPairs);
        g_system->SetGravity(Vec3(0.0f, 0.0f, -9.81f)); // the game's z is up
        g_ragdollSkeleton = new JPH::Skeleton;
        g_ragdollSkeleton->AddRef();
        for (const PartDef &def : kParts)
            g_ragdollSkeleton->AddJoint("b" + std::to_string(def.bone), def.parent);
        g_dolls = new std::unordered_map<int, Doll>;
        g_obstacles = new std::unordered_map<int, JPH::BodyID>;
        g_shapes = new std::unordered_map<int, JPH::Ref<JPH::Shape>>;
        g_props = new std::unordered_map<int, JPH::BodyID>;
        std::fprintf(stderr, "[jolt] Jolt Physics %d.%d.%d: %d-part ragdolls, %u bodies at most\n", JPH_VERSION_MAJOR,
                     JPH_VERSION_MINOR, JPH_VERSION_PATCH, kPartCount, kMaxBodies);
        return true;
    }

    int create(const RagdollDesc &desc)
    {
        if (!init() || !desc.bones || !desc.parents)
            return 0;
        for (int i = 0; i < kPedBones; ++i)
        {
            if (desc.parents[i] != kPedParents[i])
            {
                trace("refused: bone %d's parent is %d, not the ped skeleton's %d", i, desc.parents[i], kPedParents[i]);
                return 0;
            }
            // the rows' xyz (the game never writes their w: leftovers there can be anything)
            for (int r = 0; r < 4; ++r)
                if (!finite(desc.bones[i].m + r * 4, 3))
                {
                    trace("refused: bone %d's matrix is not finite", i);
                    return 0;
                }
        }
        if (g_system->GetNumBodies() + kPartCount + 1 > kMaxBodies)
        {
            trace("refused: %u bodies, no room for another ragdoll", g_system->GetNumBodies());
            return 0;
        }
        NearestRounding nearest;
        Mat44 frames[kPedBones];
        boneFrames(desc.bones, frames);

        Vec3 velocity(desc.velocity[0], desc.velocity[1], desc.velocity[2]);
        if (!finite(desc.velocity, 3))
            velocity = Vec3::sZero();
        if (velocity.Length() > kMaxSpeed)
            velocity = velocity.Normalized() * kMaxSpeed;

        JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings;
        settings->mSkeleton = g_ragdollSkeleton;
        settings->mParts.resize(kPartCount);
        Mat44 partFrames[kPartCount];
        for (int p = 0; p < kPartCount; ++p)
        {
            const PartDef &def = kParts[p];
            JPH::RagdollSettings::Part &part = settings->mParts[p];
            partFrames[p] = frames[def.bone];
            part.SetShape(partShape(def, frames));
            part.mPosition = RVec3(frames[def.bone].GetTranslation());
            part.mRotation = frames[def.bone].GetQuaternion().Normalized();
            part.mMotionType = p == 0 && desc.active ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic;
            // swept against what it would pass in a step: a body thrown at 15 - 30 m/s (a bike's crash, a blast) moves
            // more than a limb's thickness per step, and the world's walls are single triangles
            part.mMotionQuality = JPH::EMotionQuality::LinearCast;
            part.mObjectLayer = kMoving;
            part.mLinearVelocity = velocity;
            part.mFriction = kFriction;
            part.mRestitution = kRestitution;
            part.mLinearDamping = kLinearDamping;
            part.mAngularDamping = kAngularDamping;
            part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            part.mMassPropertiesOverride.mMass = def.mass;
            if (def.joint != Joint::Root)
                part.mToParent = joint(def, frames);
        }
        settings->Stabilize();
        settings->DisableParentChildCollisions(partFrames, 0.0f);
        const int id = g_nextId++;
        JPH::Ref<DollFilter> filter = new DollFilter(settings->mParts[0].mCollisionGroup.GetGroupFilter());
        for (JPH::RagdollSettings::Part &part : settings->mParts)
            part.mCollisionGroup.SetGroupFilter(filter);
        settings->CalculateBodyIndexToConstraintIndex();
        settings->CalculateConstraintIndexToBodyIdxPair();

        // its ground, where the world has none under it: a slab under it that only it lands on
        const Vec3 hips = frames[kHipsBone].GetTranslation();
        JPH::BodyInterface &bodies = g_system->GetBodyInterface();
        JPH::BodyID ground;
        if (desc.groundPlane)
        {
            JPH::BodyCreationSettings floor(new JPH::BoxShape(Vec3(30.0f, 30.0f, 0.5f)),
                                            RVec3(hips.GetX(), hips.GetY(), desc.groundZ - 0.5f), Quat::sIdentity(),
                                            JPH::EMotionType::Static, kStatic);
            floor.mCollisionGroup = JPH::CollisionGroup(filter, JPH::CollisionGroup::GroupID(id), kGroundSubGroup);
            floor.mFriction = kFriction;
            floor.mRestitution = kRestitution;
            ground = bodies.CreateAndAddBody(floor, JPH::EActivation::DontActivate);
            if (ground.IsInvalid())
            {
                trace("refused: no body for its ground");
                return 0;
            }
        }
        JPH::Ref<JPH::Ragdoll> ragdoll = settings->CreateRagdoll(JPH::CollisionGroup::GroupID(id), JPH::uint64(id), g_system);
        if (!ragdoll)
        {
            trace("refused: no bodies for its parts");
            if (!ground.IsInvalid())
            {
                bodies.RemoveBody(ground);
                bodies.DestroyBody(ground);
            }
            return 0;
        }
        ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);

        // the ped's skeleton and how the parts map onto it, in a model space around the hips
        Doll &doll = (*g_dolls)[id];
        doll.ragdoll = ragdoll;
        doll.ground = ground;
        doll.skeleton = new JPH::Skeleton;
        for (int i = 0; i < kPedBones; ++i)
            doll.skeleton->AddJoint("b" + std::to_string(i), kPedParents[i]);
        Mat44 neutralParts[kPartCount], neutralBones[kPedBones];
        for (int p = 0; p < kPartCount; ++p)
            neutralParts[p] = partFrames[p].PostTranslated(-hips);
        for (int i = 0; i < kPedBones; ++i)
            neutralBones[i] = frames[i].PostTranslated(-hips);
        auto inverse = [](const Mat44 &m) { return m.InversedRotationTranslation(); };
        doll.mapper = new JPH::SkeletonMapper;
        doll.mapper->Initialize(g_ragdollSkeleton, neutralParts, doll.skeleton, neutralBones);
        // Jolt reorients a chain of unmapped bones between two parts toward the next part (the chest's bone itself,
        // with the neck and the left clavicle up to the left arm): here every bone without a body of its own stays as
        // it was on its parent
        JPH::SkeletonMapper::UnmappedVector &unmapped = doll.mapper->GetUnmapped();
        for (const JPH::SkeletonMapper::Chain &chain : doll.mapper->GetChains())
            for (size_t k = 1; k + 1 < chain.mJointIndices2.size(); ++k)
                unmapped.emplace_back(chain.mJointIndices2[k], kPedParents[chain.mJointIndices2[k]]);
        doll.mapper->GetChains().clear();
        std::sort(unmapped.begin(), unmapped.end(),
                  [](const JPH::SkeletonMapper::Unmapped &a, const JPH::SkeletonMapper::Unmapped &b) { return a.mJointIdx < b.mJointIdx; });
        // the parts' bones keep their joints' places on their parents (no stretch shown); the hips' is the body's own
        bool locked[kPedBones] = {};
        for (int p = 1; p < kPartCount; ++p)
            locked[kParts[p].bone] = true;
        doll.mapper->LockTranslations(doll.skeleton, locked, neutralBones);
        for (const JPH::SkeletonMapper::Mapping &m : doll.mapper->GetMappings())
        {
            if (m.mJointIdx1 == 0)
                doll.hipsToBone = m.mJoint1To2;
            if (m.mJointIdx1 >= 0 && m.mJointIdx1 < kPartCount)
                doll.partToBone[m.mJointIdx1] = m.mJoint1To2;
        }
        for (int i = 0; i < kPedBones; ++i)
            doll.local[i] = kPedParents[i] >= 0 ? inverse(neutralBones[kPedParents[i]]) * neutralBones[i] : neutralBones[i];
        doll.rootFromHips = inverse(neutralBones[kHipsBone]) * neutralBones[kRootBone];
        return id;
    }

    void destroy(int id)
    {
        Doll *doll = find(id);
        if (!doll)
            return;
        NearestRounding nearest;
        doll->ragdoll->RemoveFromPhysicsSystem();
        if (!doll->ground.IsInvalid())
        {
            JPH::BodyInterface &bodies = g_system->GetBodyInterface();
            bodies.RemoveBody(doll->ground);
            bodies.DestroyBody(doll->ground);
        }
        g_dolls->erase(id); // the ragdoll's bodies go with it
    }

    void push(int id, int part, const float impulse[3])
    {
        Doll *doll = find(id);
        if (!doll || part < 0 || part >= kPartCount || !finite(impulse, 3))
            return;
        NearestRounding nearest;
        g_system->GetBodyInterface().AddImpulse(doll->ragdoll->GetBodyID(part), Vec3(impulse[0], impulse[1], impulse[2]));
    }

    void kick(int id, int part, const float dv[3])
    {
        Doll *doll = find(id);
        if (!doll || part < 0 || part >= kPartCount || !finite(dv, 3))
            return;
        NearestRounding nearest;
        g_system->GetBodyInterface().AddLinearVelocity(doll->ragdoll->GetBodyID(part), Vec3(dv[0], dv[1], dv[2]));
    }

    int shoot(const float from[3], const float to[3], float push, int *part)
    {
        if (!g_system || !g_dolls || g_dolls->empty() || !finite(from, 3) || !finite(to, 3) || !(push > 0.0f))
            return 0;
        NearestRounding nearest;
        const RVec3 a(from[0], from[1], from[2]);
        const Vec3 d(to[0] - from[0], to[1] - from[1], to[2] - from[2]);
        if (d.LengthSq() < 1e-6f)
            return 0;
        // ragdoll parts (their user data is their ragdoll's id) and the static world in front of them; not the grounds
        // of the ragdolls, nor vehicles and objects (a drive-by's line starts inside its own car)
        class PartsAndWalls final : public JPH::BodyFilter
        {
        public:
            bool ShouldCollideLocked(const JPH::Body &body) const override
            {
                if (body.IsSensor())
                    return false; // a severed limb
                return body.IsDynamic() ? body.GetUserData() != 0 : body.IsStatic() && !isGroundPlane(body);
            }
        };
        const JPH::RRayCast ray{a, d};
        JPH::RayCastResult result;
        if (!g_system->GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, PartsAndWalls()))
            return 0;
        JPH::BodyInterface &bodies = g_system->GetBodyInterface();
        if (bodies.GetMotionType(result.mBodyID) != JPH::EMotionType::Dynamic)
            return 0; // a wall first
        bodies.AddImpulse(result.mBodyID, d.Normalized() * push, ray.GetPointOnRay(result.mFraction));
        const int id = int(bodies.GetUserData(result.mBodyID));
        if (part)
        {
            *part = -1;
            if (const Doll *doll = find(id))
                for (int p = 0; p < kPartCount; ++p)
                    if (doll->ragdoll->GetBodyID(p) == result.mBodyID)
                        *part = p;
        }
        return id;
    }

    void sever(int id, int part)
    {
        Doll *doll = find(id);
        if (!doll || part <= 0 || part >= kPartCount)
            return;
        NearestRounding nearest;
        for (int p = 1; p < kPartCount; ++p)
        {
            int up = p;
            while (up >= 0 && up != part)
                up = kParts[up].parent;
            if (up != part)
                continue;
            JPH::BodyLockWrite lock(g_system->GetBodyLockInterface(), doll->ragdoll->GetBodyID(p));
            if (lock.Succeeded())
                lock.GetBody().SetIsSensor(true);
        }
    }

    void detach(int id, int part, const float velocity[3])
    {
        Doll *doll = find(id);
        if (!doll || part <= 0 || part >= kPartCount || !velocity || !finite(velocity, 3))
            return;
        NearestRounding nearest;
        const int index = doll->ragdoll->GetRagdollSettings()->GetConstraintIndexForBodyIndex(part);
        if (index >= 0)
            doll->ragdoll->GetConstraint(index)->SetEnabled(false);
        doll->detached[part] = true;
        JPH::BodyInterface &bodies = g_system->GetBodyInterface();
        const JPH::BodyID body = doll->ragdoll->GetBodyID(part);
        bodies.AddLinearVelocity(body, Vec3(velocity[0], velocity[1], velocity[2]));
        bodies.ActivateBody(body);
    }

    void blast(int id, const float centre[3], float radius, float speed)
    {
        Doll *doll = find(id);
        if (!doll || !finite(centre, 3) || !(radius > 0.0f) || !(speed > 0.0f))
            return;
        NearestRounding nearest;
        JPH::BodyInterface &bodies = g_system->GetBodyInterface();
        const RVec3 c(centre[0], centre[1], centre[2]);
        for (const JPH::BodyID &body : doll->ragdoll->GetBodyIDs())
        {
            const Vec3 away(bodies.GetCenterOfMassPosition(body) - c);
            const float d = away.Length();
            if (d >= radius)
                continue;
            const Vec3 dir = ((d > 0.05f ? away / d : Vec3::sZero()) + Vec3(0.0f, 0.0f, 0.6f)).Normalized();
            bodies.AddLinearVelocity(body, dir * speed * (1.0f - d / radius));
        }
    }

    int addObstacle(const BoneMatrix &transform, const float boxMin[3], const float boxMax[3], bool car)
    {
        if (!init() || !finite(transform.m, 3) || !finite(transform.m + 12, 3) || !finite(boxMin, 3) || !finite(boxMax, 3))
            return 0;
        const Vec3 lo(boxMin[0], boxMin[1], boxMin[2]), hi(boxMax[0], boxMax[1], boxMax[2]);
        const Vec3 half = 0.5f * (hi - lo);
        if (half.ReduceMin() < 0.06f || half.ReduceMax() > 20.0f || g_system->GetNumBodies() + 1 > kMaxBodies)
            return 0;
        NearestRounding nearest;
        auto block = [](Vec3 a, Vec3 b) -> JPH::Ref<JPH::Shape> {
            return new JPH::RotatedTranslatedShape(0.5f * (a + b), Quat::sIdentity(), new JPH::BoxShape(0.5f * (b - a)));
        };
        JPH::Ref<JPH::Shape> shape = block(lo, hi);
        if (car && half.GetZ() > 0.3f && half.GetY() > 1.0f)
        {
            // the lower body to the bonnet's height (55% up), the cabin over the middle (a fifth of the length off the
            // back, a third off the front: the bonnet)
            const float bonnet = lo.GetZ() + 1.1f * half.GetZ(), length = 2.0f * half.GetY();
            JPH::StaticCompoundShapeSettings compound;
            compound.AddShape(Vec3::sZero(), Quat::sIdentity(), block(lo, Vec3(hi.GetX(), hi.GetY(), bonnet)));
            compound.AddShape(Vec3::sZero(), Quat::sIdentity(),
                              block(Vec3(lo.GetX(), lo.GetY() + 0.2f * length, bonnet - 0.05f),
                                    Vec3(hi.GetX(), hi.GetY() - 0.33f * length, hi.GetZ())));
            JPH::ShapeSettings::ShapeResult result = compound.Create();
            if (result.IsValid())
                shape = result.Get();
        }
        const Mat44 frame = orthonormal(transform, Mat44::sIdentity());
        JPH::BodyCreationSettings body(shape, RVec3(frame.GetTranslation()), frame.GetQuaternion().Normalized(),
                                       JPH::EMotionType::Kinematic, kMoving);
        body.mFriction = kFriction;
        body.mRestitution = kRestitution;
        const JPH::BodyID bodyId = g_system->GetBodyInterface().CreateAndAddBody(body, JPH::EActivation::Activate);
        if (bodyId.IsInvalid())
            return 0;
        const int id = g_nextId++;
        (*g_obstacles)[id] = bodyId;
        return id;
    }

    void moveObstacle(int id, const BoneMatrix &transform, float seconds)
    {
        if (!g_obstacles || !finite(transform.m, 3) || !finite(transform.m + 12, 3))
            return;
        const auto it = g_obstacles->find(id);
        if (it == g_obstacles->end())
            return;
        NearestRounding nearest;
        const Mat44 frame = orthonormal(transform, Mat44::sIdentity());
        const RVec3 to(frame.GetTranslation());
        const Quat rotation = frame.GetQuaternion().Normalized();
        JPH::BodyInterface &bodies = g_system->GetBodyInterface();
        // swept there over the step (the contacts get its speed); placed when it jumped (a teleport, a respawn)
        if (seconds > 0.0f && Vec3(to - bodies.GetPosition(it->second)).Length() < 5.0f)
            bodies.MoveKinematic(it->second, to, rotation, seconds);
        else
        {
            bodies.SetPositionAndRotation(it->second, to, rotation, JPH::EActivation::DontActivate);
            bodies.SetLinearAndAngularVelocity(it->second, Vec3::sZero(), Vec3::sZero());
        }
    }

    void removeObstacle(int id)
    {
        if (!g_obstacles)
            return;
        const auto it = g_obstacles->find(id);
        if (it == g_obstacles->end())
            return;
        NearestRounding nearest;
        JPH::BodyInterface &bodies = g_system->GetBodyInterface();
        bodies.RemoveBody(it->second);
        bodies.DestroyBody(it->second);
        g_obstacles->erase(it);
    }

    int addShape(const ColShape &col)
    {
        if (!init())
            return 0;
        NearestRounding nearest;
        std::vector<JPH::Ref<JPH::Shape>> parts;
        std::vector<Vec3> at;
        if (col.vertices && col.triangles && col.vertexCount > 0 && col.triangleCount > 0 &&
            finite(col.vertices, col.vertexCount * 3))
        {
            JPH::VertexList vertices;
            vertices.reserve(col.vertexCount);
            for (int i = 0; i < col.vertexCount; ++i)
                vertices.push_back(JPH::Float3(col.vertices[i * 3], col.vertices[i * 3 + 1], col.vertices[i * 3 + 2]));
            JPH::IndexedTriangleList triangles;
            triangles.reserve(col.triangleCount);
            for (int i = 0; i < col.triangleCount; ++i)
            {
                const int *t = col.triangles + i * 3;
                if (std::min({t[0], t[1], t[2]}) >= 0 && std::max({t[0], t[1], t[2]}) < col.vertexCount)
                    triangles.push_back(JPH::IndexedTriangle(JPH::uint32(t[0]), JPH::uint32(t[1]), JPH::uint32(t[2]), 0));
            }
            // MeshShapeSettings drops degenerate triangles; none left is an error, not a shape
            JPH::MeshShapeSettings mesh(std::move(vertices), std::move(triangles));
            JPH::ShapeSettings::ShapeResult result = mesh.Create();
            if (result.IsValid())
            {
                parts.push_back(result.Get());
                at.push_back(Vec3::sZero());
            }
        }
        for (int i = 0; col.boxes && i < col.boxCount; ++i)
        {
            const float *b = col.boxes + i * 6;
            if (!finite(b, 6))
                continue;
            const Vec3 lo(b[0], b[1], b[2]), hi(b[3], b[4], b[5]);
            const Vec3 half = Vec3::sMax(0.5f * (hi - lo), Vec3::sReplicate(0.01f));
            if (half.ReduceMax() > 500.0f)
                continue;
            parts.push_back(new JPH::BoxShape(half));
            at.push_back(0.5f * (lo + hi));
        }
        for (int i = 0; col.spheres && i < col.sphereCount; ++i)
        {
            const float *s = col.spheres + i * 4;
            if (!finite(s, 4) || !(s[3] > 0.01f) || s[3] > 500.0f)
                continue;
            parts.push_back(new JPH::SphereShape(s[3]));
            at.push_back(Vec3(s[0], s[1], s[2]));
        }
        JPH::Ref<JPH::Shape> shape;
        if (parts.size() == 1)
            shape = placed(at[0], parts[0]);
        else if (parts.size() > 1)
        {
            JPH::StaticCompoundShapeSettings compound;
            for (size_t i = 0; i < parts.size(); ++i)
                compound.AddShape(at[i], Quat::sIdentity(), parts[i]);
            JPH::ShapeSettings::ShapeResult result = compound.Create();
            if (result.IsValid())
                shape = result.Get();
        }
        if (!shape)
            return 0;
        const int id = g_nextId++;
        (*g_shapes)[id] = shape;
        return id;
    }

    void removeShape(int shape)
    {
        if (!g_shapes)
            return;
        NearestRounding nearest; // the shape may go with it
        g_shapes->erase(shape);
    }

    int addWorld(int shape, const BoneMatrix &transform, bool moving)
    {
        if (!g_shapes || !finite(transform.m, 3) || !finite(transform.m + 4, 3) || !finite(transform.m + 8, 3) ||
            !finite(transform.m + 12, 3))
            return 0;
        const auto it = g_shapes->find(shape);
        if (it == g_shapes->end() || g_system->GetNumBodies() + 1 > kWorldBodies)
            return 0;
        NearestRounding nearest;
        const Mat44 frame = orthonormal(transform, Mat44::sIdentity());
        JPH::BodyCreationSettings body(it->second, RVec3(frame.GetTranslation()), frame.GetQuaternion().Normalized(),
                                       moving ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static,
                                       moving ? kMoving : kStatic);
        body.mFriction = kFriction;
        body.mRestitution = kRestitution;
        const JPH::BodyID bodyId = g_system->GetBodyInterface().CreateAndAddBody(body, JPH::EActivation::DontActivate);
        if (bodyId.IsInvalid())
            return 0;
        const int id = g_nextId++;
        (*g_obstacles)[id] = bodyId;
        return id;
    }

    bool ground(const float from[3], float depth, float &z)
    {
        if (!g_system || !finite(from, 3) || !(depth > 0.0f))
            return false;
        NearestRounding nearest;
        // the world's bodies and the obstacles: what isn't a ragdoll's part or ground
        class World final : public JPH::BodyFilter
        {
        public:
            bool ShouldCollideLocked(const JPH::Body &body) const override
            {
                return !body.IsDynamic() && body.GetUserData() == 0 && !isGroundPlane(body); // (a reacting ped's hips are kinematic)
            }
        };
        const JPH::RRayCast ray{RVec3(from[0], from[1], from[2]), Vec3(0.0f, 0.0f, -depth)};
        JPH::RayCastResult result;
        if (!g_system->GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, World()))
            return false;
        z = float(ray.GetPointOnRay(result.mFraction).GetZ());
        return true;
    }

    void step(float seconds)
    {
        if (!g_system || !g_dolls || (g_dolls->empty() && g_props->empty()) || !(seconds > 0.0f))
            return;
        NearestRounding nearest;
        const auto start = std::chrono::steady_clock::now();
        const int steps = std::clamp(int(std::ceil(seconds * 60.0f - 0.01f)), 1, 4);
        g_system->Update(std::min(seconds, 0.1f), steps, g_temp, g_jobs);
        g_stepMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    bool pose(int id, BoneMatrix *out)
    {
        Doll *doll = find(id);
        if (!doll || !out)
            return false;
        NearestRounding nearest;
        RVec3 origin;
        Mat44 parts[kPartCount], bones[kPedBones];
        doll->ragdoll->GetPose(origin, parts);
        // the root bone has no part above it: carried with the hips
        doll->local[kRootBone] = parts[0] * doll->hipsToBone * doll->rootFromHips;
        doll->mapper->Map(parts, doll->local, bones);
        // a part let go of the body: its bone where its body is (the mapper keeps a part's joint on its parent), the bones
        // below it carried with it (they come after their parents)
        for (int p = 1; p < kPartCount; ++p)
        {
            if (!doll->detached[p])
                continue;
            const int top = kParts[p].bone;
            bones[top] = parts[p] * doll->partToBone[p];
            for (int i = top + 1; i < kPedBones; ++i)
            {
                int up = kPedParents[i];
                while (up >= 0 && up != top)
                    up = kPedParents[up];
                if (up == top)
                    bones[i] = bones[kPedParents[i]] * doll->local[i];
            }
        }
        for (int i = 0; i < kPedBones; ++i)
        {
            const Vec3 x = bones[i].GetAxisX(), y = bones[i].GetAxisY(), z = bones[i].GetAxisZ();
            const Vec3 p = Vec3(RVec3(bones[i].GetTranslation()) + origin);
            const float m[16] = {x.GetX(), x.GetY(), x.GetZ(), 0.0f, y.GetX(), y.GetY(), y.GetZ(), 0.0f,
                                 z.GetX(), z.GetY(), z.GetZ(), 0.0f, p.GetX(), p.GetY(), p.GetZ(), 0.0f};
            std::copy(m, m + 16, out[i].m);
        }
        return true;
    }

    void drive(int id, const BoneMatrix *bones, float seconds, float strength)
    {
        Doll *doll = find(id);
        if (!doll || !bones)
            return;
        for (int i = 0; i < kPedBones; ++i)
            for (int r = 0; r < 4; ++r)
                if (!finite(bones[i].m + r * 4, 3))
                    return;
        NearestRounding nearest;
        strength = std::clamp(std::isfinite(strength) ? strength : 0.0f, 0.0f, 3.0f);
        Mat44 frames[kPedBones];
        boneFrames(bones, frames);
        JPH::BodyInterface &bodies = g_system->GetBodyInterface();
        const JPH::BodyID hips = doll->ragdoll->GetBodyID(0);
        if (seconds > 0.0f && bodies.GetMotionType(hips) == JPH::EMotionType::Kinematic)
            bodies.MoveKinematic(hips, RVec3(frames[kHipsBone].GetTranslation()),
                                 frames[kHipsBone].GetQuaternion().Normalized(), seconds);
        const JPH::RagdollSettings *settings = doll->ragdoll->GetRagdollSettings();
        const JPH::EMotorState state = strength > 0.0f ? JPH::EMotorState::Position : JPH::EMotorState::Off;
        for (int p = 1; p < kPartCount; ++p)
        {
            const PartDef &def = kParts[p];
            const int index = settings->GetConstraintIndexForBodyIndex(p);
            if (index < 0)
                continue;
            // the child's rotation in its parent's (the bodies' frames are the bones')
            const Quat rel = (frames[kParts[def.parent].bone].GetQuaternion().Normalized().Conjugated() *
                              frames[def.bone].GetQuaternion().Normalized()).Normalized();
            JPH::TwoBodyConstraint *c = doll->ragdoll->GetConstraint(index);
            if (def.joint == Joint::Hinge)
            {
                auto *h = static_cast<JPH::HingeConstraint *>(c);
                h->GetMotorSettings().SetTorqueLimit(def.muscle * strength);
                h->SetMotorState(state);
                h->SetTargetOrientationBS(rel);
            }
            else
            {
                auto *st = static_cast<JPH::SwingTwistConstraint *>(c);
                st->GetSwingMotorSettings().SetTorqueLimit(def.muscle * strength);
                st->GetTwistMotorSettings().SetTorqueLimit(def.muscle * strength);
                st->SetSwingMotorState(state);
                st->SetTwistMotorState(state);
                st->SetTargetOrientationBS(rel);
            }
        }
        if (strength > 0.0f)
            doll->ragdoll->Activate();
    }

    void brace(int id, const float dir[3], float strength)
    {
        Doll *doll = find(id);
        if (!doll || !dir || !finite(dir, 3))
            return;
        NearestRounding nearest;
        Vec3 d(dir[0], dir[1], dir[2]);
        if (d.LengthSq() < 1e-6f)
            return;
        d = d.Normalized();
        strength = std::clamp(std::isfinite(strength) ? strength : 0.0f, 0.0f, 3.0f);
        const JPH::EMotorState state = strength > 0.0f ? JPH::EMotorState::Position : JPH::EMotorState::Off;
        JPH::BodyInterface &bodies = g_system->GetBodyInterface();
        const JPH::RagdollSettings *settings = doll->ragdoll->GetRagdollSettings();
        for (int p : {int(kUpperArmL), int(kForearmL), int(kUpperArmR), int(kForearmR)})
        {
            const PartDef &def = kParts[p];
            const int index = settings->GetConstraintIndexForBodyIndex(p);
            if (index < 0)
                continue;
            JPH::TwoBodyConstraint *c = doll->ragdoll->GetConstraint(index);
            if (def.joint == Joint::Hinge)
            {
                // the elbow straight: the forearm as the upper arm (a limb's frame runs along it)
                auto *h = static_cast<JPH::HingeConstraint *>(c);
                h->GetMotorSettings().SetTorqueLimit(def.muscle * strength);
                h->SetMotorState(state);
                h->SetTargetOrientationBS(Quat::sIdentity());
                continue;
            }
            // the upper arm turned so its length (X) points along d, from where it is now
            const Quat parent = bodies.GetRotation(doll->ragdoll->GetBodyID(def.parent)).Normalized();
            const Quat child = bodies.GetRotation(doll->ragdoll->GetBodyID(p)).Normalized();
            const Quat target = (Quat::sFromTo(child * Vec3::sAxisX(), d) * child).Normalized();
            auto *st = static_cast<JPH::SwingTwistConstraint *>(c);
            st->GetSwingMotorSettings().SetTorqueLimit(def.muscle * strength);
            st->GetTwistMotorSettings().SetTorqueLimit(def.muscle * strength);
            st->SetSwingMotorState(state);
            st->SetTwistMotorState(state);
            st->SetTargetOrientationBS((parent.Conjugated() * target).Normalized());
        }
        if (strength > 0.0f)
            doll->ragdoll->Activate();
    }

    bool awake(int id)
    {
        Doll *doll = find(id);
        return doll && doll->ragdoll->IsActive();
    }

    float clear(const float from[3], const float to[3])
    {
        if (!g_system || !finite(from, 3) || !finite(to, 3))
            return 1.0f;
        NearestRounding nearest;
        class World final : public JPH::BodyFilter
        {
        public:
            bool ShouldCollideLocked(const JPH::Body &body) const override
            {
                return !body.IsDynamic() && body.GetUserData() == 0 && !isGroundPlane(body); // (a reacting ped's hips are kinematic)
            }
        };
        const JPH::RRayCast ray{RVec3(from[0], from[1], from[2]), Vec3(to[0] - from[0], to[1] - from[1], to[2] - from[2])};
        JPH::RayCastResult result;
        return g_system->GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, World()) ? result.mFraction : 1.0f;
    }

    int addProp(const BoneMatrix &transform, const float centre[3], const float half[3], float mass, const float velocity[3],
                const float spin[3])
    {
        if (!init() || !finite(transform.m, 3) || !finite(transform.m + 4, 3) || !finite(transform.m + 8, 3) ||
            !finite(transform.m + 12, 3) || !finite(centre, 3) || !finite(half, 3) || !finite(velocity, 3) ||
            !finite(spin, 3) || g_system->GetNumBodies() + 1 > kWorldBodies)
            return 0;
        NearestRounding nearest;
        const Vec3 h = Vec3::sMax(Vec3(half[0], half[1], half[2]), Vec3::sReplicate(0.01f));
        if (h.ReduceMax() > 2.0f)
            return 0;
        const Mat44 frame = orthonormal(transform, Mat44::sIdentity());
        JPH::BodyCreationSettings body(placed(Vec3(centre[0], centre[1], centre[2]), new JPH::BoxShape(h, std::min(0.02f, h.ReduceMin()))),
                                       RVec3(frame.GetTranslation()), frame.GetQuaternion().Normalized(),
                                       JPH::EMotionType::Dynamic, kMoving);
        body.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        body.mMassPropertiesOverride.mMass = std::clamp(mass, 0.05f, 20.0f);
        body.mMotionQuality = JPH::EMotionQuality::LinearCast; // small and fast: through a kerb in one step otherwise
        body.mFriction = 0.6f;
        body.mRestitution = 0.2f;
        body.mLinearDamping = 0.05f;
        body.mAngularDamping = 0.3f;
        body.mLinearVelocity = Vec3(velocity[0], velocity[1], velocity[2]);
        body.mAngularVelocity = Vec3(spin[0], spin[1], spin[2]);
        const JPH::BodyID bodyId = g_system->GetBodyInterface().CreateAndAddBody(body, JPH::EActivation::Activate);
        if (bodyId.IsInvalid())
            return 0;
        const int id = g_nextId++;
        (*g_props)[id] = bodyId;
        return id;
    }

    bool propPose(int id, BoneMatrix &out)
    {
        if (!g_props)
            return false;
        const auto it = g_props->find(id);
        if (it == g_props->end())
            return false;
        NearestRounding nearest;
        const Mat44 m = g_system->GetBodyInterface().GetWorldTransform(it->second);
        for (int r = 0; r < 4; ++r)
        {
            const Vec3 v = r < 3 ? m.GetColumn3(r) : m.GetTranslation();
            out.m[r * 4] = v.GetX();
            out.m[r * 4 + 1] = v.GetY();
            out.m[r * 4 + 2] = v.GetZ();
            out.m[r * 4 + 3] = 0.0f;
        }
        return true;
    }

    bool propAwake(int id)
    {
        if (!g_props)
            return false;
        const auto it = g_props->find(id);
        return it != g_props->end() && g_system->GetBodyInterface().IsActive(it->second);
    }

    void removeProp(int id)
    {
        if (!g_props)
            return;
        const auto it = g_props->find(id);
        if (it == g_props->end())
            return;
        NearestRounding nearest;
        JPH::BodyInterface &bodies = g_system->GetBodyInterface();
        bodies.RemoveBody(it->second);
        bodies.DestroyBody(it->second);
        g_props->erase(it);
    }

    void blendPose(const BoneMatrix *from, const BoneMatrix *to, float t, BoneMatrix *out, int count)
    {
        NearestRounding nearest;
        t = std::clamp(std::isfinite(t) ? t : 1.0f, 0.0f, 1.0f);
        for (int i = 0; i < count; ++i)
        {
            const Mat44 a = orthonormal(from[i], Mat44::sIdentity()), b = orthonormal(to[i], a);
            const Quat q = a.GetQuaternion().Normalized().SLERP(b.GetQuaternion().Normalized(), t).Normalized();
            const Mat44 m = Mat44::sRotationTranslation(q, a.GetTranslation() * (1.0f - t) + b.GetTranslation() * t);
            for (int r = 0; r < 4; ++r)
            {
                const Vec3 v = r < 3 ? m.GetColumn3(r) : m.GetTranslation();
                out[i].m[r * 4] = v.GetX();
                out[i].m[r * 4 + 1] = v.GetY();
                out[i].m[r * 4 + 2] = v.GetZ();
                out[i].m[r * 4 + 3] = 0.0f;
            }
        }
    }

    Stats stats()
    {
        Stats s;
        if (!g_system)
            return s;
        s.ragdolls = int(g_dolls->size());
        s.shapes = int(g_shapes->size());
        s.props = int(g_props->size());
        s.bodies = g_system->GetNumBodies();
        s.activeBodies = g_system->GetNumActiveBodies(JPH::EBodyType::RigidBody);
        s.stepMs = g_stepMs;
        return s;
    }
}
