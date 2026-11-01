// See jolt.h and the port's docs/game-internals.md 12h.
#include "jolt.h"

#include "physics/ragdoll.h"
#include "mod.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vcs::jolt
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kSetDie = 0x0020A3B8u;         // CPed::SetDie(ped a0, ...): ped state 0x39
        constexpr uint32_t kApplyMoveSpeed = 0x00348098u; // CPhysical::ApplyMoveSpeed(entity a0): position += speed x step
        constexpr uint32_t kAfterWorld = 0x00408C30u;     // called only at 0x3BABDC, right after CWorld::Process
        constexpr uint32_t kTreeUpdate = 0x0024E8D0u;     // an anim tree's bone matrices (tree a0)
        constexpr uint32_t kTreeUpdateInPedRender = 0x003C3280u; // its return address in FUN_003c3250 (s0 = the ped)
        constexpr uint32_t kAfterBones = 0x0020C310u;            // CPed::IsPlayer, called by the ped's render next
        constexpr uint32_t kAfterBonesInPedRender = 0x00200740u; // its return address there (a0 = the ped)
        // A part gone on whole (a head shot off) is a second draw of the ped's skinned element. CPed::Render only builds
        // the bones; the renderer calls the element's render callback (FUN_0036eb70, a0 the element) later, and what it
        // queues is drawn at the frame's end through the element's anim tree and the tree's matrices (+0x28) - so the
        // second draw is a copy of the element pointing at a copy of the tree pointing at matrices of its own (one slot
        // each, in the main heap), passed to the callback after the element itself.
        constexpr uint32_t kElementRender = 0x0036EB70u;
        constexpr uint32_t kElementAgain = 0x0036EB74u;  // inside it, no entry: the continuation of the second call ...
        constexpr uint32_t kPoolMade = 0x0036EB78u;      // ... and of the slots' allocation
        constexpr uint32_t kMainHeapAlloc = 0x002BD178u; // FUN_002BD178(size): the main heap's malloc (v0)
        constexpr uint32_t kSlotMatrices = 0, kSlotTree = 25u * 0x40u, kTreeCopy = 0x400u, kSlotElement = kSlotTree + kTreeCopy,
                           kElementCopy = 0x40u, kPoolSlot = kSlotElement + kElementCopy;
        constexpr uint32_t kPoolSlots = 8; // loose parts drawn in a frame
        // CPed::InflictDamage(ped a0, attacker a1, weapon a2, damage f12, piece a3, direction t0); it calls SetDie
        constexpr uint32_t kInflictDamage = 0x001D7940u;
        constexpr int kBlastDamage = 0x31; // its weapon for a blast (CWorld::TriggerExplosion's)
        // CWeapon::ProcessLineOfSight(start* a0, end* a1, colpoint* a2, entity** a3, weapon t0, shooter t1, ...): every
        // bullet's line - FireInstantHit's, each shotgun pellet's, drive-bys' (free aim's hook, installed after, wraps
        // this one with its line put in)
        constexpr uint32_t kLineOfSight = 0x002D0870u;
        constexpr int kT0 = 8;
        // CWorld::TriggerExplosion(position* a0, culprit a1, ..., radius f12, power f13): a blast's push and damage
        constexpr uint32_t kTriggerExplosion = 0x00192760u;
        constexpr uint32_t kPedPool = 0x00487A94u, kPedSize = 0xD20u; // CPool: +0 entries, +4 flag bytes, +8 size
        constexpr uint32_t kVehiclePool = 0x00487A98u, kVehicleSize = 0x8C0u;
        constexpr uint32_t kModelInfos = 0x00486518u; // CModelInfo::ms_modelInfoPtrs
        // a model info's CColModel; its bounding box's min / max (16-byte vectors) in the model's frame
        constexpr uint32_t kModelColModel = 0x14u, kColBoxMin = 0x10u, kColBoxMax = 0x20u;
        constexpr uint32_t kVehicleMoveSpeed = 0x140u; // x 50 = m/s
        constexpr uint32_t kVehicleType = 0x354u;      // 0 car, 1 boat, 2 jetski, 4 heli, 5 plane, 6 bike, 8 BMX, 9 quad
        constexpr uint32_t kPedPosition = 0x30u, kPedMoveSpeed = 0x140u, kPedVehicle = 0x480u, kPedState = 0x8C4u;
        constexpr uint32_t kPedModel = 0x56u;
        constexpr uint32_t kStateDying = 0x39u, kStateDead = 0x3Au;
        constexpr uint32_t kTimeStep = 0x004CD110u;     // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        constexpr uint32_t kFrameCounter = 0x004CD124u; // CTimer::m_FrameCounter
        // RslTAnimTree: +0x04 node count, +0x28 the bones' world matrices (0x40 each), +0x30 node info (8 bytes each:
        // id, index, flags - 2 push, 1 pop the parent, as RenderWare's HAnim - and a pad byte; then the node's frame)
        constexpr uint32_t kTreeNodes = 0x04u, kTreeMatrices = 0x28u, kTreeNodeInfo = 0x30u;
        constexpr uint32_t kNodePush = 2u, kNodePop = 1u;
        constexpr int kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7, kS0 = 16, kRa = 31;
        constexpr int kBones = physics::kPedBones;
        constexpr size_t kMaxDolls = 24;        // more: the oldest is frozen in its last pose
        constexpr float kSoles = 1.0f;          // a standing ped's position is 1 m over its soles
        constexpr uint32_t kSeedFrames = 15;    // a death noted longer ago than this (off screen) starts still
        constexpr uint32_t kPendingFrames = 3000; // ... and is forgotten after this, never drawn
        constexpr float kGameMoved = 2.0f;      // the game moved the ped by more than this: a teleport, a respawn
        constexpr uint32_t kHitFrames = 10;     // a blow this recent is the one that killed
        constexpr float kVehicleReach = 15.0f;  // vehicles this near a ragdoll's hips are obstacles in the physics
        constexpr float kVehicleClearance = 0.25f; // ... their box raised off the ground: lying bodies go under, as wheels
        constexpr float kFollowZ = 0.5f;        // the ped's height follows its body when off by more (down stairs, a ledge)
        constexpr float kLost = 60.0f;          // a body this far under where it started fell out of the world: frozen

        // The world's collision (game-internals.md 12k). CWorld's sectors: 50 x 50 of 80 m (index x / 80 + 30, y / 80 + 25)
        // at *0x486464, 0x38 bytes of entity lists each (CPtrList: the first node; a node is {item, prev, next}).
        constexpr uint32_t kSectors = 0x00486464u, kSectorBytes = 0x38u;
        constexpr int kSectorCount = 50;
        constexpr float kSectorSize = 80.0f, kSectorX0 = 30.0f, kSectorY0 = 25.0f;
        // the lists CWorld::ProcessLineOfSight takes buildings from, and the objects'
        constexpr uint32_t kBuildingLists[] = {0x00u, 0x04u, 0x14u, 0x18u}, kObjectLists[] = {0x0Cu, 0x10u};
        constexpr uint32_t kListLimit = 4096; // entities a list is walked for at most
        // CEntity: +0 its matrix (rows right, forward, up, position), +0x48 flags (bits 1-3 the type, bit 9 uses
        // collision), +0x56 its model
        constexpr uint32_t kEntityFlags = 0x48u, kUsesCollision = 1u << 9;
        // CColModel past its bounds: counts (u16 boxes, u16 triangles, u8 spheres) and arrays - spheres 0x20 bytes
        // (centre, radius), boxes 0x30 (min, max), vertices int16 xyz / 128, triangles 8 bytes (three u16 byte offsets
        // into the vertices, clockwise seen from the front; surface; a byte)
        constexpr uint32_t kColBoxCount = 0x30u, kColTriangleCount = 0x32u, kColSphereCount = 0x34u,
                           kColSphereArray = 0x38u, kColBoxArray = 0x40u, kColVertexArray = 0x48u,
                           kColTriangleArray = 0x4Cu;
        constexpr float kWorldReach = 10.0f;  // the world's collision this near a ragdoll's hips is in the physics
        constexpr float kGroundSearch = 50.0f; // a ragdoll gets its own ground when the world has nothing this far under it
        constexpr uint32_t kShapeMs = 10000;   // a col model's shape is kept this long unused
        constexpr uint32_t kTimeMs = 0x004CD104u; // CTimer::m_snTimeInMilliseconds (game time)

        // Bike falls: CBike::KnockOffRider(bike a0, weapon a1, direction a2, rider a3, get back on t0) throws a rider
        // off (a crash, a vehicle's hit, a shot) and ends in CPed::SetFall (FUN_002022f0: state 0x2D, a get-up time) or
        // SetDie. CPed::Fall (FUN_00202808) gets the ped up (SetGetUp FUN_00203240, state 0x2E) once that time has
        // passed (-1: not yet) and the ped stands (+0x1C8 bit 0).
        constexpr uint32_t kKnockOffRider = 0x003F0A58u;
        // Catching the fall and firing back from the ground (SA's Jolt mod's CatchFall / KnockdownGunFire)
        constexpr uint32_t kCatchMs = 1500;       // the arms reach for the ground at most this long into a knockdown ...
        constexpr float kCatchLand = 0.35f;       // ... or until the hips are this near the ground under them
        // CWeapon::Fire(weapon a0, shooter a1, source* a2 - 0: 0.6 m up the shooter's axis from its position); a ped's
        // weapons at +0x574, 0x1C each (+4 type, +0xC clip), the current one's index the byte at +0x789
        constexpr uint32_t kFire = 0x002C0F98u;
        // CParticle::AddParticle(type a0 - 7 blood -, position* a1, direction* a2, entity a3, size f12, t0 .. t3 0)
        // (FUN_001e9040: the shotgun's pellets on a body down spray 8 of them), and CGame::nastyGame (0 = no gore)
        constexpr uint32_t kAddParticle = 0x001E9040u, kBloodParticle = 7u, kNastyGame = 0x00489F80u;
        constexpr int kPedParentBone[25] = {-1, 0, 1, 2, 3, 4, 5, 4, 7, 8, 9, 10, 4, 12, 13, 14, 15, 2, 17, 18, 19,
                                            2, 21, 22, 23};
        constexpr uint32_t kPedWeapons = 0x574u, kWeaponSize = 0x1Cu, kPedWeaponSlot = 0x789u;
        constexpr float kMuzzleUp = 0.6f;
        constexpr uint32_t kFireAfterMs = 500;    // after landing, the first shot ...
        constexpr uint32_t kFireGapMs = 350;      // ... then one every this
        constexpr int kFireShots = 4;             // at most
        constexpr uint32_t kStateFall = 0x2Du;
        constexpr uint32_t kPedGetUpTime = 0x6C4u; // CPed::m_getUpTimer (ms)
        constexpr uint32_t kPedHeading = 0x8E0u;   // m_fRotationCur, then m_fRotationDest (radians; 0 faces +y)
        constexpr uint32_t kKnockMs = 1000;        // a rider knocked off is ragdolled at a draw within this
        constexpr float kThrowUp = 4.5f;           // a rider thrown off rises at most this fast (m/s; throwOff) ...
        constexpr float kThrowMax = 20.0f;         // ... and goes on at most this fast
        constexpr float kRammerReach = 4.5f, kRammerSpeed = 3.0f; // a vehicle this near and fast rammed it (rammedBy)
        constexpr uint32_t kSettleMs = 400;        // its body still this long (asleep, or its hips within 3 cm): it gets up ...
        constexpr uint32_t kMinDownMs = 2500;      // ... down this long at least (as GTA IV's peds lie a moment) ...
        constexpr uint32_t kDownMs = 8000;         // ... or at the latest after this
        constexpr uint32_t kBlendMs = 350;         // its last pose eased into the game's get-up over this
        constexpr uint32_t kStandMs = 1500;        // ... which the game starts within this (else the ped is left to it)

        // Hit reactions (GTA IV's Euphoria, after SA's Jolt Physics mod's active ragdolls): a ped shot and not killed gets
        // a ragdoll for a moment whose hips follow its animation and whose muscles pull every limb back to it; the bullet
        // pushes the part it hit. Drawn as the physics' pose blended into the animation's.
        constexpr uint32_t kReactMs = 900;        // how long
        constexpr uint32_t kReactHoldMs = 200;    // all physics this long, then eased back into the animation
        constexpr float kReactLimp = 0.6f, kReactStrength = 1.0f; // its muscles: weaker as it is hit, back to tone by the end
        constexpr float kReactPush = 0.8f;        // the killing push's share a hit gets
        constexpr uint32_t kFreshHitFrames = 2;   // a hit this recent (frames) reacts at the ped's next draw
        constexpr float kReactMaxOff = 0.2f;      // a reacting body drawn at most this far (m) off its animation
        constexpr float kReactRunSpeed = 3.0f;    // its animation's hips faster than this (m/s: sprinting off) ...
        constexpr uint32_t kReactRunFadeMs = 150; // ... the reaction fades out over this
        // Its extras (SA's mod's arm grab; Euphoria's stagger): the free hand to the wound for kClutchMs (eased in and
        // out), and for a push of kStumblePush or more - or the third hit within kStumbleRowMs - a few steps along it.
        constexpr uint32_t kClutchMs = 1600, kClutchInMs = 250, kClutchOutMs = 400;
        constexpr float kStumblePush = 85.0f;      // kg m/s: a shotgun's, a rifle's; not a pistol's
        constexpr uint32_t kStumbleRowMs = 1500;
        constexpr uint32_t kPlayerPed = 0x004E4910u; // CWorld::Players[0].m_pPed: Vic gets neither

        // Pickups (game-internals.md 12m): CPickups' array at 0x4EA740, 336 of 0x40 bytes (the last 16 for money): +0x00
        // the position collection is measured from, +0x20 the object drawn, +0x34 u16 its model, +0x38 u8 the type (0
        // none, 4 a dead ped's gun, 8 money). CPickups::DoPickUpEffects(entity a0) = FUN_001df618, from the render: a
        // pickup's spin (a gun's x 1.84), glow and coronas.
        constexpr uint32_t kPickups = 0x004EA740u, kPickupSize = 0x40u, kPickupCount = 336;
        constexpr uint32_t kPickupObject = 0x20u, kPickupModel = 0x34u, kPickupType = 0x38u;
        constexpr uint32_t kPickupEffects = 0x001DF618u;
        constexpr uint32_t kMoneyEffects = 0x001E0E08u; // CPickups::DoMoneyEffects(entity a0): cash's spin, from the render
        constexpr uint32_t kPickupMoney = 8u, kPickupDropped = 4u;
        constexpr int kFirstWeaponModel = 331, kLastWeaponModel = 368; // knuckles .. land mine (game-internals.md 12g)
        constexpr float kPickupHeight = 0.6f;  // a prop's pickup position this far over it (where the game had it)
        constexpr uint32_t kPropSettleMs = 500; // a prop still this long keeps its place without physics
        constexpr uint32_t kPropUnseenMs = 3000; // ... as does one not drawn this long
        constexpr size_t kMaxProps = 64;        // moving at once; more keep the game's spinning pickups

        // The push of a killing blow (kg m/s) by weapon type (game-internals.md 12d), after SA's Jolt Physics mod's
        // DeathPush column: blunt weapons a shove, blades little, guns by calibre, shotguns the most; thrown weapons and
        // rockets none (their explosion moves the ped itself).
        constexpr float kKillPush[40] = {
            25, 25, 10, 30, 25, 10, 40, 30, 15, 15, 15, 10, // 0 unarmed .. 11 chainsaw
            0, 0, 0, 0, 0, 0, 0,                            // 12 grenade .. 18 rocket
            70, 105, 125, 125, 115,                         // 19 Colt45, 20 Python, 21 Shotgun, 22 Spas12, 23 Stubby
            60, 60, 55, 65, 70, 85,                         // 24 Tec9, 25 Uzi, 26 SilencedIngram, 27 Mp5, 28 m4, 29 Ruger
            100, 100, 0, 0, 100, 100, 0, 120, 0, 0};        // 30, 31 sniper rifles, 34 M60, 35 Minigun, 37 HeliCannon
        constexpr int kShotguns[] = {21, 22, 23};

        // A bullet on a body already down (kg m/s; a shotgun's per pellet), after SA's mod's PhysicsForce x its
        // CorpseBulletReactionIntensity (2).
        constexpr float kCorpsePush[40] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // melee
            0, 0, 0, 0, 0, 0, 0,                // thrown, rocket
            50, 90, 45, 45, 45,                 // Colt45, Python, the shotguns (a pellet)
            40, 40, 35, 45, 60, 70,             // Tec9, Uzi, SilencedIngram, Mp5, m4, Ruger
            120, 120, 0, 0, 90, 90, 0, 150, 0, 0};
        constexpr float kBlastSpeed = 12.0f; // m/s at a power-300 blast's centre (a grenade, a rocket, a car), down to 0 at its edge

        struct Death
        {
            int generation = 0;
            float velocity[3] = {}; // m/s at SetDie
            uint32_t frame = 0;
        };

        // The last blow a ped took (InflictDamage's arguments): what pushes its ragdoll when it was the one that killed.
        struct Hit
        {
            uint32_t attacker = 0;
            int weapon = -1, piece = 0; // piece: VC's 0 torso, 1 mid, 2 left arm, 3 right arm, 4 left leg, 5 right leg, 6 head
            uint32_t frame = 0;
            bool vehicle = false;       // the attacker is a vehicle: its speed then (m/s; the hit slows it)
            float speed[3] = {};
            float pedSpeed[3] = {};     // the ped's own before the blow (the game knocks it back after)
        };

        // A recent blast: it throws the ragdolls there, and those its damage makes in the next frames.
        struct Blast
        {
            float centre[3] = {}, radius = 0.0f, speed = 0.0f;
            uint32_t frame = 0;
        };

        struct Obstacle
        {
            int id = 0;          // physics'
            int generation = 0, model = -1;
            uint32_t seen = 0;   // the frame it was last near a ragdoll
        };

        struct Doll
        {
            int id = 0;              // physics' (0: frozen - its last pose is kept)
            int generation = 0;      // its pool slot's, at death
            uint32_t frame = 0;      // made
            physics::BoneMatrix bones[kBones];
            float followX = 0.0f, followY = 0.0f; // the ped's position as last written
            float startZ = 0.0f;     // its hips when it was made
            // a rider knocked off its bike, alive: it gets up once its body has settled (a death makes it a dead one)
            bool knockdown = false;
            uint32_t bike = 0;       // its bike: no obstacle for it (it starts astride it)
            uint32_t since = 0, still = 0; // CTimer ms: made; its bodies asleep since (0: moving)
            float restAt[3] = {};    // its hips where they last moved from (a body pressed into a wall jitters awake)
            uint32_t restSince = 0;  // CTimer ms they have stayed within 3 cm of it
            // catching the fall: the arms reach toward the ground the body falls to until it lands (landed: ms)
            float fallFrom[3] = {};  // its hips at the last step
            uint32_t landed = 0;
            bool bracing = false;
            // firing back from the ground at whoever shot it down
            uint32_t shooter = 0;
            int shots = 0;
            uint32_t nextShot = 0;
            bool fireNow = false;
            // dismemberment: the bones taken off (a bit each), the stumps they hang from, the blood still to come
            uint32_t severed = 0;
            int stumps = 0, stumpBone[6] = {}, stumpParent[6] = {};
            bool stumpKept[6] = {};   // the part goes on whole, a loose body (a head shot off: Keep severed heads) ...
            float stumpReach[6] = {}; // ... and the stump is this far along its parent bone (the neck's length)
            uint32_t bleedUntil = 0, nextDrop = 0;
            int burst = 0;
            bool bloodNow = false;           // a drop at the next ApplyMoveSpeed ...
            float dropAt[3] = {}, dropDir[3] = {}; // ... from here, this way (m per 1/50 s)
            // a hit reaction: alive and animated, its hips the animation's, its pose blended back into it
            bool reaction = false;
            uint32_t hitFrame = 0;   // the hit that pushed it last
            bool hasTarget = false;
            physics::BoneMatrix target[kBones]; // the animation's pose at the last draw
            float lastHips[3] = {};  // the animation's hips then ...
            uint32_t lastDraw = 0;   // ... (CTimer ms)
            bool running = false;    // the animation ran off (faster than kReactRunSpeed) at runFrom
            uint32_t runFrom = 0;
            int clutchArm = -1, clutchBone = 0; // the hand reaching (its upper arm, 8 / 13; -1 none) for the bone hit ...
            float clutchLocal[3] = {};          // ... at this point in that bone's frame
            uint32_t clutchSince = 0;
            float clutchBest = 0.0f;            // the nearest the hand has come (m), held
            uint32_t hitTimes[3] = {}, hitCount = 0; // its last hits (CTimer ms)
            uint32_t stumbleSince = 0, stumbleMs = 0; // a stumble (0: none)
            int stumbleSteps = 0;
            bool stumbleLeft = false;           // the first step's leg
            float stumbleDir[2] = {}, stumbleDist = 0.0f, stumbleDone = 0.0f;
        };

        // A rider knocked off its bike, until its next draw makes its ragdoll.
        struct Knock
        {
            int generation = 0;
            uint32_t bike = 0, time = 0; // CTimer ms
            int weapon = -1;
            uint32_t rammer = 0;         // another vehicle that hit the rider (InflictDamage's attacker), its speed then
            float rammerSpeed[3] = {};
        };

        // A gun or wad of cash lying where the physics dropped it: a pickup's object (its model's frame, no spin or glow).
        struct Prop
        {
            int id = 0;            // physics' while it moves (0: settled)
            uint32_t slot = 0;     // its pickup
            physics::BoneMatrix rest; // where it is
            uint32_t still = 0, seen = 0; // CTimer ms: asleep since (0: moving); last drawn
        };

        // A ped getting up from its knockdown ragdoll: its body's last pose eased into the game's get-up.
        struct Blend
        {
            int generation = 0;
            physics::BoneMatrix bones[kBones];
            uint32_t released = 0, start = 0; // CTimer ms: let go; the get-up seen (0: not yet)
        };

        // A col model made into a shape, kept while its data stays the same (streamed collision goes and comes back).
        struct ColShape
        {
            int id = 0;        // physics' (0: nothing usable in it)
            uint64_t key = 0;  // what it held (colKey)
            uint32_t used = 0; // CTimer ms it was last wanted
        };

        // A building or object near a ragdoll, as a body in the physics.
        struct WorldBody
        {
            int id = 0;          // physics'
            uint32_t col = 0;    // its col model ...
            uint64_t key = 0;    // ... and what that held
            bool moving = false; // an object (kinematic, moved with the game's), else a building (static)
            float at[3] = {};    // where it was put (a building elsewhere is another one: made again)
            uint32_t seen = 0;
        };

        std::atomic<bool> g_enabled{false};
        std::atomic<bool> g_bikeFalls{true}; // Bike falls (with Ragdolls on)
        std::atomic<bool> g_pickupsOn{false}; // Pickups on the ground
        std::atomic<bool> g_hitReactions{true}; // Hit reactions (with Ragdolls on)
        std::atomic<bool> g_catchFall{true};    // Catch falls (with Ragdolls on)
        std::atomic<bool> g_blindFire{true};    // Fire back from the ground (with Ragdolls on)
        std::atomic<bool> g_gore{false};        // Dismemberment (with Ragdolls on)
        std::atomic<bool> g_keepHeads{true};    // ... a head shot off rolls away whole instead of bursting
        uint32_t g_goreSeed = 12345u;
        std::atomic<int> g_force{100}; // Hit force, percent
        bool g_worldOn = true;         // VCS_JOLT_WORLD=0: no world collision (each ragdoll on a ground of its own)
        VcsGuestFn g_origSetDie = nullptr, g_origMove = nullptr, g_origAfterWorld = nullptr,
                                       g_origTree = nullptr, g_origAfterBones = nullptr, g_origInflict = nullptr,
                                       g_origLineOfSight = nullptr, g_origExplosion = nullptr, g_origKnockOff = nullptr, g_origFire = nullptr, g_origParticle = nullptr,
                                       g_origPickupEffects = nullptr, g_origMoneyEffects = nullptr;
        VcsGuestFn g_origElementRender = nullptr;
        bool g_secondDrawOk = false;  // kElementAgain / kPoolMade registered, the callback hooked
        uint32_t g_pool = 0;          // kPoolSlots slots in the main heap (64-byte aligned), once made
        bool g_poolTried = false;
        uint32_t g_poolFrame = 0, g_poolUsed = 0; // slots handed out in this frame
        // the callback being called twice: 1 for the element, 2 for its copy (-1 the allocation); where it returns to
        int g_drawStage = 0;
        uint32_t g_drawElement = 0, g_drawCopy = 0, g_drawReturn = 0;
        unsigned g_looseDraws = 0;

        // game thread
        std::unordered_map<uint32_t, Death> g_pending; // ped -> its death, until its next draw
        std::unordered_map<uint32_t, Knock> g_knocked; // rider -> its fall off the bike, until its next draw
        std::unordered_map<uint32_t, Blend> g_blends;  // ped -> its get-up from a knockdown ragdoll
        std::unordered_map<uint32_t, Doll> g_dolls;    // ped -> its ragdoll
        std::unordered_map<uint32_t, Prop> g_props;    // pickup object -> its prop
        std::unordered_map<uint32_t, uint32_t> g_trees; // ped -> its anim tree (noted / ragdolled peds), last draw
        std::unordered_map<uint32_t, Hit> g_hits;       // ped -> the last blow it took
        std::unordered_map<uint32_t, Obstacle> g_obstacles; // vehicle -> its box in the physics, while near a ragdoll
        std::unordered_map<uint32_t, ColShape> g_colShapes; // col model -> its shape
        std::unordered_map<uint32_t, WorldBody> g_world;    // building / object -> its body, while near a ragdoll
        std::vector<uint32_t> g_checked;                    // the entities a world() pass looked at
        std::vector<Blast> g_blasts;                        // the last few blasts
        unsigned g_steps = 0, g_created = 0, g_removed = 0, g_corpseHits = 0, g_blasted = 0, g_shapesMade = 0;
        unsigned g_knocks = 0, g_knockdowns = 0, g_gotUp = 0, g_propsMade = 0, g_reactions = 0, g_shotDown = 0;
        unsigned g_braced = 0, g_blindShots = 0, g_severed = 0, g_bloodDrops = 0;
        unsigned g_clutches = 0, g_stumbles = 0;
        float g_clutchGap = -1.0f, g_lastStumble = 0.0f; // the last clutch's hand-to-wound gap held, the last stumble's ground
        float g_reactDeviation = 0.0f; // the farthest a reacting body has been drawn off its animation (m), since the last
                                       // reaction began
        uint32_t g_newest = 0;   // the last ped given a ragdoll

        // for the control channel's "jolt" (another thread)
        struct Status
        {
            int dolls = 0, frozen = 0, pending = 0, obstacles = 0, world = 0, shapes = 0;
            unsigned steps = 0, created = 0, removed = 0, corpseHits = 0, blasted = 0, shapesMade = 0;
            unsigned knocks = 0, knockdowns = 0, gotUp = 0, propsMade = 0, reactions = 0, shotDown = 0, braced = 0, blindShots = 0,
                     severed = 0, bloodDrops = 0;
            int reacting = 0;
            float reactDeviation = 0.0f, clutchGap = 0.0f, lastStumble = 0.0f;
            unsigned clutches = 0, stumbles = 0;
            int props = 0, propsMoving = 0;
            uint32_t ped = 0, tree = 0;
            bool awake = false, down = false; // the newest ragdoll's: moving; a knockdown
            float pelvis[16] = {}; // the newest ragdoll's pelvis bone (1) as written
            physics::Stats physics;
        };
        std::mutex g_statusMutex;
        Status g_status;

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        uint32_t rd16(const uint8_t *ram, uint32_t a)
        {
            uint16_t v;
            std::memcpy(&v, ram + (a & kRamMask), 2);
            return v;
        }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
        void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & kRamMask), &v, 4); }

        // The ped's pool slot generation (the flag byte's low 7 bits; 0x80 = free), or -1 when it is not a used slot.
        int generation(const uint8_t *ram, uint32_t ped)
        {
            const uint32_t pool = rd32(ram, kPedPool);
            if (!pool)
                return -1;
            const uint32_t base = rd32(ram, pool), flags = rd32(ram, pool + 4), size = rd32(ram, pool + 8);
            if (ped < base || (ped - base) % kPedSize || (ped - base) / kPedSize >= size)
                return -1;
            const uint8_t f = ram[(flags + (ped - base) / kPedSize) & kRamMask];
            return f & 0x80 ? -1 : f & 0x7F;
        }

        // A used vehicle pool slot's entity.
        bool isVehicle(const uint8_t *ram, uint32_t e)
        {
            const uint32_t pool = rd32(ram, kVehiclePool);
            if (!pool || !e)
                return false;
            const uint32_t base = rd32(ram, pool), flags = rd32(ram, pool + 4), size = rd32(ram, pool + 8);
            if (e < base || (e - base) % kVehicleSize || (e - base) / kVehicleSize >= size)
                return false;
            return !(ram[(flags + (e - base) / kVehicleSize) & kRamMask] & 0x80);
        }

        float length(const float *v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

        // Still the ped that died: its slot not freed or reused, dying or dead.
        bool stillDead(const uint8_t *ram, uint32_t ped, int gen)
        {
            if (generation(ram, ped) != gen)
                return false;
            const uint32_t state = rd32(ram, ped + kPedState);
            return state == kStateDying || state == kStateDead;
        }

        // Still the ped the ragdoll was made for, as it was: dying or dead - or, knocked off its bike, still down; a hit
        // reaction's alive, on foot, not falling.
        bool stillDown(const uint8_t *ram, uint32_t ped, const Doll &doll)
        {
            if (doll.reaction)
            {
                const uint32_t state = rd32(ram, ped + kPedState);
                return generation(ram, ped) == doll.generation && state != kStateDying && state != kStateDead &&
                       state != kStateFall && !rd32(ram, ped + kPedVehicle);
            }
            return stillDead(ram, ped, doll.generation) ||
                   (doll.knockdown && generation(ram, ped) == doll.generation && rd32(ram, ped + kPedState) == kStateFall);
        }

        // The tree's hierarchy from its node flags: -1 for the root.
        void hierarchy(const uint8_t *ram, uint32_t info, int *parents)
        {
            int stack[kBones], depth = 0, parent = -1;
            for (int i = 0; i < kBones; ++i)
            {
                const uint32_t flags = (rd32(ram, info + i * 8u) >> 16) & 0xFFu;
                parents[i] = parent;
                if ((flags & kNodePush) && depth < kBones)
                    stack[depth++] = parent;
                parent = (flags & kNodePop) ? (depth ? stack[--depth] : -1) : i;
            }
        }

        // The ragdoll's pose into the tree's matrices (rows X, Y, Z and position; their w lanes are the game's).
        void write(uint8_t *ram, uint32_t tree, const physics::BoneMatrix *bones)
        {
            const uint32_t matrices = rd32(ram, tree + kTreeMatrices);
            if (!matrices || rd32(ram, tree + kTreeNodes) != uint32_t(kBones))
                return;
            for (int i = 0; i < kBones; ++i)
                for (uint32_t r = 0; r < 4; ++r)
                    std::memcpy(ram + ((matrices + i * 0x40u + r * 0x10u) & kRamMask), bones[i].m + r * 4, 12);
        }

        void publish()
        {
            Status st;
            for (const auto &[ped, doll] : g_dolls)
                ++(doll.id ? st.dolls : st.frozen);
            st.pending = int(g_pending.size());
            st.obstacles = int(g_obstacles.size());
            st.world = int(g_world.size());
            st.shapes = int(g_colShapes.size());
            st.shapesMade = g_shapesMade;
            st.corpseHits = g_corpseHits;
            st.blasted = g_blasted;
            st.steps = g_steps;
            st.created = g_created;
            st.removed = g_removed;
            st.knocks = g_knocks;
            st.knockdowns = g_knockdowns;
            st.gotUp = g_gotUp;
            st.propsMade = g_propsMade;
            st.reactions = g_reactions;
            st.shotDown = g_shotDown;
            st.braced = g_braced;
            st.blindShots = g_blindShots;
            st.severed = g_severed;
            st.bloodDrops = g_bloodDrops;
            st.reactDeviation = g_reactDeviation;
            st.clutches = g_clutches;
            st.stumbles = g_stumbles;
            st.clutchGap = g_clutchGap;
            st.lastStumble = g_lastStumble;
            for (const auto &[ped, doll] : g_dolls)
                st.reacting += doll.reaction;
            st.props = int(g_props.size());
            for (const auto &[object, prop] : g_props)
                st.propsMoving += prop.id != 0;
            if (const auto it = g_dolls.find(g_newest); it != g_dolls.end())
            {
                st.ped = g_newest;
                const auto tree = g_trees.find(g_newest);
                st.tree = tree != g_trees.end() ? tree->second : 0;
                st.awake = it->second.id && physics::awake(it->second.id);
                st.down = it->second.knockdown;
                std::copy(it->second.bones[1].m, it->second.bones[1].m + 16, st.pelvis);
            }
            st.physics = physics::stats();
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status = st;
        }

        void remove(uint8_t *ram, std::unordered_map<uint32_t, Doll>::iterator it, const char *why)
        {
            if (it->second.id)
                physics::destroy(it->second.id);
            // a knocked-down ped still down is let up (its get-up was held back while it had its ragdoll)
            if (it->second.knockdown && generation(ram, it->first) == it->second.generation &&
                rd32(ram, it->first + kPedState) == kStateFall)
            {
                wr32(ram, it->first + kPedGetUpTime, rd32(ram, kTimeMs));
                for (uint32_t k = 0; k < 3; ++k)
                    wrf(ram, it->first + kPedMoveSpeed + 4 * k, 0.0f);
            }
            std::fprintf(stderr, "[jolt] ped %08X: ragdoll removed (%s)\n", it->first, why);
            g_trees.erase(it->first);
            g_dolls.erase(it);
            ++g_removed;
        }

        // A killing shot or blow from a ped: a push from the attacker's side on the part hit, the trunk taking the rest
        // (shotguns: the whole trunk, harder close up), level and a little up. The push's size, 0 when none.
        float blow(const uint8_t *ram, int id, const Hit &hit, const physics::BoneMatrix *bones, float force)
        {
            if (hit.weapon < 0 || hit.weapon >= int(std::size(kKillPush)) || kKillPush[hit.weapon] <= 0.0f ||
                generation(ram, hit.attacker) < 0)
                return 0.0f;
            struct Piece
            {
                int part, rest;
                float share; // the part's; the rest's is 1 - share
            };
            static constexpr Piece kPieces[7] = {
                {physics::kChest, physics::kHips, 0.7f},     {physics::kHips, physics::kChest, 0.7f},
                {physics::kUpperArmL, physics::kChest, 0.3f}, {physics::kUpperArmR, physics::kChest, 0.3f},
                {physics::kThighL, physics::kHips, 0.5f},    {physics::kThighR, physics::kHips, 0.5f},
                {physics::kHead, physics::kChest, 0.25f}};
            const bool shotgun = std::find(std::begin(kShotguns), std::end(kShotguns), hit.weapon) != std::end(kShotguns);
            const Piece &piece = kPieces[shotgun || hit.piece < 0 || hit.piece > 6 ? 0 : hit.piece];
            const float *chest = bones[3].m + 12; // spine1
            float d[3] = {chest[0] - rdf(ram, hit.attacker + kPedPosition), chest[1] - rdf(ram, hit.attacker + kPedPosition + 4),
                          0.0f};
            const float distance = std::sqrt(d[0] * d[0] + d[1] * d[1]);
            if (distance < 0.05f)
                return 0.0f;
            d[0] /= distance;
            d[1] /= distance;
            d[2] = shotgun ? 0.25f : 0.15f;
            float push = kKillPush[hit.weapon] * force;
            if (shotgun)
                push *= std::clamp(1.6f - 0.06f * distance, 1.0f, 1.6f);
            float a[3], b[3];
            for (int k = 0; k < 3; ++k)
            {
                a[k] = d[k] * push * piece.share;
                b[k] = d[k] * push * (1.0f - piece.share);
            }
            physics::push(id, piece.part, a);
            physics::push(id, piece.rest, b);
            return push;
        }

        // Run down by a vehicle: the bumper takes the legs at its speed and lifts them, the trunk and head follow less,
        // so the body folds back onto the bonnet; the vehicle's box in the physics carries and throws it from there.
        void runDown(int id, const Hit &hit, float force)
        {
            //                                                hips  chest  head   arms ...................  legs .....................
            static constexpr float kShare[physics::kPartTotal] = {0.6f, 0.25f, 0.15f, 0.3f, 0.3f, 0.3f, 0.3f, 0.9f, 1.0f, 0.9f, 1.0f};
            static constexpr float kLift[physics::kPartTotal] = {0.15f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.25f, 0.3f, 0.25f, 0.3f};
            const float speed = length(hit.speed);
            for (int p = 0; p < physics::kPartTotal; ++p)
            {
                const float dv[3] = {hit.speed[0] * kShare[p] * force, hit.speed[1] * kShare[p] * force,
                                     (hit.speed[2] * kShare[p] + speed * kLift[p]) * force};
                physics::kick(id, p, dv);
            }
        }

        // The vehicle (not its own bike) whose hit knocked a rider off, from InflictDamage's note of this frame or
        // the last few: the attacker and its speed then. A ped's own collision with a vehicle (FUN_001db5f8 /
        // FUN_001dc3e8: weapon 47) does not go through InflictDamage: then the fastest vehicle moving faster than
        // kRammerSpeed within kRammerReach of the rider.
        void rammedBy(const uint8_t *ram, uint32_t rider, Knock &k)
        {
            const auto hit = g_hits.find(rider);
            if (hit != g_hits.end() && hit->second.vehicle && hit->second.attacker != k.bike &&
                rd32(ram, kFrameCounter) - hit->second.frame <= kHitFrames)
            {
                k.rammer = hit->second.attacker;
                std::copy(hit->second.speed, hit->second.speed + 3, k.rammerSpeed);
                return;
            }
            const uint32_t pool = rd32(ram, kVehiclePool);
            const uint32_t base = pool ? rd32(ram, pool) : 0, flags = pool ? rd32(ram, pool + 4) : 0,
                           size = pool ? rd32(ram, pool + 8) : 0;
            float fastest = kRammerSpeed;
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint32_t v = base + i * kVehicleSize;
                if (ram[(flags + i) & kRamMask] & 0x80 || v == k.bike)
                    continue;
                float d[3], speed[3];
                for (uint32_t c = 0; c < 3; ++c)
                {
                    d[c] = rdf(ram, v + kPedPosition + 4 * c) - rdf(ram, rider + kPedPosition + 4 * c);
                    speed[c] = rdf(ram, v + kVehicleMoveSpeed + 4 * c) * 50.0f;
                }
                if (length(d) < kRammerReach && length(speed) > fastest)
                {
                    fastest = length(speed);
                    k.rammer = v;
                    std::copy(speed, speed + 3, k.rammerSpeed);
                }
            }
        }

        float goreRandom();

        // A rider thrown off a bike, after SA's (its riders tumble off, they are not launched): the game's throw is made
        // for an animation - the bike's speed and up to 11 m/s up, one block, so the body would sail 5 m high still in
        // its riding pose. Rammed by a vehicle: swept up by it - the legs at its speed, the trunk and head less, lifted a
        // little -, so it folds and rolls over the bonnet (the vehicle is in the physics). Else (a crash, a shot, a tyre)
        // over the handlebars: on along the throw's way, the legs held back by the bike and the trunk and head on, so it
        // pitches head first; up no more than a few m/s. The arms flail (a random push each).
        void throwOff(int id, const float game[3], const Knock &k, float force)
        {
            using namespace physics;
            //                                             hips  chest  head  arms ...................  legs ....................
            static constexpr float kRammed[kPartTotal] = {0.8f, 0.6f, 0.5f, 0.6f, 0.6f, 0.6f, 0.6f, 1.0f, 1.0f, 1.0f, 1.0f};
            static constexpr float kRammedLift[kPartTotal] = {0.15f, 0.08f, 0.05f, 0.0f, 0.0f, 0.0f, 0.0f, 0.2f, 0.2f, 0.2f, 0.2f};
            static constexpr float kPitch[kPartTotal] = {0.85f, 1.0f, 1.05f, 1.0f, 1.0f, 1.0f, 1.0f, 0.65f, 0.55f, 0.65f, 0.55f};
            static constexpr float kPitchLift[kPartTotal] = {1.0f, 1.1f, 1.1f, 1.0f, 1.0f, 1.0f, 1.0f, 0.7f, 0.6f, 0.7f, 0.6f};
            const float rammed = std::hypot(k.rammerSpeed[0], k.rammerSpeed[1]);
            const bool swept = k.rammer && rammed > 3.0f;
            float along[3], up;
            if (swept)
            {
                along[0] = k.rammerSpeed[0], along[1] = k.rammerSpeed[1], along[2] = 0.0f;
                up = std::min(rammed * 0.2f, kThrowUp);
            }
            else
            {
                const float h = std::hypot(game[0], game[1]), keep = h > kThrowMax ? kThrowMax / h : 1.0f;
                along[0] = game[0] * keep * 0.9f, along[1] = game[1] * keep * 0.9f, along[2] = 0.0f;
                up = std::clamp(game[2], 0.0f, std::min(1.5f + 0.15f * h, kThrowUp));
            }
            const float flail = std::min(1.5f, std::hypot(along[0], along[1]) / 10.0f);
            for (int p = 0; p < kPartTotal; ++p)
            {
                const float share = swept ? kRammed[p] : kPitch[p];
                const float lift = swept ? kRammedLift[p] * rammed : kPitchLift[p] * up;
                float dv[3] = {along[0] * share, along[1] * share, std::min(lift, kThrowUp)};
                if (p >= kUpperArmL && p <= kForearmR)
                    for (float &v : dv)
                        v += (goreRandom() - 0.5f) * 2.0f * flail;
                for (float &v : dv)
                    v *= force;
                kick(id, p, dv);
            }
        }

        // Dismemberment (game-internals.md 12p): a part taken off a body - its bones drawn collapsed onto the joint it
        // hangs from (the stump: the skin weighted to them shrinks into it), its bodies no longer meeting anything, blood
        // from the stump. By weapon (VCS's types; SA's Jolt mod's JoltPhysics.dat SeverChance / HeadBurst columns mapped
        // onto them): the chance a lethal hit on a limb (or a shot on a body down) takes it off, and a head hit the head.
        struct Sever
        {
            float limb, head;
        };
        constexpr Sever kSever[40] = {
            {0, 0}, {0, 0}, {0, 0}, {0.03f, 0}, {0.02f, 0}, {0.35f, 0}, {0.04f, 0}, {0.03f, 0}, // 0 unarmed .. 7 hammer
            {0.6f, 0}, {0.8f, 0}, {0.85f, 0}, {1.0f, 0},                                        // cleaver, machete, katana, chainsaw
            {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},                              // thrown, rocket: their blast
            {0.02f, 0}, {0.35f, 1.0f}, {0.7f, 1.0f}, {0.75f, 1.0f}, {0.65f, 1.0f},               // Colt45, Python, shotguns
            {0.01f, 0}, {0.01f, 0}, {0.01f, 0}, {0.015f, 0}, {0.2f, 1.0f}, {0.2f, 1.0f},         // Tec9 .. Ruger
            {0.6f, 1.0f}, {0.6f, 1.0f}, {0, 0}, {0, 0}, {0.3f, 1.0f}, {0.4f, 1.0f}, {0, 0}, {1.0f, 1.0f}, {0, 0}, {0, 0}};
        constexpr float kBlastSever = 0.6f;   // a limb's chance at a blast's centre (less farther out), 2 a body at most
        constexpr uint32_t kBleedMs = 3000;   // a stump bleeds this long ...
        constexpr uint32_t kBleedGapMs = 45;  // ... a drop every this (~22 a second) ...
        constexpr int kBurst = 14, kHeadBurst = 30; // ... after a burst of this many (one a frame)

        // The part a hit's piece is (VC's pieces: 2 / 3 left / right arm, 4 / 5 legs, 6 head) - an arm or leg its whole
        // or its lower half at random -, or -1.
        int piecePart(int piece, uint32_t roll)
        {
            switch (piece)
            {
            case 2: return roll & 1 ? physics::kUpperArmL : physics::kForearmL;
            case 3: return roll & 1 ? physics::kUpperArmR : physics::kForearmR;
            case 4: return roll & 1 ? physics::kThighL : physics::kCalfL;
            case 5: return roll & 1 ? physics::kThighR : physics::kCalfR;
            case 6: return physics::kHead;
            default: return -1;
            }
        }

        // The bones a part's loss takes (its bone and the ones below it in the ped's skeleton) and the one they hang from.
        uint32_t severedBones(int part, int &root)
        {
            switch (part)
            {
            case physics::kHead: root = 5; return 0x60u;                      // head, head end
            case physics::kUpperArmL: root = 8; return 0xF00u;                // upper arm .. fingers
            case physics::kForearmL: root = 9; return 0xE00u;
            case physics::kUpperArmR: root = 13; return 0x1E000u;
            case physics::kForearmR: root = 14; return 0x1C000u;
            case physics::kThighL: root = 17; return 0x1E0000u;               // thigh .. toes
            case physics::kCalfL: root = 18; return 0x1C0000u;
            case physics::kThighR: root = 21; return 0x1E00000u;
            case physics::kCalfR: root = 22; return 0x1C00000u;
            default: root = -1; return 0;
            }
        }

        bool goreOn(const uint8_t *ram)
        {
            return g_gore.load(std::memory_order_relaxed) && ram[kNastyGame & kRamMask] != 0;
        }

        float goreRandom()
        {
            g_goreSeed = g_goreSeed * 1664525u + 1013904223u;
            return float(g_goreSeed >> 8) / float(1u << 24);
        }

        // `part` taken off the body (a second loss of the same bones does nothing): drawn as a stump, bleeding.
        bool sever(Doll &d, uint32_t ped, int part, uint32_t ms, const char *why)
        {
            int root = -1;
            const uint32_t bones = severedBones(part, root);
            if (!bones || (d.severed & bones) == bones || d.stumps >= int(std::size(d.stumpBone)))
                return false;
            d.severed |= bones;
            d.stumpBone[d.stumps] = root;
            d.stumpParent[d.stumps] = kPedParentBone[root];
            // a head kept whole: let go of the neck and thrown up and a little aside, the stump where the neck ends
            const bool keep = d.id && part == physics::kHead && g_keepHeads.load(std::memory_order_relaxed) && g_secondDrawOk;
            d.stumpKept[d.stumps] = keep;
            if (keep)
            {
                const float *head = d.bones[root].m + 12, *neck = d.bones[kPedParentBone[root]].m + 12;
                const float along[3] = {head[0] - neck[0], head[1] - neck[1], head[2] - neck[2]};
                d.stumpReach[d.stumps] = length(along);
                const float throwOff[3] = {(goreRandom() - 0.5f) * 2.0f, (goreRandom() - 0.5f) * 2.0f, 2.0f + goreRandom()};
                physics::detach(d.id, part, throwOff);
            }
            else if (d.id)
                physics::sever(d.id, part);
            ++d.stumps;
            d.bleedUntil = ms + kBleedMs;
            d.burst += part == physics::kHead ? kHeadBurst : kBurst;
            ++g_severed;
            std::fprintf(stderr, "[jolt] ped %08X: %s (bone %d) taken off%s - %s\n", ped,
                         part == physics::kHead ? "head" : "limb", root, keep ? " whole" : "", why);
            return true;
        }

        // A killing hit on a limb or the head: off by the weapon's chance.
        void severByHit(Doll &d, uint32_t ped, int weapon, int piece, uint32_t ms)
        {
            if (weapon < 0 || weapon >= int(std::size(kSever)))
                return;
            // tests: VCS_JOLT_HEADSHOTS=1 - every killing hit takes the head
            static const bool headshots = [] { const char *e = std::getenv("VCS_JOLT_HEADSHOTS"); return e && e[0] == '1'; }();
            const int part = headshots ? int(physics::kHead) : piecePart(piece, g_goreSeed >> 4);
            if (part < 0)
                return;
            const float chance = headshots ? 1.0f : part == physics::kHead ? kSever[weapon].head : kSever[weapon].limb;
            if (chance > 0.0f && goreRandom() < chance)
                sever(d, ped, part, ms, part == physics::kHead ? "head burst" : "hit");
        }

        // A blast's limbs: each by kBlastSever falling off with distance from its centre, 2 a body at most (heads half).
        void severByBlast(Doll &d, uint32_t ped, const float *centre, float radius, uint32_t ms)
        {
            static constexpr int kLimbs[] = {physics::kUpperArmL, physics::kForearmL, physics::kUpperArmR, physics::kForearmR,
                                             physics::kThighL,    physics::kCalfL,    physics::kThighR,    physics::kCalfR,
                                             physics::kHead};
            static constexpr int kLimbBone[] = {8, 9, 13, 14, 17, 18, 21, 22, 5};
            int taken = 0;
            for (size_t i = 0; i < std::size(kLimbs) && taken < 2; ++i)
            {
                const float *at = d.bones[kLimbBone[i]].m + 12;
                const float off[3] = {at[0] - centre[0], at[1] - centre[1], at[2] - centre[2]};
                const float reach = std::max(0.0f, 1.0f - length(off) / radius);
                const float chance = kBlastSever * reach * (kLimbs[i] == physics::kHead ? 0.5f : 1.0f);
                if (chance > 0.0f && goreRandom() < chance && sever(d, ped, kLimbs[i], ms, "blast"))
                    ++taken;
            }
        }

        // Where a stump is: the joint its part hung from - for a part gone on whole (elsewhere now), the end of the bone it
        // hung from (a bone's X runs along it).
        void stumpJoint(const Doll &d, int s, const physics::BoneMatrix *in, float *joint)
        {
            const float *at = in[d.stumpBone[s]].m + 12;
            if (d.stumpKept[s])
            {
                const float *parent = in[d.stumpParent[s]].m;
                for (int k = 0; k < 3; ++k)
                    joint[k] = parent[12 + k] + parent[k] * d.stumpReach[s];
            }
            else
                std::copy(at, at + 3, joint);
        }

        bool inPart(int bone, int top)
        {
            while (bone >= 0 && bone != top)
                bone = kPedParentBone[bone];
            return bone == top;
        }

        // The pose as drawn: a stump's bones collapsed onto the joint they hang from. A part gone on whole is drawn by a
        // second render of the ped (pedRender, `loose`): there only that part, where its body is - every other bone
        // collapsed onto the part's own joint - so the skin weighted across the cut closes up at both ends instead of
        // stretching from the body to the part.
        void stumped(const Doll &d, const physics::BoneMatrix *in, physics::BoneMatrix *out, bool loose = false)
        {
            std::copy(in, in + kBones, out);
            if (!d.severed)
                return;
            if (loose)
            {
                int top = -1;
                for (int s = 0; s < d.stumps && top < 0; ++s)
                    if (d.stumpKept[s])
                        top = d.stumpBone[s];
                if (top < 0)
                    return;
                const float *joint = in[top].m + 12;
                for (int i = 0; i < kBones; ++i)
                {
                    if (inPart(i, top))
                        continue;
                    std::fill(out[i].m, out[i].m + 12, 0.0f);
                    std::copy(joint, joint + 3, out[i].m + 12);
                }
                return;
            }
            for (int s = 0; s < d.stumps; ++s)
            {
                float joint[3];
                stumpJoint(d, s, in, joint);
                for (int i = 0; i < kBones; ++i)
                {
                    int up = i;
                    while (up >= 0 && up != d.stumpBone[s])
                        up = kPedParentBone[up];
                    if (up != d.stumpBone[s])
                        continue;
                    std::fill(out[i].m, out[i].m + 12, 0.0f);
                    out[i].m[12] = joint[0];
                    out[i].m[13] = joint[1];
                    out[i].m[14] = joint[2];
                }
            }
        }

        // Once a step: a stump's next drop of blood (out of the stump, along the bone it hangs from), for applyMoveSpeed.
        void bleed(Doll &d, uint32_t ms)
        {
            if (!d.stumps || d.bloodNow || (!d.burst && ms >= d.bleedUntil) || (!d.burst && ms < d.nextDrop))
                return;
            const int s = int(g_goreSeed % uint32_t(d.stumps));
            float joint[3];
            stumpJoint(d, s, d.bones, joint);
            const float *parent = d.bones[d.stumpParent[s]].m + 12;
            float out[3] = {joint[0] - parent[0], joint[1] - parent[1], joint[2] - parent[2]};
            const float l = length(out);
            for (int k = 0; k < 3; ++k)
                out[k] = (l > 1e-3f ? out[k] / l : 0.0f) * 0.06f + (goreRandom() - 0.5f) * 0.04f;
            out[2] += d.burst ? 0.04f : 0.01f;
            std::copy(joint, joint + 3, d.dropAt);
            std::copy(out, out + 3, d.dropDir);
            d.bloodNow = true;
            if (d.burst)
                --d.burst;
            d.nextDrop = ms + kBleedGapMs;
        }

        void world(const uint8_t *ram, float seconds, const float *extra = nullptr);

        // A ped noted at its death - or knocked off its bike (`knock`; its death then has its generation) -, drawn: its
        // ragdoll from the bones its render just built.
        void build(uint8_t *ram, uint32_t ped, uint32_t tree, const Death &death, const Knock *knock = nullptr)
        {
            auto refuse = [ped](const char *why) { std::fprintf(stderr, "[jolt] ped %08X: no ragdoll (%s)\n", ped, why); };
            const bool down = knock != nullptr;
            if (down && (generation(ram, ped) != knock->generation || rd32(ram, ped + kPedState) != kStateFall))
                return refuse("not down off its bike any more");
            if (!down && (!stillDead(ram, ped, death.generation) || (rd32(ram, ped + kPedVehicle) && !g_knocked.count(ped))))
                return refuse("not dying any more, or in a vehicle");
            const uint32_t matrices = rd32(ram, tree + kTreeMatrices), info = rd32(ram, tree + kTreeNodeInfo);
            if (rd32(ram, tree + kTreeNodes) != uint32_t(kBones) || !matrices || !info)
                return refuse("not a 25-bone tree");
            physics::BoneMatrix bones[kBones];
            for (int i = 0; i < kBones; ++i)
                std::memcpy(bones[i].m, ram + ((matrices + i * 0x40u) & kRamMask), 64);
            const float px = rdf(ram, ped + kPedPosition), py = rdf(ram, ped + kPedPosition + 4),
                        pz = rdf(ram, ped + kPedPosition + 8);
            const float dx = bones[0].m[12] - px, dy = bones[0].m[13] - py, dz = bones[0].m[14] - pz;
            if (dx * dx + dy * dy + dz * dz > 4.0f)
                return refuse("its bones are not where it is");
            int parents[kBones];
            hierarchy(ram, info, parents);
            physics::RagdollDesc desc;
            desc.bones = bones;
            desc.parents = parents;
            // the blow that killed it (InflictDamage calls SetDie), if one did
            const uint32_t now = rd32(ram, kFrameCounter);
            const bool fresh = now - death.frame <= kSeedFrames;
            const auto hitIt = g_hits.find(ped);
            const Hit hit = !down && fresh && hitIt != g_hits.end() && now - hitIt->second.frame <= kHitFrames + kSeedFrames
                                ? hitIt->second
                                : Hit{};
            const bool pushed = !hit.vehicle && hit.weapon >= 0 && hit.weapon < int(std::size(kKillPush)) &&
                                kKillPush[hit.weapon] > 0.0f && generation(ram, hit.attacker) >= 0;
            // its speed: when the blow is ours to give (blow), its own walking or running (2 m/s at most): the game
            // knocks it back before InflictDamage - a shotgun 5.6 m/s -, and both would throw it metres; run down, none
            // (runDown gives its parts the vehicle's speed in shares); else the faster of SetDie's and the one the game
            // gave it since (a blast's)
            float later[3], own[3];
            for (uint32_t k = 0; k < 3; ++k)
                later[k] = rdf(ram, ped + kPedMoveSpeed + 4 * k) * 50.0f;
            const float ownScale = std::min(1.0f, 2.0f / std::max(length(hit.pedSpeed), 1e-3f));
            for (int k = 0; k < 3; ++k)
                own[k] = hit.pedSpeed[k] * ownScale;
            // a blast's victim: none either - the game threw it as one block before the damage; the blast's own throw
            // part by part replaces that (below). Any recent blast that reaches it counts: its damage comes as the
            // blast's weapon (a grenade's 12, a car's 49)
            const float *hips = bones[2].m + 12;
            auto reaches = [&](const Blast &b) {
                const float d[3] = {hips[0] - b.centre[0], hips[1] - b.centre[1], hips[2] - b.centre[2]};
                return now - b.frame <= kHitFrames + kSeedFrames && length(d) < b.radius;
            };
            const bool blasted = !down && fresh &&
                                 (hit.weapon == kBlastDamage || std::any_of(g_blasts.begin(), g_blasts.end(), reaches));
            // knocked off: the speed the game gave it leaving the bike (the bike's, and its throw)
            const float *speed = down ? later : pushed ? own : length(later) > length(death.velocity) ? later : death.velocity;
            // (off a bike: none - throwOff gives its parts theirs)
            const bool offBike = down && knock->bike;
            for (int k = 0; k < 3; ++k)
                desc.velocity[k] = offBike ? 0.0f : down || (fresh && !hit.vehicle && !blasted) ? speed[k] : 0.0f;
            const float force = static_cast<float>(g_force.load(std::memory_order_relaxed)) / 100.0f;
            // the ground: the world's collision around it goes into the physics first; where that has no surface under
            // it (collision not streamed in), a ground of its own under the soles of a ped standing at its position (or
            // its lowest bone, if that is lower)
            world(ram, 0.0f, hips);
            const float from[3] = {px, py, pz};
            float surface = 0.0f;
            desc.groundPlane = !g_worldOn || !physics::ground(from, kGroundSearch, surface);
            float lowest = pz;
            for (const physics::BoneMatrix &b : bones)
                lowest = std::min(lowest, b.m[14]);
            desc.groundZ = desc.groundPlane ? std::min(pz - kSoles, lowest - 0.02f) : surface;
            const int id = physics::create(desc);
            if (!id)
                return refuse("the physics refused it - its reason above");
            Doll &doll = g_dolls[ped];
            doll.id = id;
            doll.generation = death.generation;
            doll.frame = rd32(ram, kFrameCounter);
            doll.followX = px;
            doll.followY = py;
            doll.startZ = hips[2];
            doll.knockdown = down;
            doll.bike = down ? knock->bike : 0;
            doll.since = rd32(ram, kTimeMs);
            physics::pose(id, doll.bones);
            write(ram, tree, doll.bones);
            g_newest = ped;
            ++g_created;
            if (down)
            {
                ++g_knockdowns;
                Knock thrown = *knock;
                if (offBike)
                {
                    if (!thrown.rammer)
                        rammedBy(ram, ped, thrown);
                    throwOff(id, later, thrown, force);
                }
                std::fprintf(stderr,
                             "[jolt] ped %08X (model %d): %s %08X (weapon %d): ragdoll %d at (%.2f, %.2f, "
                             "%.2f), (%.2f, %.2f, %.2f) m/s (the game's (%.2f, %.2f, %.2f), rammed by %08X at %.1f m/s), "
                             "ground %.2f (%s), vehicle pointer %08X\n",
                             ped, int(int16_t(rd32(ram, ped + kPedModel) & 0xFFFFu)),
                             knock->bike ? "knocked off bike" : "knocked down by a hit,", knock->bike, knock->weapon, id, px,
                             py, pz, desc.velocity[0], desc.velocity[1], desc.velocity[2], later[0], later[1], later[2],
                             thrown.rammer, length(thrown.rammerSpeed), desc.groundZ,
                             desc.groundPlane ? "its own" : "the world's", rd32(ram, ped + kPedVehicle));
            }
            else
            {
                const float push = pushed && !blasted ? blow(ram, id, hit, bones, force) : 0.0f;
                if (hit.vehicle)
                    runDown(id, hit, force);
                // the blasts of the last frames that reach it (the one that killed it, most likely)
                unsigned blasts = 0;
                for (const Blast &b : g_blasts)
                    if (fresh && reaches(b))
                    {
                        physics::blast(id, b.centre, b.radius, b.speed);
                        ++blasts;
                    }
                if (goreOn(ram))
                {
                    const uint32_t ms = rd32(ram, kTimeMs);
                    if (fresh && hit.weapon >= 0)
                        severByHit(doll, ped, hit.weapon, hit.piece, ms);
                    for (const Blast &b : g_blasts)
                        if (fresh && reaches(b))
                            severByBlast(doll, ped, b.centre, b.radius, ms);
                }
                g_hits.erase(ped);
                std::fprintf(stderr,
                             "[jolt] ped %08X (model %d): ragdoll %d at (%.2f, %.2f, %.2f), %.2f m/s (SetDie %.2f, since "
                             "%.2f), ground %.2f (%s); killed by %08X (%s, weapon %d, piece %d), push %.0f, vehicle %.1f "
                             "m/s, blasts %u\n",
                             ped, int(int16_t(rd32(ram, ped + kPedModel) & 0xFFFFu)), id, px, py, pz, length(desc.velocity),
                             length(death.velocity), length(later), desc.groundZ, desc.groundPlane ? "its own" : "the world's",
                             hit.attacker, hit.vehicle ? "a vehicle" : hit.attacker ? "a ped" : "nobody", hit.weapon,
                             hit.piece, push, length(hit.speed), blasts);
            }
            // too many: the oldest dead one still simulated keeps its last pose (a knockdown only when all are)
            size_t live = 0;
            for (const auto &[p, d] : g_dolls)
                live += d.id != 0;
            if (live > kMaxDolls)
            {
                auto oldest = g_dolls.end();
                for (auto it = g_dolls.begin(); it != g_dolls.end(); ++it)
                    if (it->second.id && (oldest == g_dolls.end() ||
                                          std::make_pair(it->second.knockdown, it->second.frame) <
                                              std::make_pair(oldest->second.knockdown, oldest->second.frame)))
                        oldest = it;
                physics::destroy(oldest->second.id);
                oldest->second.id = 0;
            }
        }

        // A knocked-down ped whose body has settled: the game gets it up from where its body lies. Its position on the
        // body (1 m over its lowest bone), facing along it - feet first for a body on its back, head first for one on
        // its front, the way the game's get-ups stand up -, no speed left, its get-up time now (CPed::Fall then calls
        // SetGetUp); its body's last pose is eased into the game's get-up as that starts (afterBones).
        void getUp(uint8_t *ram, uint32_t ped, std::unordered_map<uint32_t, Doll>::iterator it)
        {
            const Doll &doll = it->second;
            const float *hips = doll.bones[2].m + 12, *head = doll.bones[5].m + 12;
            const bool onBack = doll.bones[3].m[6] > 0.0f; // the chest's forward (bone 3's Y row) points up
            float dx = head[0] - hips[0], dy = head[1] - hips[1];
            if (onBack)
            {
                dx = -dx;
                dy = -dy;
            }
            const float d = std::sqrt(dx * dx + dy * dy);
            const float heading = d > 0.05f ? std::atan2(-dx, dy) : rdf(ram, ped + kPedHeading);
            const float c = std::cos(heading), s = std::sin(heading);
            const float right[3] = {c, s, 0.0f}, forward[3] = {-s, c, 0.0f}, up[3] = {0.0f, 0.0f, 1.0f};
            for (uint32_t k = 0; k < 3; ++k)
            {
                wrf(ram, ped + 0x00u + 4 * k, right[k]);
                wrf(ram, ped + 0x10u + 4 * k, forward[k]);
                wrf(ram, ped + 0x20u + 4 * k, up[k]);
                wrf(ram, ped + kPedMoveSpeed + 4 * k, 0.0f);
            }
            wrf(ram, ped + kPedHeading, heading);
            wrf(ram, ped + kPedHeading + 4, heading);
            float lowest = doll.bones[0].m[14];
            for (const physics::BoneMatrix &b : doll.bones)
                lowest = std::min(lowest, b.m[14]);
            wrf(ram, ped + kPedPosition, doll.bones[0].m[12]);
            wrf(ram, ped + kPedPosition + 4, doll.bones[0].m[13]);
            wrf(ram, ped + kPedPosition + 8, lowest + kSoles);
            const uint32_t ms = rd32(ram, kTimeMs);
            wr32(ram, ped + kPedGetUpTime, ms);
            Blend &blend = g_blends[ped];
            blend.generation = doll.generation;
            std::copy(std::begin(doll.bones), std::end(doll.bones), blend.bones);
            blend.released = ms;
            blend.start = 0;
            std::fprintf(stderr, "[jolt] ped %08X: up from its knockdown after %.1f s, on its %s, heading %.2f\n", ped,
                         float(ms - doll.since) / 1000.0f, onBack ? "back" : "front", heading);
            if (doll.id)
                physics::destroy(doll.id);
            g_dolls.erase(it); // its tree stays noted for the blend
            ++g_gotUp;
        }


        // Small vector maths for the drawn pose's touches (the clutch's reach, the stumble's steps).
        void sub3(const float *a, const float *b, float *out)
        {
            for (int k = 0; k < 3; ++k)
                out[k] = a[k] - b[k];
        }
        float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
        void cross3(const float *a, const float *b, float *out)
        {
            const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
            out[0] = x;
            out[1] = y;
            out[2] = z;
        }
        bool normalise3(float *v)
        {
            const float l = length(v);
            if (l < 1e-5f)
                return false;
            for (int k = 0; k < 3; ++k)
                v[k] /= l;
            return true;
        }
        // v turned by `angle` about the unit `axis` (Rodrigues)
        void turn3(float *v, const float *axis, float angle)
        {
            float c[3];
            cross3(axis, v, c);
            const float cs = std::cos(angle), sn = std::sin(angle), d = dot3(axis, v) * (1.0f - cs);
            for (int k = 0; k < 3; ++k)
                v[k] = v[k] * cs + c[k] * sn + axis[k] * d;
        }
        // Bones first..last (a limb and what hangs on it) turned about `pivot`: their rows and positions.
        void turnBones(physics::BoneMatrix *b, int first, int last, const float *pivot, const float *axis, float angle)
        {
            for (int i = first; i <= last; ++i)
            {
                for (int r = 0; r < 3; ++r)
                    turn3(b[i].m + r * 4, axis, angle);
                float p[3];
                sub3(b[i].m + 12, pivot, p);
                turn3(p, axis, angle);
                for (int k = 0; k < 3; ++k)
                    b[i].m[12 + k] = pivot[k] + p[k];
            }
        }
        // The turn taking direction u to v: false when there is none to make.
        bool fromTo(const float *u, const float *v, float *axis, float &angle)
        {
            cross3(u, v, axis);
            const float s = length(axis), c = dot3(u, v);
            angle = std::atan2(s, c);
            return normalise3(axis) && angle > 1e-4f;
        }

        // An arm (upper arm `up`, then forearm, hand, fingers) reaching for `target`: two bones, the elbow kept on the
        // side it bends to.
        void reach(physics::BoneMatrix *b, int up, const float *target)
        {
            const float *s = b[up].m + 12;
            float se[3], eh[3], st[3];
            sub3(b[up + 1].m + 12, s, se);
            sub3(b[up + 2].m + 12, b[up + 1].m + 12, eh);
            sub3(target, s, st);
            const float la = length(se), lb = length(eh), d = std::clamp(length(st), std::fabs(la - lb) + 0.02f,
                                                                           la + lb - 0.02f);
            float dir[3] = {st[0], st[1], st[2]};
            if (la < 0.05f || lb < 0.05f || !normalise3(dir))
                return;
            float perp[3];
            for (int k = 0; k < 3; ++k)
                perp[k] = se[k] - dir[k] * dot3(se, dir);
            if (!normalise3(perp))
            {
                const float down[3] = {0.0f, 0.0f, -1.0f};
                cross3(dir, down, perp);
                if (!normalise3(perp))
                    return;
            }
            const float ca = std::clamp((la * la + d * d - lb * lb) / (2.0f * la * d), -1.0f, 1.0f);
            const float sa = std::sqrt(1.0f - ca * ca);
            float elbow[3], hand[3], axis[3], angle;
            for (int k = 0; k < 3; ++k)
            {
                elbow[k] = s[k] + dir[k] * la * ca + perp[k] * la * sa;
                hand[k] = s[k] + dir[k] * d;
            }
            float se2[3];
            sub3(elbow, s, se2);
            const float pivot[3] = {s[0], s[1], s[2]};
            if (fromTo(se, se2, axis, angle))
                turnBones(b, up, up + 3, pivot, axis, angle);
            float eh2[3], eh3[3];
            sub3(b[up + 2].m + 12, b[up + 1].m + 12, eh2);
            sub3(hand, b[up + 1].m + 12, eh3);
            const float knee[3] = {b[up + 1].m[12], b[up + 1].m[13], b[up + 1].m[14]};
            if (fromTo(eh2, eh3, axis, angle))
                turnBones(b, up + 1, up + 3, knee, axis, angle);
        }

        // A hit's extras (the ped not the player): the free hand to the wound, and for a hard hit - or the third in
        // a row - a stumble along the push.
        void hitExtras(const uint8_t *ram, uint32_t ped, Doll &d, const Hit &hit, const physics::BoneMatrix *bones,
                       float push)
        {
            const uint32_t ms = rd32(ram, kTimeMs);
            if (ped == rd32(ram, kPlayerPed) || generation(ram, hit.attacker) < 0)
                return;
            float away[3] = {rdf(ram, ped + kPedPosition) - rdf(ram, hit.attacker + kPedPosition),
                             rdf(ram, ped + kPedPosition + 4) - rdf(ram, hit.attacker + kPedPosition + 4), 0.0f};
            if (!normalise3(away))
                return;
            // the wound: on the part hit (VC's pieces; the head none), on the shooter's side
            static constexpr int kWoundBone[7] = {3, 2, 8, 13, 17, 21, -1};
            static constexpr float kAlong[7] = {0.12f, 0.05f, 0.12f, 0.12f, 0.18f, 0.18f, 0.0f};
            static constexpr float kOut[7] = {0.13f, 0.14f, 0.05f, 0.05f, 0.08f, 0.08f, 0.0f};
            const int piece = hit.piece >= 0 && hit.piece < 7 ? hit.piece : 0;
            const int bone = kWoundBone[piece];
            if (bone >= 0 && !d.running && d.clutchArm < 0)
            {
                const physics::BoneMatrix &w = bones[bone];
                float at[3];
                for (int k = 0; k < 3; ++k)
                    at[k] = w.m[12 + k] + w.m[k] * kAlong[piece] - away[k] * kOut[piece];
                // the other arm for an arm, the same side's for a leg, the nearer shoulder's for the trunk
                int arm;
                if (piece == 2 || piece == 3)
                    arm = piece == 2 ? 13 : 8;
                else if (piece == 4 || piece == 5)
                    arm = piece == 4 ? 8 : 13;
                else
                {
                    float l[3], r[3];
                    sub3(at, bones[8].m + 12, l);
                    sub3(at, bones[13].m + 12, r);
                    arm = length(l) < length(r) ? 8 : 13;
                }
                float off[3];
                sub3(at, w.m + 12, off);
                for (int r = 0; r < 3; ++r)
                    d.clutchLocal[r] = dot3(off, w.m + r * 4); // in the wounded bone's frame: it follows the body
                d.clutchArm = arm;
                d.clutchBone = bone;
                d.clutchSince = ms;
                d.clutchBest = 1e9f;
                ++g_clutches;
            }
            // a stumble
            d.hitTimes[d.hitCount++ % 3] = ms;
            const bool third = d.hitCount >= 3 && ms - d.hitTimes[(d.hitCount - 3) % 3] < kStumbleRowMs;
            if ((push >= kStumblePush || third) && !d.stumbleSince)
            {
                d.stumbleDist = std::clamp(push / 110.0f, 0.45f, 1.2f);
                d.stumbleSteps = d.stumbleDist < 0.6f ? 1 : d.stumbleDist < 0.95f ? 2 : 3;
                d.stumbleMs = 250 + 220 * uint32_t(d.stumbleSteps);
                d.stumbleDone = 0.0f;
                d.stumbleDir[0] = away[0];
                d.stumbleDir[1] = away[1];
                d.stumbleSince = ms;
                d.stumbleLeft = (d.hitCount & 1) != 0;
                ++g_stumbles;
            }
        }

        // The extras in the drawn pose: the hand on the wound (eased in, held, eased out), the stumble's steps (each
        // leg in turn swung along the push, its knee bent).
        void drawExtras(physics::BoneMatrix *out, Doll &d, uint32_t ms)
        {
            if (d.clutchArm >= 0)
            {
                const uint32_t t = ms - d.clutchSince;
                float wc = t < kClutchInMs ? float(t) / kClutchInMs
                                           : t < kClutchMs - kClutchOutMs ? 1.0f
                                                                          : std::max(0.0f, float(kClutchMs - t) / kClutchOutMs);
                // (kept when the ped runs off once it has it: GTA IV's peds run away holding the wound)
                if (t >= kClutchMs)
                    d.clutchArm = -1;
                else if (wc > 0.0f)
                {
                    const physics::BoneMatrix &w = out[d.clutchBone];
                    float at[3];
                    for (int k = 0; k < 3; ++k)
                        at[k] = w.m[12 + k] + w.m[k] * d.clutchLocal[0] + w.m[4 + k] * d.clutchLocal[1] + w.m[8 + k] * d.clutchLocal[2];
                    physics::BoneMatrix ik[kBones];
                    std::copy(out, out + kBones, ik);
                    reach(ik, d.clutchArm, at);
                    physics::blendPose(out, ik, wc, out, kBones);
                    if (wc > 0.99f && d.clutchBest > 1e8f)
                        std::fprintf(stderr, "[jolt] clutch: arm %d on bone %d held\n", d.clutchArm, d.clutchBone);
                    if (wc > 0.99f)
                    {
                        float gap[3];
                        sub3(out[d.clutchArm + 2].m + 12, at, gap);
                        d.clutchBest = std::min(d.clutchBest, length(gap));
                        g_clutchGap = d.clutchBest;
                    }
                }
            }
            if (d.stumbleSince && ms - d.stumbleSince < d.stumbleMs)
            {
                const float s = float(ms - d.stumbleSince) / float(d.stumbleMs) * float(d.stumbleSteps);
                const int step = std::min(int(s), d.stumbleSteps - 1);
                const float phase = s - float(step), pulse = std::sin(3.14159265f * std::clamp(phase, 0.0f, 1.0f));
                const bool left = d.stumbleLeft == ((step & 1) == 0);
                const int thigh = left ? 17 : 21;
                const float dir[3] = {d.stumbleDir[0], d.stumbleDir[1], 0.0f}, up[3] = {0.0f, 0.0f, 1.0f};
                float axis[3];
                cross3(dir, up, axis); // turning about it by + swings a foot toward dir
                if (normalise3(axis))
                {
                    const float hip[3] = {out[thigh].m[12], out[thigh].m[13], out[thigh].m[14]};
                    turnBones(out, thigh, thigh + 3, hip, axis, 0.45f * pulse);
                    const float knee[3] = {out[thigh + 1].m[12], out[thigh + 1].m[13], out[thigh + 1].m[14]};
                    turnBones(out, thigh + 1, thigh + 3, knee, axis, -0.7f * pulse);
                }
            }
        }

        // The stumble's ground covered this frame: the ped moved along the push (eased out), stopped short of what the
        // world puts in its way.
        void stumbleMove(uint8_t *ram, uint32_t ped, Doll &d, uint32_t ms)
        {
            if (!d.stumbleSince)
                return;
            const float x = std::min(1.0f, float(ms - d.stumbleSince) / float(d.stumbleMs));
            const float delta = d.stumbleDist * (1.0f - (1.0f - x) * (1.0f - x)) - d.stumbleDone;
            if (delta > 0.0f)
            {
                const float from[3] = {rdf(ram, ped + kPedPosition), rdf(ram, ped + kPedPosition + 4),
                                       rdf(ram, ped + kPedPosition + 8)};
                const float to[3] = {from[0] + d.stumbleDir[0] * (delta + 0.35f), from[1] + d.stumbleDir[1] * (delta + 0.35f),
                                     from[2]};
                if (physics::clear(from, to) < 1.0f)
                    d.stumbleDist = d.stumbleDone; // a wall, a car: no further
                else
                {
                    wrf(ram, ped + kPedPosition, from[0] + d.stumbleDir[0] * delta);
                    wrf(ram, ped + kPedPosition + 4, from[1] + d.stumbleDir[1] * delta);
                    d.stumbleDone += delta;
                    g_lastStumble = d.stumbleDone;
                }
            }
            if (x >= 1.0f)
                d.stumbleSince = 0;
        }

        // A ped shot and not killed, drawn: an active ragdoll from the pose its animation just gave it - the hips held to
        // the animation, the muscles pulling every limb back to it -, the bullet's push on the part it hit (from the
        // shooter's side, a share of the killing push). A ped already reacting takes the new hit's push.
        void react(uint8_t *ram, uint32_t ped, uint32_t tree, const Hit &hit)
        {
            const float force = static_cast<float>(g_force.load(std::memory_order_relaxed)) / 100.0f * kReactPush;
            if (const auto it = g_dolls.find(ped); it != g_dolls.end())
            {
                if (it->second.reaction && it->second.id && it->second.hitFrame != hit.frame)
                {
                    const float push = blow(ram, it->second.id, hit, it->second.bones, force);
                    hitExtras(ram, ped, it->second, hit, it->second.target, push);
                    it->second.hitFrame = hit.frame;
                    it->second.since = rd32(ram, kTimeMs);
                    ++g_reactions;
                }
                return;
            }
            const uint32_t matrices = rd32(ram, tree + kTreeMatrices), info = rd32(ram, tree + kTreeNodeInfo);
            if (rd32(ram, tree + kTreeNodes) != uint32_t(kBones) || !matrices || !info)
                return;
            physics::BoneMatrix bones[kBones];
            for (int i = 0; i < kBones; ++i)
                std::memcpy(bones[i].m, ram + ((matrices + i * 0x40u) & kRamMask), 64);
            const float px = rdf(ram, ped + kPedPosition), py = rdf(ram, ped + kPedPosition + 4);
            const float dx = bones[0].m[12] - px, dy = bones[0].m[13] - py;
            if (dx * dx + dy * dy > 4.0f)
                return;
            int parents[kBones];
            hierarchy(ram, info, parents);
            physics::RagdollDesc desc;
            desc.bones = bones;
            desc.parents = parents;
            desc.active = true;
            desc.groundPlane = false; // its hips carry it
            world(ram, 0.0f, bones[2].m + 12);
            const int id = physics::create(desc);
            if (!id)
                return;
            physics::drive(id, bones, 0.0f, kReactLimp);
            Doll &doll = g_dolls[ped];
            doll.id = id;
            doll.generation = generation(ram, ped);
            doll.frame = rd32(ram, kFrameCounter);
            doll.followX = px;
            doll.followY = py;
            doll.startZ = bones[2].m[14];
            doll.since = rd32(ram, kTimeMs);
            doll.reaction = true;
            doll.hitFrame = hit.frame;
            doll.hasTarget = true;
            std::copy(std::begin(bones), std::end(bones), doll.target);
            physics::pose(id, doll.bones);
            const float push = blow(ram, id, hit, bones, force);
            hitExtras(ram, ped, doll, hit, bones, push);
            ++g_reactions;
            g_newest = ped;
            g_reactDeviation = 0.0f;
            std::fprintf(stderr, "[jolt] ped %08X: hit (weapon %d, piece %d) by %08X - reaction %d, push %.0f\n", ped,
                         hit.weapon, hit.piece, hit.attacker, id, push);
        }

        // The ped's position follows its body's root bone (culling and the world's lists go where the body lies) - its
        // height too when the body went down stairs or off a ledge (1 m over its lowest bone, as a standing ped's is over
        // its soles); false when the game moved it itself (a teleport, the player's respawn): the ragdoll is left then.
        bool follow(uint8_t *ram, uint32_t ped, Doll &doll)
        {
            const float px = rdf(ram, ped + kPedPosition), py = rdf(ram, ped + kPedPosition + 4);
            if (std::fabs(px - doll.followX) > kGameMoved || std::fabs(py - doll.followY) > kGameMoved)
                return false;
            doll.followX = doll.bones[0].m[12];
            doll.followY = doll.bones[0].m[13];
            wrf(ram, ped + kPedPosition, doll.followX);
            wrf(ram, ped + kPedPosition + 4, doll.followY);
            float lowest = doll.bones[0].m[14];
            for (const physics::BoneMatrix &b : doll.bones)
                lowest = std::min(lowest, b.m[14]);
            if (std::fabs(lowest + kSoles - rdf(ram, ped + kPedPosition + 8)) > kFollowZ)
                wrf(ram, ped + kPedPosition + 8, lowest + kSoles);
            return true;
        }

        // Vehicles near a simulated ragdoll are boxes in the physics - their col model's bounding box, raised off the
        // ground - moved with them every step: a car throws a body it runs into and rolls over one lying under it. The
        // others are taken out (all of them when no ragdoll is simulated).
        void vehicles(const uint8_t *ram, float seconds)
        {
            const uint32_t now = rd32(ram, kFrameCounter);
            float hips[kMaxDolls + kMaxProps + 1][3]; // the ragdolls' hips and the moving props
            size_t dolls = 0;
            for (const auto &[ped, doll] : g_dolls)
                if (doll.id && dolls <= kMaxDolls)
                    std::copy(doll.bones[2].m + 12, doll.bones[2].m + 15, hips[dolls++]);
            for (const auto &[object, prop] : g_props)
                if (prop.id && dolls < kMaxDolls + kMaxProps + 1)
                    std::copy(prop.rest.m + 12, prop.rest.m + 15, hips[dolls++]);
            const uint32_t pool = dolls ? rd32(ram, kVehiclePool) : 0;
            const uint32_t base = pool ? rd32(ram, pool) : 0, flags = pool ? rd32(ram, pool + 4) : 0,
                           size = pool ? rd32(ram, pool + 8) : 0;
            const uint32_t infos = rd32(ram, kModelInfos);
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint8_t flag = ram[(flags + i) & kRamMask];
                const uint32_t v = base + i * kVehicleSize;
                if (flag & 0x80)
                    continue;
                // a rider knocked off it starts astride it: no box for it while that one is down
                if (std::any_of(g_dolls.begin(), g_dolls.end(),
                                [v](const auto &d) { return d.second.id && d.second.knockdown && d.second.bike == v; }))
                    continue;
                const float x = rdf(ram, v + kPedPosition), y = rdf(ram, v + kPedPosition + 4), z = rdf(ram, v + kPedPosition + 8);
                bool near = false;
                for (size_t d = 0; d < dolls && !near; ++d)
                    near = (x - hips[d][0]) * (x - hips[d][0]) + (y - hips[d][1]) * (y - hips[d][1]) +
                               (z - hips[d][2]) * (z - hips[d][2]) < kVehicleReach * kVehicleReach;
                if (!near)
                    continue;
                const int model = int(int16_t(rd32(ram, v + kPedModel) & 0xFFFFu)), gen = flag & 0x7F;
                physics::BoneMatrix m;
                std::memcpy(m.m, ram + (v & kRamMask), 64); // right, forward, up, position
                auto it = g_obstacles.find(v);
                if (it != g_obstacles.end() && (it->second.generation != gen || it->second.model != model))
                {
                    physics::removeObstacle(it->second.id);
                    g_obstacles.erase(it);
                    it = g_obstacles.end();
                }
                if (it == g_obstacles.end())
                {
                    const uint32_t col = model >= 0 && infos ? rd32(ram, rd32(ram, infos + uint32_t(model) * 4) + kModelColModel) : 0;
                    if (!col)
                        continue;
                    float lo[3], hi[3];
                    for (uint32_t k = 0; k < 3; ++k)
                    {
                        lo[k] = rdf(ram, col + kColBoxMin + 4 * k);
                        hi[k] = rdf(ram, col + kColBoxMax + 4 * k);
                    }
                    lo[2] = std::min(lo[2] + kVehicleClearance, hi[2] - 0.3f);
                    const int id = physics::addObstacle(m, lo, hi, rd32(ram, v + kVehicleType) == 0); // 0: a car
                    if (!id)
                        continue;
                    it = g_obstacles.emplace(v, Obstacle{id, gen, model, now}).first;
                }
                else
                    physics::moveObstacle(it->second.id, m, seconds);
                it->second.seen = now;
            }
            for (auto it = g_obstacles.begin(); it != g_obstacles.end();)
                if (it->second.seen != now)
                {
                    physics::removeObstacle(it->second.id);
                    it = g_obstacles.erase(it);
                }
                else
                    ++it;
        }

        // What a col model holds now - its counts, its arrays and its first vertices -, 0 when it has no primitives
        // (models without collision of their own, collision streamed out).
        uint64_t colKey(const uint8_t *ram, uint32_t col)
        {
            const uint32_t counts = rd32(ram, col + kColBoxCount), spheres = ram[(col + kColSphereCount) & kRamMask];
            if (!counts && !spheres)
                return 0;
            uint64_t h = 1469598103934665603ull;
            auto mix = [&h](uint32_t v) { h = (h ^ v) * 1099511628211ull; };
            mix(counts);
            mix(spheres);
            for (uint32_t array : {kColSphereArray, kColBoxArray, kColVertexArray, kColTriangleArray})
                mix(rd32(ram, col + array));
            const uint32_t vertices = rd32(ram, col + kColVertexArray);
            for (uint32_t i = 0; vertices && i < 16; i += 4)
                mix(rd32(ram, vertices + i));
            return h | 1;
        }

        // A col model's primitives as a shape in the physics: its triangles, boxes and spheres.
        int makeShape(const uint8_t *ram, uint32_t col)
        {
            const uint32_t boxes = rd16(ram, col + kColBoxCount), triangles = rd16(ram, col + kColTriangleCount),
                           spheres = ram[(col + kColSphereCount) & kRamMask];
            std::vector<float> vertices, boxData, sphereData;
            std::vector<int> indices;
            const uint32_t vertexArray = rd32(ram, col + kColVertexArray), triangleArray = rd32(ram, col + kColTriangleArray);
            if (triangles && vertexArray && triangleArray)
            {
                uint32_t last = 0;
                indices.reserve(triangles * 3u);
                for (uint32_t i = 0; i < triangles; ++i)
                {
                    const uint32_t t = triangleArray + i * 8u;
                    const uint32_t a = rd16(ram, t), b = rd16(ram, t + 2), c = rd16(ram, t + 4);
                    if (a % 6 || b % 6 || c % 6)
                        continue;
                    // the game's front face is clockwise (its plane's normal is (c - a) x (b - a)); the physics' is not
                    indices.insert(indices.end(), {int(a / 6), int(c / 6), int(b / 6)});
                    last = std::max({last, a, b, c});
                }
                vertices.resize((last / 6 + 1) * 3);
                for (uint32_t i = 0; i < vertices.size(); ++i)
                    vertices[i] = float(int16_t(rd16(ram, vertexArray + i * 2))) / 128.0f;
            }
            const uint32_t boxArray = rd32(ram, col + kColBoxArray), sphereArray = rd32(ram, col + kColSphereArray);
            for (uint32_t i = 0; boxArray && i < boxes; ++i)
                for (uint32_t k : {0x00u, 0x04u, 0x08u, 0x10u, 0x14u, 0x18u}) // min, max
                    boxData.push_back(rdf(ram, boxArray + i * 0x30u + k));
            for (uint32_t i = 0; sphereArray && i < spheres; ++i)
                for (uint32_t k = 0; k < 16; k += 4) // centre, radius
                    sphereData.push_back(rdf(ram, sphereArray + i * 0x20u + k));
            physics::ColShape shape;
            shape.vertices = vertices.data();
            shape.vertexCount = int(vertices.size() / 3);
            shape.triangles = indices.data();
            shape.triangleCount = int(indices.size() / 3);
            shape.boxes = boxData.data();
            shape.boxCount = int(boxData.size() / 6);
            shape.spheres = sphereData.data();
            shape.sphereCount = int(sphereData.size() / 4);
            ++g_shapesMade;
            return physics::addShape(shape);
        }

        int sectorIndex(float coordinate, float origin)
        {
            return std::clamp(int(std::floor(coordinate / kSectorSize + origin)), 0, kSectorCount - 1);
        }

        // The world's collision near the ragdolls (and `extra`: where one is about to be made) in the physics: the col
        // models of the buildings and objects in the sectors around them whose bounds come within kWorldReach - buildings
        // as static bodies, objects as kinematic ones moved with the game's. The others are taken out; shapes not wanted
        // for kShapeMs go.
        void world(const uint8_t *ram, float seconds, const float *extra)
        {
            const uint32_t now = rd32(ram, kFrameCounter), ms = rd32(ram, kTimeMs);
            float points[kMaxDolls + kMaxProps + 2][3];
            size_t count = 0;
            if (g_worldOn)
            {
                for (const auto &[ped, doll] : g_dolls)
                    if (doll.id && count <= kMaxDolls)
                        std::copy(doll.bones[2].m + 12, doll.bones[2].m + 15, points[count++]);
                for (const auto &[object, prop] : g_props)
                    if (prop.id && count < kMaxDolls + kMaxProps + 1)
                        std::copy(prop.rest.m + 12, prop.rest.m + 15, points[count++]);
                if (extra)
                    std::copy(extra, extra + 3, points[count++]);
            }
            const uint32_t sectors = count ? rd32(ram, kSectors) : 0, infos = rd32(ram, kModelInfos);
            auto near = [&](const float *c, float radius) {
                for (size_t p = 0; p < count; ++p)
                {
                    const float d[3] = {c[0] - points[p][0], c[1] - points[p][1], c[2] - points[p][2]};
                    if (length(d) < radius + kWorldReach)
                        return true;
                }
                return false;
            };
            auto consider = [&](uint32_t e, bool moving) {
                if (!e || g_props.count(e) || std::find(g_checked.begin(), g_checked.end(), e) != g_checked.end())
                    return; // (a prop is in the physics as itself)
                g_checked.push_back(e);
                const int model = int(int16_t(rd16(ram, e + kPedModel)));
                if (!(rd32(ram, e + kEntityFlags) & kUsesCollision) || model < 0 || !infos)
                    return;
                const uint32_t info = rd32(ram, infos + uint32_t(model) * 4);
                const uint32_t col = info ? rd32(ram, info + kModelColModel) : 0;
                if (!col)
                    return;
                physics::BoneMatrix m;
                std::memcpy(m.m, ram + (e & kRamMask), 64);
                // its bounding sphere in the world
                float centre[3];
                for (int k = 0; k < 3; ++k)
                    centre[k] = m.m[12 + k] + m.m[k] * rdf(ram, col) + m.m[4 + k] * rdf(ram, col + 4) + m.m[8 + k] * rdf(ram, col + 8);
                if (!near(centre, rdf(ram, col + 12)))
                    return;
                const uint64_t key = colKey(ram, col);
                if (!key)
                    return;
                ColShape &shape = g_colShapes[col];
                if (shape.key != key)
                {
                    if (shape.id)
                        physics::removeShape(shape.id);
                    shape.id = makeShape(ram, col);
                    shape.key = key;
                }
                shape.used = ms;
                if (!shape.id)
                    return;
                auto it = g_world.find(e);
                if (it != g_world.end() &&
                    (it->second.col != col || it->second.key != key || it->second.moving != moving ||
                     (!moving && std::fabs(m.m[12] - it->second.at[0]) + std::fabs(m.m[13] - it->second.at[1]) +
                                         std::fabs(m.m[14] - it->second.at[2]) > 0.01f)))
                {
                    physics::removeObstacle(it->second.id);
                    g_world.erase(it);
                    it = g_world.end();
                }
                if (it == g_world.end())
                {
                    const int id = physics::addWorld(shape.id, m, moving);
                    if (!id)
                        return;
                    it = g_world.emplace(e, WorldBody{id, col, key, moving, {m.m[12], m.m[13], m.m[14]}, now}).first;
                }
                else if (moving)
                    physics::moveObstacle(it->second.id, m, seconds);
                it->second.seen = now;
            };
            g_checked.clear();
            uint32_t walked[128];
            size_t walkedCount = 0;
            for (size_t p = 0; p < count && sectors; ++p)
            {
                const int x0 = sectorIndex(points[p][0] - kWorldReach, kSectorX0), x1 = sectorIndex(points[p][0] + kWorldReach, kSectorX0);
                const int y0 = sectorIndex(points[p][1] - kWorldReach, kSectorY0), y1 = sectorIndex(points[p][1] + kWorldReach, kSectorY0);
                for (int sy = y0; sy <= y1; ++sy)
                    for (int sx = x0; sx <= x1; ++sx)
                    {
                        const uint32_t index = uint32_t(sy * kSectorCount + sx);
                        if (walkedCount == std::size(walked) ||
                            std::find(walked, walked + walkedCount, index) != walked + walkedCount)
                            continue;
                        walked[walkedCount++] = index;
                        const uint32_t sector = sectors + index * kSectorBytes;
                        for (const uint32_t *lists : {kBuildingLists, kObjectLists})
                        {
                            const bool moving = lists == kObjectLists;
                            const size_t n = moving ? std::size(kObjectLists) : std::size(kBuildingLists);
                            for (size_t l = 0; l < n; ++l)
                            {
                                uint32_t node = rd32(ram, sector + lists[l]);
                                for (uint32_t i = 0; node && i < kListLimit; ++i, node = rd32(ram, node + 8))
                                    consider(rd32(ram, node), moving);
                            }
                        }
                    }
            }
            for (auto it = g_world.begin(); it != g_world.end();)
                if (it->second.seen != now)
                {
                    physics::removeObstacle(it->second.id);
                    it = g_world.erase(it);
                }
                else
                    ++it;
            for (auto it = g_colShapes.begin(); it != g_colShapes.end();)
                if (ms - it->second.used > kShapeMs)
                {
                    if (it->second.id)
                        physics::removeShape(it->second.id);
                    it = g_colShapes.erase(it);
                }
                else
                    ++it;
        }

        // The pickup whose object `object` is, or -1.
        int pickupSlot(const uint8_t *ram, uint32_t object)
        {
            for (uint32_t i = 0; i < kPickupCount; ++i)
            {
                const uint32_t slot = kPickups + i * kPickupSize;
                if (rd32(ram, slot + kPickupObject) == object && ram[(slot + kPickupType) & kRamMask])
                    return int(i);
            }
            return -1;
        }

        // A pickup that lies on the ground as in GTA IV: cash, or a weapon (its model one of the weapons').
        bool loosePickup(const uint8_t *ram, uint32_t i)
        {
            const uint32_t slot = kPickups + i * kPickupSize;
            const int model = int(int16_t(rd16(ram, slot + kPickupModel)));
            return ram[(slot + kPickupType) & kRamMask] == kPickupMoney || (model >= kFirstWeaponModel && model <= kLastWeaponModel);
        }

        size_t movingProps()
        {
            size_t n = 0;
            for (const auto &[object, prop] : g_props)
                n += prop.id != 0;
            return n;
        }

        // A model's box (centre, half extents) in its frame: from its loaded geometry - the 'ldm' chunk at model info
        // +0xC, its RslElement at +0x20, the element's geometry at +0x14, whose PS2 part (+0x20) has int16 bounds at
        // +0x1C / +0x22 scaled by +0x28 around +0x34 (game-internals.md 12g, 12m) -, else its col model's bounds when
        // they are not the generic half-metre cube, else a pistol's size.
        void propBox(const uint8_t *ram, int model, float *centre, float *half)
        {
            static constexpr float kDefault[3] = {0.12f, 0.025f, 0.08f};
            std::copy(kDefault, kDefault + 3, half);
            std::fill(centre, centre + 3, 0.0f);
            const uint32_t infos = rd32(ram, kModelInfos);
            const uint32_t info = infos && model >= 0 ? rd32(ram, infos + uint32_t(model) * 4) : 0;
            const uint32_t chunk = info ? rd32(ram, info + 0xCu) : 0;
            const uint32_t element = chunk ? rd32(ram, chunk + 0x20u) : 0;
            if (element && rd32(ram, element) == 0x0004AA01u && rd32(ram, element + 0x14u))
            {
                const uint32_t ps2 = rd32(ram, element + 0x14u) + 0x20u;
                float lo[3], hi[3];
                bool good = true;
                for (uint32_t k = 0; k < 3; ++k)
                {
                    const float scale = rdf(ram, ps2 + 0x28u + 4 * k), mid = rdf(ram, ps2 + 0x34u + 4 * k);
                    lo[k] = float(int16_t(rd16(ram, ps2 + 0x1Cu + 2 * k))) / 32768.0f * scale + mid;
                    hi[k] = float(int16_t(rd16(ram, ps2 + 0x22u + 2 * k))) / 32768.0f * scale + mid;
                    good = good && std::isfinite(lo[k]) && std::isfinite(hi[k]) && hi[k] > lo[k] && hi[k] - lo[k] < 3.0f;
                }
                if (good)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        centre[k] = 0.5f * (lo[k] + hi[k]);
                        half[k] = std::max(0.5f * (hi[k] - lo[k]), 0.012f);
                    }
                    return;
                }
            }
            const uint32_t col = info ? rd32(ram, info + kModelColModel) : 0;
            if (!col)
                return;
            float lo[3], hi[3];
            for (uint32_t k = 0; k < 3; ++k)
            {
                lo[k] = rdf(ram, col + kColBoxMin + 4 * k);
                hi[k] = rdf(ram, col + kColBoxMax + 4 * k);
            }
            if (std::isfinite(lo[0] + hi[0] + lo[1] + hi[1] + lo[2] + hi[2]) && hi[0] - lo[0] < 0.45f && hi[0] > lo[0] &&
                hi[1] > lo[1] && hi[2] > lo[2])
                for (int k = 0; k < 3; ++k)
                {
                    centre[k] = 0.5f * (lo[k] + hi[k]);
                    half[k] = std::max(0.5f * (hi[k] - lo[k]), 0.012f);
                }
        }

        // A pickup's object dropped by the physics from where the game put it: tumbling about its length so it lands
        // on a side; a dead ped's gun or cash thrown a little out and up as well.
        bool makeProp(const uint8_t *ram, uint32_t i, uint32_t object, Prop &prop)
        {
            const uint32_t slot = kPickups + i * kPickupSize;
            const int model = int(int16_t(rd16(ram, slot + kPickupModel)));
            const uint32_t type = ram[(slot + kPickupType) & kRamMask];
            float centre[3], half[3];
            propBox(ram, model, centre, half);
            physics::BoneMatrix m;
            std::memcpy(m.m, ram + (object & kRamMask), 64);
            for (int r = 0; r < 3; ++r) // the game's spin scale out
            {
                const float l = length(m.m + r * 4);
                for (int k = 0; k < 3; ++k)
                    m.m[r * 4 + k] = l > 1e-4f ? m.m[r * 4 + k] / l : (r == k ? 1.0f : 0.0f);
                m.m[r * 4 + 3] = 0.0f;
            }
            m.m[15] = 0.0f;
            uint32_t seed = object * 2654435761u ^ rd32(ram, kTimeMs);
            auto random = [&seed](float lo, float hi) {
                seed = seed * 1664525u + 1013904223u;
                return lo + (hi - lo) * float(seed >> 8) / float(1u << 24);
            };
            const bool thrown = type == kPickupDropped || type == kPickupMoney;
            const float angle = random(0.0f, 6.2831853f), out = thrown ? random(0.5f, 1.5f) : 0.0f;
            const float velocity[3] = {std::cos(angle) * out, std::sin(angle) * out, thrown ? random(1.0f, 2.0f) : 0.0f};
            const float roll = random(0.0f, 1.0f) < 0.5f ? -random(3.0f, 6.0f) : random(3.0f, 6.0f);
            const float spin[3] = {m.m[0] * roll + random(-1.0f, 1.0f), m.m[1] * roll + random(-1.0f, 1.0f),
                                   m.m[2] * roll + random(-2.0f, 2.0f)};
            const float mass = std::clamp(8.0f * half[0] * half[1] * half[2] * 700.0f, 0.15f, 6.0f);
            prop.id = physics::addProp(m, centre, half, mass, velocity, spin);
            if (!prop.id)
                return false;
            prop.slot = i;
            prop.rest = m;
            ++g_propsMade;
            std::fprintf(stderr, "[jolt] pickup %u (model %d, type %u): prop %d, box (%.2f, %.2f, %.2f) %.2f kg, at (%.2f, %.2f, "
                         "%.2f)\n", i, model, type, prop.id, 2 * half[0], 2 * half[1], 2 * half[2], mass, m.m[12], m.m[13],
                         m.m[14]);
            return true;
        }

        // Before the step: props whose pickup is gone (collected, timed out) go; one not drawn for a while stops where
        // it is.
        void props(uint32_t ms, const uint8_t *ram)
        {
            const bool on = g_pickupsOn.load(std::memory_order_relaxed);
            for (auto it = g_props.begin(); it != g_props.end();)
            {
                Prop &p = it->second;
                const uint32_t slot = kPickups + p.slot * kPickupSize;
                if (!on || rd32(ram, slot + kPickupObject) != it->first || !ram[(slot + kPickupType) & kRamMask])
                {
                    std::fprintf(stderr, "[jolt] pickup %u: prop gone (%s)\n", p.slot, !on ? "switched off" : "picked up or gone");
                    if (p.id)
                        physics::removeProp(p.id);
                    it = g_props.erase(it);
                    continue;
                }
                if (p.id && ms - p.seen > kPropUnseenMs)
                {
                    physics::removeProp(p.id);
                    p.id = 0;
                }
                ++it;
            }
        }

        // After the step: the moving props' places, their pickups' positions over them (collection is measured from
        // there); settled ones leave the physics.
        void propsMoved(uint8_t *ram, uint32_t ms)
        {
            for (auto &[object, p] : g_props)
            {
                if (!p.id || !physics::propPose(p.id, p.rest))
                    continue;
                const uint32_t slot = kPickups + p.slot * kPickupSize;
                wrf(ram, slot + 0, p.rest.m[12]);
                wrf(ram, slot + 4, p.rest.m[13]);
                wrf(ram, slot + 8, p.rest.m[14] + kPickupHeight);
                p.still = physics::propAwake(p.id) ? 0 : p.still ? p.still : ms;
                if (p.still && ms - p.still >= kPropSettleMs)
                {
                    std::fprintf(stderr, "[jolt] pickup %u: prop at rest at (%.2f, %.2f, %.2f)\n", p.slot, p.rest.m[12],
                                 p.rest.m[13], p.rest.m[14]);
                    physics::removeProp(p.id);
                    p.id = 0;
                }
            }
        }

        bool isPlayer(const uint8_t *ram, uint32_t ped) { return ped && rd32(ram, kPlayerPed) == ped; }

        // A knocked-down ped while it falls: its arms reach toward the ground it falls to - along its hips' way down,
        // forward or back as it goes - until it lands (its hips within kCatchLand of the ground) or kCatchMs; then they
        // go limp and, armed and shot down by a ped, it may fire back at it from the ground.
        void catchFall(uint8_t *ram, uint32_t ped, Doll &d, uint32_t ms)
        {
            const float *hips = d.bones[2].m + 12;
            if (!d.landed)
            {
                const float way[3] = {hips[0] - d.fallFrom[0], hips[1] - d.fallFrom[1], 0.0f};
                const bool first = d.fallFrom[0] == 0.0f && d.fallFrom[1] == 0.0f;
                std::copy(hips, hips + 3, d.fallFrom);
                float under = 0.0f;
                const bool near = physics::ground(hips, 3.0f, under) && hips[2] - under < kCatchLand;
                if (near || ms - d.since > kCatchMs || !g_catchFall.load(std::memory_order_relaxed))
                {
                    const float none[3] = {0.0f, 0.0f, -1.0f};
                    physics::brace(d.id, none, 0.0f);
                    d.landed = ms;
                }
                else if (!first)
                {
                    const float l = length(way);
                    const float dir[3] = {l > 1e-3f ? way[0] / l * 0.7f : 0.0f, l > 1e-3f ? way[1] / l * 0.7f : 0.0f, -1.0f};
                    if (!d.bracing)
                        ++g_braced;
                    d.bracing = true;
                    physics::brace(d.id, dir, 1.0f);
                }
                return;
            }
            // firing back: a gun in hand with rounds in it, its shooter alive, not the player
            if (!d.shooter || d.shots >= kFireShots || !g_blindFire.load(std::memory_order_relaxed) || isPlayer(ram, ped))
                return;
            const uint32_t weapon = ped + kPedWeapons + ram[(ped + kPedWeaponSlot) & kRamMask] * kWeaponSize;
            const int type = int(rd32(ram, weapon + 4));
            if (type < 19 || type > 37 || !rd32(ram, weapon + 0xC) || generation(ram, d.shooter) < 0 ||
                stillDead(ram, d.shooter, generation(ram, d.shooter)))
                return;
            if (!d.nextShot)
                d.nextShot = d.landed + kFireAfterMs;
            if (ms >= d.nextShot)
            {
                d.fireNow = true; // at its next ApplyMoveSpeed
                d.nextShot = ms + kFireGapMs;
                ++d.shots;
            }
        }

        // Once a game frame, after the world's update: stale ragdolls go, the physics steps, the poses are taken.
        void frame(uint8_t *ram)
        {
            const bool on = g_enabled.load(std::memory_order_relaxed);
            for (auto it = g_dolls.begin(); it != g_dolls.end();)
            {
                auto next = std::next(it);
                if (!on)
                    remove(ram, it, "switched off");
                else if (!stillDown(ram, it->first, it->second))
                    remove(ram, it, "the ped is gone or up");
                else if (it->second.reaction && rd32(ram, kTimeMs) - it->second.since > kReactMs && it->second.clutchArm < 0 &&
                         !it->second.stumbleSince)
                {
                    if (it->second.id)
                        physics::destroy(it->second.id);
                    g_trees.erase(it->first);
                    g_dolls.erase(it);
                }
                else if (it->second.knockdown && stillDead(ram, it->first, it->second.generation))
                {
                    it->second.knockdown = false; // it died down there: a dead body from now on
                    std::fprintf(stderr, "[jolt] ped %08X: died while down\n", it->first);
                }
                it = next;
            }
            const uint32_t now = rd32(ram, kFrameCounter), ms = rd32(ram, kTimeMs);
            const bool bikes = on && g_bikeFalls.load(std::memory_order_relaxed);
            for (auto it = g_knocked.begin(); it != g_knocked.end();)
                if (!bikes || generation(ram, it->first) != it->second.generation || ms - it->second.time > kKnockMs ||
                    g_dolls.count(it->first))
                {
                    if (!g_dolls.count(it->first) && !g_pending.count(it->first) && !g_blends.count(it->first))
                        g_trees.erase(it->first);
                    it = g_knocked.erase(it);
                }
                else
                    ++it;
            for (auto it = g_blends.begin(); it != g_blends.end();)
                if (!on || generation(ram, it->first) != it->second.generation ||
                    (it->second.start ? ms - it->second.start > kBlendMs : ms - it->second.released > kStandMs))
                {
                    if (!g_dolls.count(it->first) && !g_pending.count(it->first) && !g_knocked.count(it->first))
                        g_trees.erase(it->first);
                    it = g_blends.erase(it);
                }
                else
                    ++it;
            for (auto it = g_pending.begin(); it != g_pending.end();)
                if (!on || !stillDead(ram, it->first, it->second.generation) || now - it->second.frame > kPendingFrames)
                {
                    if (!g_dolls.count(it->first))
                        g_trees.erase(it->first);
                    it = g_pending.erase(it);
                }
                else
                    ++it;
            for (auto it = g_hits.begin(); it != g_hits.end();)
                it = now - it->second.frame > kHitFrames + kSeedFrames ? g_hits.erase(it) : std::next(it);
            g_blasts.erase(std::remove_if(g_blasts.begin(), g_blasts.end(),
                                          [now](const Blast &b) { return now - b.frame > kHitFrames + kSeedFrames; }),
                           g_blasts.end());
            props(ms, ram);
            const float step = rdf(ram, kTimeStep);
            for (auto &[ped, doll] : g_dolls)
                if (doll.reaction && doll.id && doll.hasTarget)
                {
                    const float t = std::clamp(float(ms - doll.since - kReactHoldMs) / float(kReactMs - kReactHoldMs), 0.0f, 1.0f);
                    physics::drive(doll.id, doll.target, step > 0.0f ? step / 50.0f : 0.0f,
                                   ms - doll.since < kReactHoldMs ? kReactLimp : kReactLimp + (kReactStrength - kReactLimp) * t);
                }
            vehicles(ram, step > 0.0f ? step / 50.0f : 0.0f);
            world(ram, step > 0.0f ? step / 50.0f : 0.0f);
            if ((!g_dolls.empty() || movingProps()) && step > 0.0f)
            {
                physics::step(step / 50.0f);
                ++g_steps;
            }
            propsMoved(ram, ms);
            for (auto it = g_dolls.begin(); it != g_dolls.end();)
            {
                auto next = std::next(it);
                Doll &doll = it->second;
                if (doll.id && doll.reaction)
                {
                    physics::pose(doll.id, doll.bones); // the game moves the ped ...
                    stumbleMove(ram, it->first, doll, ms); // ... and a stumble with it
                }
                else if (doll.id)
                {
                    physics::pose(doll.id, doll.bones);
                    if (!follow(ram, it->first, doll))
                        remove(ram, it, "moved by the game");
                    else if (doll.bones[2].m[14] < doll.startZ - kLost)
                    {
                        // nothing under it any more (collision streamed out): it stays as it is
                        std::fprintf(stderr, "[jolt] ped %08X: ragdoll fell out of the world, frozen\n", it->first);
                        if (doll.knockdown)
                            remove(ram, it, "fell out of the world");
                        else
                        {
                            physics::destroy(doll.id);
                            doll.id = 0;
                        }
                    }
                    else if (doll.knockdown)
                    {
                        // down while its body moves (the game's get-up held back, no speed of its own building up);
                        // up once it has lain still a moment, or at the latest after kDownMs
                        doll.still = physics::awake(doll.id) ? 0 : doll.still ? doll.still : ms;
                        const float *hips = doll.bones[2].m + 12;
                        const float moved[3] = {hips[0] - doll.restAt[0], hips[1] - doll.restAt[1], hips[2] - doll.restAt[2]};
                        if (!doll.restSince || length(moved) > 0.03f)
                        {
                            std::copy(hips, hips + 3, doll.restAt);
                            doll.restSince = ms;
                        }
                        else if (!doll.still && ms - doll.restSince >= kSettleMs)
                            doll.still = doll.restSince; // as good as asleep
                        catchFall(ram, it->first, doll, ms);
                        if ((doll.still && ms - doll.still >= kSettleMs && ms - doll.since >= kMinDownMs) ||
                            ms - doll.since >= kDownMs)
                            getUp(ram, it->first, it);
                        else
                        {
                            wr32(ram, it->first + kPedGetUpTime, 0xFFFFFFFFu);
                            for (uint32_t k = 0; k < 3; ++k)
                                wrf(ram, it->first + kPedMoveSpeed + 4 * k, 0.0f);
                        }
                    }
                }
                it = next;
            }
            // stumps bleeding: the move speed the last drop borrowed back to 0, the next drop (at the next ApplyMoveSpeed)
            if (goreOn(ram))
                for (auto &[ped, d] : g_dolls)
                    if (d.stumps && !d.reaction && !d.bloodNow)
                    {
                        for (uint32_t k = 0; k < 3; ++k)
                            wrf(ram, ped + kPedMoveSpeed + 4 * k, 0.0f);
                        bleed(d, ms);
                    }
            publish();
        }

        // CPed::SetDie(ped a0): the ped and its velocity, for its next draw.
        VCS_GUEST_CC void setDie(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive) // an older vcs.exe with these built in (jolt.h passive)
                return g_origSetDie(rdram, ctx, runtime);
            const uint32_t ped = mod::gpr32(ctx, kA0);
            if (const auto it = g_dolls.find(ped); it != g_dolls.end() && it->second.reaction)
            {
                if (it->second.id)
                    physics::destroy(it->second.id);
                g_dolls.erase(it); // its tree stays noted: the death's ragdoll is made from its next draw
            }
            if (g_enabled.load(std::memory_order_relaxed) && ped && !g_dolls.count(ped))
            {
                const uint32_t state = rd32(rdram, ped + kPedState);
                const int gen = generation(rdram, ped);
                // not in a vehicle - unless being thrown off a bike (KnockOffRider calls SetDie for a rider it kills)
                if (gen >= 0 && state != kStateDying && state != kStateDead &&
                    (!rd32(rdram, ped + kPedVehicle) || g_knocked.count(ped)))
                {
                    Death &death = g_pending[ped];
                    death.generation = gen;
                    for (uint32_t k = 0; k < 3; ++k)
                        death.velocity[k] = rdf(rdram, ped + kPedMoveSpeed + 4 * k) * 50.0f;
                    death.frame = rd32(rdram, kFrameCounter);
                }
            }
            return g_origSetDie(rdram, ctx, runtime);
        }

        // CPed::InflictDamage(ped a0, attacker a1, weapon a2, damage f12, piece a3, direction t0): who hit the ped, with
        // what, where - its ragdoll's push when this blow kills it (the original calls SetDie then).
        VCS_GUEST_CC void inflictDamage(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive) // an older vcs.exe with these built in (jolt.h passive)
                return g_origInflict(rdram, ctx, runtime);
            const uint32_t now = rd32(rdram, kFrameCounter);
            const auto same = g_hits.find(mod::gpr32(ctx, kA0));
            // a shotgun's pellets, a car's contacts: the burst's first blow is the one taken (the game knocks the ped
            // back between them)
            if (g_enabled.load(std::memory_order_relaxed) && mod::gpr32(ctx, kA0) &&
                (same == g_hits.end() || same->second.frame != now || same->second.attacker != mod::gpr32(ctx, kA1)))
            {
                Hit &hit = g_hits[mod::gpr32(ctx, kA0)];
                hit.attacker = mod::gpr32(ctx, kA1);
                hit.weapon = int(mod::gpr32(ctx, kA2));
                hit.piece = int(mod::gpr32(ctx, kA3));
                hit.frame = now;
                hit.vehicle = isVehicle(rdram, hit.attacker);
                for (uint32_t k = 0; k < 3; ++k)
                {
                    hit.speed[k] = hit.vehicle ? rdf(rdram, hit.attacker + kVehicleMoveSpeed + 4 * k) * 50.0f : 0.0f;
                    hit.pedSpeed[k] = rdf(rdram, mod::gpr32(ctx, kA0) + kPedMoveSpeed + 4 * k) * 50.0f;
                }
            }
            return g_origInflict(rdram, ctx, runtime);
        }

        // CWeapon::ProcessLineOfSight(start* a0, end* a1, ..., weapon t0, shooter t1): a bullet whose line meets a ragdolled
        // body moves it, at the point it meets it - cast against the ragdolls themselves: the game's own collision for a
        // dead ped is not where its ragdoll lies (pistol shots at a body's hips hit the road under them).
        VCS_GUEST_CC void lineOfSight(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive) // an older vcs.exe with these built in (jolt.h passive)
                return g_origLineOfSight(rdram, ctx, runtime);
            const int weapon = int(mod::gpr32(ctx, kT0));
            if (!g_dolls.empty() && mod::gpr32(ctx, kA0) && mod::gpr32(ctx, kA1) && weapon >= 0 &&
                weapon < int(std::size(kCorpsePush)) && kCorpsePush[weapon] > 0.0f)
            {
                float from[3], to[3];
                for (uint32_t k = 0; k < 3; ++k)
                {
                    from[k] = rdf(rdram, mod::gpr32(ctx, kA0) + 4 * k);
                    to[k] = rdf(rdram, mod::gpr32(ctx, kA1) + 4 * k);
                }
                const float push = kCorpsePush[weapon] * static_cast<float>(g_force.load(std::memory_order_relaxed)) / 100.0f;
                int part = -1;
                if (const int id = physics::shoot(from, to, push, &part))
                {
                    ++g_corpseHits;
                    // a shot on a body down can take the part it met off (by the weapon's chance)
                    if (goreOn(rdram) && part > 0)
                        for (auto &[ped, doll] : g_dolls)
                            if (doll.id == id && !doll.knockdown && !doll.reaction)
                            {
                                const float chance = part == physics::kHead ? kSever[weapon].head : kSever[weapon].limb;
                                if (chance > 0.0f && goreRandom() < chance)
                                    sever(doll, ped, part, rd32(rdram, kTimeMs), "a shot on the body");
                            }
                }
            }
            return g_origLineOfSight(rdram, ctx, runtime);
        }

        // CWorld::TriggerExplosion(position* a0, ..., radius f12, power f13): the ragdolls within its radius are thrown,
        // and those its damage is about to make (build) will be.
        VCS_GUEST_CC void triggerExplosion(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive) // an older vcs.exe with these built in (jolt.h passive)
                return g_origExplosion(rdram, ctx, runtime);
            const float radius = mod::fpr(ctx)[12], power = mod::fpr(ctx)[13];
            if (g_enabled.load(std::memory_order_relaxed) && mod::gpr32(ctx, kA0) && radius > 0.0f && radius < 100.0f &&
                power > 0.0f)
            {
                Blast b;
                for (uint32_t k = 0; k < 3; ++k)
                    b.centre[k] = rdf(rdram, mod::gpr32(ctx, kA0) + 4 * k);
                b.radius = radius;
                b.speed = kBlastSpeed * std::min(power, 1000.0f) / 300.0f *
                          static_cast<float>(g_force.load(std::memory_order_relaxed)) / 100.0f;
                b.frame = rd32(rdram, kFrameCounter);
                if (g_blasts.size() >= 8)
                    g_blasts.erase(g_blasts.begin());
                g_blasts.push_back(b);
                unsigned thrown = 0;
                for (auto &[ped, doll] : g_dolls)
                {
                    const float *hips = doll.bones[2].m + 12;
                    const float d[3] = {hips[0] - b.centre[0], hips[1] - b.centre[1], hips[2] - b.centre[2]};
                    if (doll.id && length(d) < b.radius + 1.0f)
                    {
                        physics::blast(doll.id, b.centre, b.radius, b.speed);
                        if (goreOn(rdram) && !doll.knockdown && !doll.reaction)
                            severByBlast(doll, ped, b.centre, b.radius, rd32(rdram, kTimeMs));
                        ++thrown;
                    }
                }
                g_blasted += thrown;
                std::fprintf(stderr, "[jolt] blast at (%.1f, %.1f, %.1f), radius %.1f, power %.0f: %u ragdolls thrown\n",
                             b.centre[0], b.centre[1], b.centre[2], radius, power, thrown);
            }
            return g_origExplosion(rdram, ctx, runtime);
        }

        // CPhysical::ApplyMoveSpeed(entity a0): a ragdolled ped is where its body is. A knocked-down ped firing back this
        // frame: CWeapon::Fire(its weapon, it, no source) instead - a tail call, back to ApplyMoveSpeed's caller -, the
        // ped turned to its shooter and shifted for this frame so that the game's own muzzle (0.6 m up its axis from its
        // position) is its gun hand (the next step puts it back on its body).
        VCS_GUEST_CC void applyMoveSpeed(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive) // an older vcs.exe with these built in (jolt.h passive)
                return g_origMove(rdram, ctx, runtime);
            if (!g_dolls.empty())
                if (const auto it = g_dolls.find(mod::gpr32(ctx, kA0)); it != g_dolls.end() && !it->second.reaction)
                {
                    Doll &d = it->second;
                    const uint32_t ped = it->first;
                    if (d.bloodNow)
                    {
                        // a drop of blood: CParticle::AddParticle(blood, the ped's position, its move speed) - a tail
                        // call back to ApplyMoveSpeed's caller -, the two set to the stump and the drop's way for this
                        // frame (the next step puts the ped back on its body)
                        d.bloodNow = false;
                        for (uint32_t k = 0; k < 3; ++k)
                        {
                            wrf(rdram, ped + kPedPosition + 4 * k, d.dropAt[k]);
                            wrf(rdram, ped + kPedMoveSpeed + 4 * k, d.dropDir[k]);
                        }
                        d.followX = d.dropAt[0];
                        d.followY = d.dropAt[1];
                        mod::setGpr32(ctx, kA0, kBloodParticle);
                        mod::setGpr32(ctx, kA1, ped + kPedPosition);
                        mod::setGpr32(ctx, kA2, ped + kPedMoveSpeed);
                        mod::setGpr32(ctx, kA3, 0u);
                        mod::setGpr32(ctx, kT0, 0u);
                        mod::setGpr32(ctx, 9, 0u);
                        mod::setGpr32(ctx, 10, 0u);
                        mod::setGpr32(ctx, 11, 0u);
                        mod::fpr(ctx)[12] = 0.0f;
                        ++g_bloodDrops;
                        return g_origParticle(rdram, ctx, runtime);
                    }
                    if (d.fireNow && d.knockdown && d.shooter && generation(rdram, d.shooter) >= 0)
                    {
                        d.fireNow = false;
                        const float *hand = d.bones[15].m + 12; // the right hand
                        const float dx = rdf(rdram, d.shooter + kPedPosition) - hand[0],
                                    dy = rdf(rdram, d.shooter + kPedPosition + 4) - hand[1];
                        const float heading = std::atan2(-dx, dy), c = std::cos(heading), s = std::sin(heading);
                        const float right[3] = {c, s, 0.0f}, forward[3] = {-s, c, 0.0f}, up[3] = {0.0f, 0.0f, 1.0f};
                        for (uint32_t k = 0; k < 3; ++k)
                        {
                            wrf(rdram, ped + 0x00u + 4 * k, right[k]);
                            wrf(rdram, ped + 0x10u + 4 * k, forward[k]);
                            wrf(rdram, ped + 0x20u + 4 * k, up[k]);
                            wrf(rdram, ped + kPedPosition + 4 * k, hand[k] - (k == 2 ? kMuzzleUp : 0.0f));
                        }
                        wrf(rdram, ped + kPedHeading, heading);
                        wrf(rdram, ped + kPedHeading + 4, heading);
                        d.followX = hand[0];
                        d.followY = hand[1];
                        const uint32_t slot = rdram[(ped + kPedWeaponSlot) & kRamMask];
                        mod::setGpr32(ctx, kA0, ped + kPedWeapons + slot * kWeaponSize);
                        mod::setGpr32(ctx, kA1, ped);
                        mod::setGpr32(ctx, kA2, 0u);
                        ++g_blindShots;
                        return g_origFire(rdram, ctx, runtime);
                    }
                    mod::setPc(ctx, mod::gpr32(ctx, kRa));
                    return;
                }
            return g_origMove(rdram, ctx, runtime);
        }

        // CBike::KnockOffRider(bike a0, weapon a1, direction a2, rider a3, get back on t0): the rider, for a ragdoll at its
        // next draw once the game has thrown it off (SetFall: state 0x2D; a rider it kills goes through SetDie).
        VCS_GUEST_CC void knockOff(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive) // an older vcs.exe with these built in (jolt.h passive)
                return g_origKnockOff(rdram, ctx, runtime);
            const uint32_t rider = mod::gpr32(ctx, kA3);
            if (g_enabled.load(std::memory_order_relaxed) && g_bikeFalls.load(std::memory_order_relaxed) && rider &&
                !g_dolls.count(rider))
            {
                const int gen = generation(rdram, rider);
                if (gen >= 0)
                {
                    Knock &knock = g_knocked[rider];
                    knock.generation = gen;
                    knock.bike = mod::gpr32(ctx, kA0);
                    knock.time = rd32(rdram, kTimeMs);
                    knock.weapon = int(mod::gpr32(ctx, kA1));
                    knock.rammer = 0;
                    rammedBy(rdram, rider, knock);
                    ++g_knocks;
                    std::fprintf(stderr, "[jolt] ped %08X: thrown off bike %08X (weapon %d, direction %u, back on %u), "
                                 "state 0x%x, rammed by %08X at %.1f m/s\n", rider, knock.bike, knock.weapon,
                                 mod::gpr32(ctx, kA2) & 0xFFu, mod::gpr32(ctx, kT0), rd32(rdram, rider + kPedState),
                                 knock.rammer, length(knock.rammerSpeed));
                }
            }
            return g_origKnockOff(rdram, ctx, runtime);
        }

        // A gun's or cash's pickup object about to be drawn: no spin, glow or corona - it is put where its prop lies (made
        // the first time it is drawn: dropped from where the game put it). False: the game's effects for it.
        bool asProp(uint8_t *rdram, VcsGuestContext *ctx)
        {
            const uint32_t object = mod::gpr32(ctx, kA0);
            if (g_pickupsOn.load(std::memory_order_relaxed) && object)
            {
                auto it = g_props.find(object);
                if (it == g_props.end())
                {
                    const int slot = pickupSlot(rdram, object);
                    Prop prop;
                    if (slot >= 0 && loosePickup(rdram, uint32_t(slot)) && movingProps() < kMaxProps &&
                        makeProp(rdram, uint32_t(slot), object, prop))
                        it = g_props.emplace(object, prop).first;
                }
                if (it != g_props.end())
                {
                    it->second.seen = rd32(rdram, kTimeMs);
                    for (uint32_t r = 0; r < 4; ++r) // rows and position (their w lanes the game's)
                        std::memcpy(rdram + ((object + r * 0x10u) & kRamMask), it->second.rest.m + r * 4, 12);
                    mod::setPc(ctx, mod::gpr32(ctx, kRa));
                    return true;
                }
            }
            return false;
        }

        // CPickups::DoPickUpEffects(entity a0) and DoMoneyEffects(entity a0), from the render.
        VCS_GUEST_CC void pickupEffects(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive) // an older vcs.exe with these built in (jolt.h passive)
                return g_origPickupEffects(rdram, ctx, runtime);
            if (!asProp(rdram, ctx))
                return g_origPickupEffects(rdram, ctx, runtime);
        }
        VCS_GUEST_CC void moneyEffects(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive) // an older vcs.exe with these built in (jolt.h passive)
                return g_origMoneyEffects(rdram, ctx, runtime);
            if (!asProp(rdram, ctx))
                return g_origMoneyEffects(rdram, ctx, runtime);
        }

        // The loose part's draw for an element of a ped whose head went on whole (its tree in the element's first words):
        // a slot's matrices posed with that part alone (stumped's `loose`), a copy of the tree on them and a copy of the
        // element on that tree. 0 when the element is no such ped's or no slot is left this frame.
        uint32_t looseCopy(uint8_t *rdram, uint32_t element)
        {
            for (const auto &[ped, d] : g_dolls)
            {
                bool loose = false;
                for (int s = 0; s < d.stumps; ++s)
                    loose = loose || d.stumpKept[s];
                const auto tree = g_trees.find(ped);
                if (!loose || d.reaction || tree == g_trees.end())
                    continue;
                for (uint32_t k = 0; k < kElementCopy; k += 4)
                {
                    if ((rd32(rdram, element + k) & kRamMask) != (tree->second & kRamMask))
                        continue;
                    const uint32_t frame = rd32(rdram, kFrameCounter);
                    if (frame != g_poolFrame)
                    {
                        g_poolFrame = frame;
                        g_poolUsed = 0;
                    }
                    if (g_poolUsed >= kPoolSlots)
                        return 0;
                    const uint32_t slot = g_pool + g_poolUsed++ * kPoolSlot;
                    const uint32_t matrices = rd32(rdram, tree->second + kTreeMatrices);
                    physics::BoneMatrix part[kBones];
                    stumped(d, d.bones, part, true);
                    for (int b = 0; b < kBones; ++b) // the w lanes as the game's
                    {
                        std::memcpy(rdram + ((slot + kSlotMatrices + b * 0x40u) & kRamMask),
                                    rdram + ((matrices + b * 0x40u) & kRamMask), 0x40);
                        for (int r = 0; r < 4; ++r)
                            std::memcpy(rdram + ((slot + kSlotMatrices + b * 0x40u + r * 16u) & kRamMask), part[b].m + r * 4, 12);
                    }
                    std::memmove(rdram + ((slot + kSlotTree) & kRamMask), rdram + (tree->second & kRamMask), kTreeCopy);
                    wr32(rdram, slot + kSlotTree + kTreeMatrices, slot + kSlotMatrices);
                    std::memmove(rdram + ((slot + kSlotElement) & kRamMask), rdram + (element & kRamMask), kElementCopy);
                    wr32(rdram, slot + kSlotElement + k, slot + kSlotTree);
                    return slot + kSlotElement;
                }
            }
            return 0;
        }

        bool anyLoose()
        {
            for (const auto &[ped, d] : g_dolls)
                for (int s = 0; s < d.stumps; ++s)
                    if (d.stumpKept[s])
                        return true;
            return false;
        }

        VCS_GUEST_CC void elementRender(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive || g_drawStage != 0 || !g_secondDrawOk || !anyLoose())
                return g_origElementRender(rdram, ctx, runtime);
            const uint32_t element = mod::gpr32(ctx, kA0);
            if (!g_pool)
            {
                if (g_poolTried)
                    return g_origElementRender(rdram, ctx, runtime);
                // the slots first (once): the main heap's malloc, returning to kPoolMade, which calls this again
                g_poolTried = true;
                g_drawStage = -1;
                g_drawElement = element;
                g_drawReturn = mod::gpr32(ctx, kRa);
                mod::setGpr32(ctx, kA0, kPoolSlots * kPoolSlot + 64u);
                mod::setGpr32(ctx, kRa, kPoolMade);
                mod::setPc(ctx, kMainHeapAlloc);
                return;
            }
            const uint32_t copy = looseCopy(rdram, element);
            if (!copy)
                return g_origElementRender(rdram, ctx, runtime);
            g_drawStage = 1;
            g_drawElement = element;
            g_drawCopy = copy;
            g_drawReturn = mod::gpr32(ctx, kRa);
            mod::setGpr32(ctx, kRa, kElementAgain);
            return g_origElementRender(rdram, ctx, runtime);
        }

        VCS_GUEST_CC void poolMade(uint8_t *, VcsGuestContext *ctx, void *)
        {
            const uint32_t at = mod::gpr32(ctx, 2); // v0
            g_pool = at ? (at + 63u) & ~63u : 0;
            std::fprintf(stderr, "[jolt] severed parts' draw slots: %s 0x%08X\n", at ? "made at" : "no memory,", g_pool);
            g_drawStage = 0; // the call that asked for them, again
            mod::setGpr32(ctx, kA0, g_drawElement);
            mod::setGpr32(ctx, kRa, g_drawReturn);
            mod::setPc(ctx, kElementRender);
        }

        VCS_GUEST_CC void elementAgain(uint8_t *, VcsGuestContext *ctx, void *)
        {
            if (g_drawStage == 1)
            {
                g_drawStage = 2;
                ++g_looseDraws;
                mod::setGpr32(ctx, kA0, g_drawCopy);
                mod::setGpr32(ctx, kRa, kElementAgain);
                mod::setPc(ctx, kElementRender);
                return;
            }
            g_drawStage = 0;
            mod::setGpr32(ctx, 2, g_drawElement); // its v0: the element
            mod::setPc(ctx, g_drawReturn);
        }

        // FUN_00408c30, right after CWorld::Process (SetDie has run for this frame's deaths).
        VCS_GUEST_CC void afterWorld(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive) // an older vcs.exe with these built in (jolt.h passive)
                return g_origAfterWorld(rdram, ctx, runtime);
            if (!g_dolls.empty() || !g_pending.empty() || !g_knocked.empty() || !g_blends.empty() || !g_props.empty())
                frame(rdram);
            return g_origAfterWorld(rdram, ctx, runtime);
        }

        bool noted(uint32_t ped)
        {
            return g_pending.count(ped) || g_dolls.count(ped) || g_knocked.count(ped) || g_blends.count(ped) ||
                   g_hits.count(ped);
        }

        // An anim tree's bones built in a ped's render (FUN_003c3250, s0 = the ped): the tree of a noted / ragdolled ped.
        VCS_GUEST_CC void treeUpdate(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive) // an older vcs.exe with these built in (jolt.h passive)
                return g_origTree(rdram, ctx, runtime);
            if ((!g_pending.empty() || !g_dolls.empty() || !g_knocked.empty() || !g_blends.empty() || !g_hits.empty()) &&
                mod::gpr32(ctx, kRa) == kTreeUpdateInPedRender)
            {
                const uint32_t ped = mod::gpr32(ctx, kS0);
                if (noted(ped))
                    g_trees[ped] = mod::gpr32(ctx, kA0);
            }
            return g_origTree(rdram, ctx, runtime);
        }

        // The ped's render, its bones built, before the skinned draw: a noted ped gets its ragdoll, a ragdolled one its
        // pose, one getting up from its knockdown its body's last pose eased into the game's get-up.
        VCS_GUEST_CC void afterBones(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (g_passive) // an older vcs.exe with these built in (jolt.h passive)
                return g_origAfterBones(rdram, ctx, runtime);
            if ((!g_pending.empty() || !g_dolls.empty() || !g_knocked.empty() || !g_blends.empty() || !g_hits.empty()) &&
                mod::gpr32(ctx, kRa) == kAfterBonesInPedRender)
            {
                const uint32_t ped = mod::gpr32(ctx, kA0);
                const auto tree = g_trees.find(ped);
                if (tree != g_trees.end())
                {
                    const auto hit = g_hits.find(ped);
                    const bool freshHit = hit != g_hits.end() && g_enabled.load(std::memory_order_relaxed) &&
                                          g_hitReactions.load(std::memory_order_relaxed) &&
                                          rd32(rdram, kFrameCounter) - hit->second.frame <= kFreshHitFrames &&
                                          !hit->second.vehicle;
                    if (const auto doll = g_dolls.find(ped); doll != g_dolls.end() && doll->second.reaction)
                    {
                        // the animation's pose: the muscles' target; drawn, the physics' pose eased back into it
                        Doll &d = doll->second;
                        const uint32_t matrices = rd32(rdram, tree->second + kTreeMatrices);
                        if (matrices && rd32(rdram, tree->second + kTreeNodes) == uint32_t(kBones) && stillDown(rdram, ped, d))
                        {
                            const uint32_t ms = rd32(rdram, kTimeMs);
                            for (int i = 0; i < kBones; ++i)
                                std::memcpy(d.target[i].m, rdram + ((matrices + i * 0x40u) & kRamMask), 64);
                            // the animation running off (the ped sprints away once hit): the limbs' muscles can't keep
                            // up with it - the reaction fades out fast instead of the body trailing behind its hips
                            const float *hips = d.target[2].m + 12;
                            const float step[3] = {hips[0] - d.lastHips[0], hips[1] - d.lastHips[1], hips[2] - d.lastHips[2]};
                            if (d.hasTarget && ms > d.lastDraw && length(step) / (float(ms - d.lastDraw) / 1000.0f) > kReactRunSpeed &&
                                !d.running)
                            {
                                d.running = true;
                                d.runFrom = ms;
                            }
                            std::copy(hips, hips + 3, d.lastHips);
                            d.lastDraw = ms;
                            d.hasTarget = true;
                            if (freshHit)
                                react(rdram, ped, tree->second, hit->second);
                            const uint32_t t = ms - d.since;
                            float w = t < kReactHoldMs ? 1.0f
                                                       : std::max(0.0f, 1.0f - float(t - kReactHoldMs) /
                                                                                   float(kReactMs - kReactHoldMs));
                            if (d.running)
                                w = std::min(w, std::max(0.0f, 1.0f - float(ms - d.runFrom) / float(kReactRunFadeMs)));
                            // never drawn farther than kReactMaxOff off the animation: a hit, not a flail
                            float farthest = 0.0f;
                            for (int i = 0; i < kBones; ++i)
                            {
                                const float off[3] = {d.bones[i].m[12] - d.target[i].m[12], d.bones[i].m[13] - d.target[i].m[13],
                                                      d.bones[i].m[14] - d.target[i].m[14]};
                                farthest = std::max(farthest, length(off));
                            }
                            if (farthest * w > kReactMaxOff)
                                w = kReactMaxOff / farthest;
                            g_reactDeviation = std::max(g_reactDeviation, farthest * w); // as drawn
                            physics::BoneMatrix out[kBones];
                            physics::blendPose(d.target, d.bones, w, out, kBones);
                            drawExtras(out, d, ms);
                            write(rdram, tree->second, out);
                        }
                    }
                    else if (doll != g_dolls.end())
                    {
                        if (stillDown(rdram, ped, doll->second))
                        {
                            physics::BoneMatrix drawn[kBones];
                            stumped(doll->second, doll->second.bones, drawn);
                            write(rdram, tree->second, drawn);
                        }
                    }
                    else if (const auto death = g_pending.find(ped); death != g_pending.end())
                    {
                        const Death d = death->second;
                        g_pending.erase(death);
                        build(rdram, ped, tree->second, d);
                        if (!g_dolls.count(ped))
                            g_trees.erase(ped);
                    }
                    else if (const auto knock = g_knocked.find(ped); knock != g_knocked.end())
                    {
                        // thrown off (down: SetFall has run); until then it is still on its way off the bike
                        if (rd32(rdram, ped + kPedState) == kStateFall)
                        {
                            const Knock k = knock->second;
                            g_knocked.erase(knock);
                            Death d;
                            d.generation = k.generation;
                            d.frame = rd32(rdram, kFrameCounter);
                            build(rdram, ped, tree->second, d, &k);
                            if (!g_dolls.count(ped))
                                g_trees.erase(ped);
                        }
                    }
                    else if (freshHit && rd32(rdram, ped + kPedState) == kStateFall && !rd32(rdram, ped + kPedVehicle))
                    {
                        // knocked down by the hit (a shotgun close up, a punch): down as a ragdoll, up again after
                        const Hit h = hit->second;
                        Knock k;
                        k.generation = generation(rdram, ped);
                        k.time = rd32(rdram, kTimeMs);
                        k.weapon = h.weapon;
                        Death d;
                        d.generation = k.generation;
                        d.frame = rd32(rdram, kFrameCounter);
                        g_hits.erase(hit);
                        build(rdram, ped, tree->second, d, &k);
                        if (const auto doll = g_dolls.find(ped); doll != g_dolls.end() && doll->second.id)
                        {
                            blow(rdram, doll->second.id, h, doll->second.bones,
                                 static_cast<float>(g_force.load(std::memory_order_relaxed)) / 100.0f * kReactPush);
                            ++g_shotDown;
                            // a ped that shot it down: it may fire back from the ground
                            if (generation(rdram, h.attacker) >= 0 && !isPlayer(rdram, ped))
                                doll->second.shooter = h.attacker;
                        }
                        else
                            g_trees.erase(ped);
                    }
                    else if (freshHit && rd32(rdram, ped + kPedState) != kStateDying &&
                             rd32(rdram, ped + kPedState) != kStateDead && !rd32(rdram, ped + kPedVehicle) &&
                             !g_blends.count(ped))
                    {
                        react(rdram, ped, tree->second, hit->second);
                        if (!g_dolls.count(ped))
                            g_trees.erase(ped);
                    }
                    else if (const auto blend = g_blends.find(ped); blend != g_blends.end())
                    {
                        // the last pose while the game has not started the get-up, then turning into it
                        Blend &b = blend->second;
                        const uint32_t ms = rd32(rdram, kTimeMs), matrices = rd32(rdram, tree->second + kTreeMatrices);
                        if (!b.start && rd32(rdram, ped + kPedState) != kStateFall)
                            b.start = ms;
                        if (matrices && rd32(rdram, tree->second + kTreeNodes) == uint32_t(kBones))
                        {
                            physics::BoneMatrix game[kBones], out[kBones];
                            for (int i = 0; i < kBones; ++i)
                                std::memcpy(game[i].m, rdram + ((matrices + i * 0x40u) & kRamMask), 64);
                            physics::blendPose(b.bones, game, b.start ? float(ms - b.start) / float(kBlendMs) : 0.0f,
                                               out, kBones);
                            write(rdram, tree->second, out);
                        }
                    }
                }
            }
            return g_origAfterBones(rdram, ctx, runtime);
        }
    }

    void registerSettings()
    {
        using namespace mod;
        Spec s;
        s.key = "physics.ragdolls";
        s.page = "Game";
        s.section = "Physics";
        s.label = "Ragdolls (Jolt Physics)";
        s.help = "Peds go limp when they die, as with San Andreas' Jolt Physics mod: the body falls with the speed it had "
                 "instead of playing a death animation, and lands on the world's own collision - down stairs and slopes, "
                 "off ledges, against walls, cars and street furniture.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_JOLT";
        s.onChange = [](int v) { g_enabled = v != 0; };
        add(s);

        s = {};
        s.key = "physics.ragdoll_force";
        s.page = "Game";
        s.section = "Physics";
        s.label = "Hit force";
        s.help = "How hard the shot, blow or vehicle that kills a ped throws its body (shotguns hardest, close up). 0: it "
                 "only falls.";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 300;
        s.format = "%d %%";
        s.defaultValue = 100;
        s.onChange = [](int v) { g_force = v; };
        add(s);

        s = {};
        s.key = "physics.ragdoll_bikes";
        s.page = "Game";
        s.section = "Physics";
        s.label = "Bike falls";
        s.help = "With Ragdolls on: a rider thrown off a bike - Vic in a crash, a ped hit by a car or shot off it - tumbles "
                 "as a ragdoll and gets up where the body comes to rest.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_JOLT_BIKES";
        s.onChange = [](int v) { g_bikeFalls = v != 0; };
        add(s);

        s = {};
        s.key = "physics.ragdoll_hits";
        s.page = "Game";
        s.section = "Physics";
        s.label = "Hit reactions";
        s.help = "With Ragdolls on: a ped shot or hit and not killed reacts with its body where the bullet hits, as with "
                 "GTA IV's Euphoria - the head snaps back, a leg buckles, a shoulder spins -, puts a hand to the wound and "
                 "keeps going; a hard hit makes it stumble a few steps, and one that knocks it down drops it as a "
                 "ragdoll that gets up again.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_JOLT_HITS";
        s.onChange = [](int v) { g_hitReactions = v != 0; };
        add(s);

        s = {};
        s.key = "physics.ragdoll_catch";
        s.page = "Game";
        s.section = "Physics";
        s.label = "Catch falls";
        s.help = "With Ragdolls on: a ped knocked down (or thrown off a bike) puts its hands out toward the ground as it falls.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_JOLT_CATCH";
        s.onChange = [](int v) { g_catchFall = v != 0; };
        add(s);

        s = {};
        s.key = "physics.ragdoll_blindfire";
        s.page = "Game";
        s.section = "Physics";
        s.label = "Fire back from the ground";
        s.help = "With Ragdolls on: an armed ped shot down fires a few blind shots back at its shooter while it lies there.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_JOLT_BLINDFIRE";
        s.onChange = [](int v) { g_blindFire = v != 0; };
        add(s);

        s = {};
        s.key = "physics.gore";
        s.page = "Game";
        s.section = "Physics";
        s.label = "Dismemberment";
        s.help = "With Ragdolls on (and the game's gore on): a lethal shot or blade can take a limb or the head off - "
                 "shotguns, rifles and the heavy guns most -, a shot on a body already down too, and blasts limbs; the "
                 "stump bleeds. A head shot off rolls away whole (Keep severed heads); limbs are gone.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_JOLT_GORE";
        s.onChange = [](int v) { g_gore = v != 0; };
        add(s);

        s = {};
        s.key = "physics.gore_keep_heads";
        s.page = "Game";
        s.section = "Physics";
        s.label = "Keep severed heads";
        s.help = "With Dismemberment: a head shot off comes off whole and falls, bounces and rolls on the ground (Jolt "
                 "Physics) instead of bursting; the neck bleeds.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_JOLT_KEEP_HEADS";
        s.onChange = [](int v) { g_keepHeads = v != 0; };
        add(s);

        s = {};
        s.key = "physics.pickups";
        s.page = "Game";
        s.section = "Physics";
        s.label = "Pickups on the ground (GTA IV)";
        s.help = "Weapon and cash pickups lie on the ground at their real size, as in GTA IV, instead of spinning and "
                 "glowing in the air: each falls with Jolt Physics where the game puts it - a dead ped's gun and cash "
                 "thrown out a little - and lands on a side.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_JOLT_PICKUPS";
        s.onChange = [](int v) { g_pickupsOn = v != 0; };
        add(s);
    }

    bool playerRagdolled()
    {
        const uint8_t *ram = mod::api->rdram();
        if (g_passive || !ram)
            return false;
        const auto doll = g_dolls.find(rd32(ram, kPlayerPed));
        return doll != g_dolls.end() && !doll->second.reaction;
    }

    bool builtInFound()
    {
        const VcsGuestFn now = mod::api->lookup_function(kSetDie);
        return g_origSetDie && now && now != &setDie;
    }

    void install()
    {
        if (const char *e = std::getenv("VCS_JOLT_HOOKS"); e && e[0] == '0')
            return;
        if (const char *e = std::getenv("VCS_JOLT_WORLD"); e && e[0] == '0')
            g_worldOn = false;
        g_origSetDie = mod::api->lookup_function(kSetDie);
        g_origMove = mod::api->lookup_function(kApplyMoveSpeed);
        g_origAfterWorld = mod::api->lookup_function(kAfterWorld);
        g_origTree = mod::api->lookup_function(kTreeUpdate);       // first person's, when it is installed
        g_origAfterBones = mod::api->lookup_function(kAfterBones); // first person's, when it is installed
        g_origInflict = mod::api->lookup_function(kInflictDamage);
        g_origLineOfSight = mod::api->lookup_function(kLineOfSight); // the original: free aim installs after, around this
        g_origExplosion = mod::api->lookup_function(kTriggerExplosion);
        g_origKnockOff = mod::api->lookup_function(kKnockOffRider);
        g_origFire = mod::api->lookup_function(kFire); // called (tail) by applyMoveSpeed, not hooked
        g_origParticle = mod::api->lookup_function(kAddParticle); // the same
        g_origPickupEffects = mod::api->lookup_function(kPickupEffects);
        g_origMoneyEffects = mod::api->lookup_function(kMoneyEffects);
        if (!g_origSetDie || !g_origMove || !g_origAfterWorld || !g_origTree || !g_origAfterBones || !g_origInflict ||
            !g_origLineOfSight || !g_origExplosion || !g_origKnockOff || !g_origFire || !g_origParticle || !g_origPickupEffects || !g_origMoneyEffects)
            return;
        mod::api->replace_function(kSetDie, &setDie);
        mod::api->replace_function(kApplyMoveSpeed, &applyMoveSpeed);
        mod::api->replace_function(kAfterWorld, &afterWorld);
        mod::api->replace_function(kTreeUpdate, &treeUpdate);
        mod::api->replace_function(kAfterBones, &afterBones);
        mod::api->replace_function(kInflictDamage, &inflictDamage);
        mod::api->replace_function(kLineOfSight, &lineOfSight);
        mod::api->replace_function(kTriggerExplosion, &triggerExplosion);
        mod::api->replace_function(kKnockOffRider, &knockOff);
        mod::api->replace_function(kPickupEffects, &pickupEffects);
        mod::api->replace_function(kMoneyEffects, &moneyEffects);
        g_origElementRender = mod::api->lookup_function(kElementRender);
        if (g_origElementRender && VCS_MOD_API_HAS(mod::api, has_function) && !mod::api->has_function(kElementAgain) &&
            !mod::api->has_function(kPoolMade) && mod::api->register_function(kElementAgain, &elementAgain) &&
            mod::api->register_function(kPoolMade, &poolMade))
        {
            mod::api->replace_function(kElementRender, &elementRender);
            g_secondDrawOk = true;
        }
        else
            std::fprintf(stderr, "[jolt] no second draw (0x%08X taken): severed heads drawn as stumps\n", kElementAgain);
        // jolt -> ok enabled= dolls= ... ped= tree= awake= pelvis=<16 floats> (tests: tools/ragdoll_check.py)
        mod::addCommand("jolt", [](const std::string &, mod::Reply reply) {
            Status st;
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                st = g_status;
            }
            char text[1024];
            int n = std::snprintf(text, sizeof(text),
                                  "ok enabled=%d dolls=%d frozen=%d pending=%d steps=%u created=%u removed=%u bodies=%u "
                                  "active=%u obstacles=%d world=%d shapes=%d shapesmade=%u corpsehits=%u blasted=%u "
                                  "knocks=%u knockdowns=%u gotup=%u props=%d propsmoving=%d propsmade=%u reactions=%u "
                                  "reacting=%d shotdown=%u braced=%u blindshots=%u severed=%u blood=%u loosedraws=%u reactdev=%.3f clutches=%u clutchgap=%.3f stumbles=%u stumbled=%.2f stepms=%.3f ped=%08x tree=%08x awake=%d down=%d pelvis=",
                                  g_enabled ? 1 : 0, st.dolls, st.frozen, st.pending, st.steps, st.created, st.removed,
                                  st.physics.bodies, st.physics.activeBodies, st.obstacles, st.world, st.shapes,
                                  st.shapesMade, st.corpseHits, st.blasted, st.knocks, st.knockdowns, st.gotUp, st.props,
                                  st.propsMoving, st.propsMade, st.reactions, st.reacting, st.shotDown, st.braced, st.blindShots, st.severed, st.bloodDrops, g_looseDraws, st.reactDeviation, st.clutches, st.clutchGap, st.stumbles,
                                  st.lastStumble, st.physics.stepMs, st.ped, st.tree, st.awake ? 1 : 0, st.down ? 1 : 0);
            for (int i = 0; i < 16 && n > 0 && n < int(sizeof(text)); ++i)
                n += std::snprintf(text + n, sizeof(text) - n, i ? ",%.4f" : "%.4f", st.pelvis[i]);
            reply(text);
        });
    }
}
