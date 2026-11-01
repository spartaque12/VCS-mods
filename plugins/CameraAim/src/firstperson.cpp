// See firstperson.h and docs/game-internals.md 12c.
#include "firstperson.h"

#include "carglass.h"
#include "freeaim.h"
#include "mod.h"
#include "reload.h"
#include "saanims.h"
#include "viewmodel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace vcs::firstperson
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kPlayerPed = 0x004E4910u;   // CWorld::Players[0].m_pPed
        constexpr uint32_t kPedForward = 0x10u, kPedPosition = 0x30u; // the ped's matrix
        constexpr uint32_t kPedVehicle = 0x480u;       // the vehicle it is in (0 on foot)
        constexpr uint32_t kPedRotationCur = 0x8E0u, kPedRotationDest = 0x8E4u; // heading = atan2(-forward.x, forward.y)
        constexpr uint32_t kPedState = 0x8C4u, kPedStateIdle = 1, kPedStateAttack = 0x10, kPedStateFight = 0x11; // VC's
        constexpr uint32_t kFrameCounter = 0x004CD124u; // CTimer::m_FrameCounter
        constexpr uint32_t kTimeStep = 0x004CD110u;     // CTimer::ms_fTimeStep (1.0 = 1/50 s)

        // TheCamera and its CCams (game-internals.md 12b)
        constexpr uint32_t kTheCamera = 0x006F44D0u;
        constexpr uint32_t kActiveCam = kTheCamera + 0x50u; // byte: which of Cams[3] is active
        constexpr uint32_t kCams = kTheCamera + 0x70u, kCamSize = 0x270u;
        constexpr uint32_t kCamMode = 0x0u, kCamFront = 0x10u, kCamSource = 0x20u, kCamUp = 0x60u, kCamBeta = 0x7Cu,
                           kCamFov = 0x134u, kCamTarget = 0x1A0u, kCamStandA = 0x220u, kCamStandB = 0x230u;
        constexpr uint32_t kModeFollowPed = 4, kModeLockOn = 11;
        constexpr uint32_t kModeSniper = 7, kModeRocket = 8, kModeCamera = 46, kModeBinoculars = 47; // the game's own aims
        // in vehicles: 18 behind cars, bikes, helis and planes, 22 boats, 16 the game's own in-car view (Select)
        constexpr uint32_t kModeCar = 18, kModeBoat = 22, kModeInCar = 16;
        constexpr uint32_t kCamOrientation = 0x7E8u; // TheCamera's heading the player's pad is read against
        constexpr uint32_t kCamLookDirection = 0x74u; // after the game's update: 0 behind, 1 left, 2 right, 3 ahead (L2 / R2)
        constexpr uint32_t kVehicleType = 0x354u;     // 6 bike, 8 BMX (lean matrix at +0x3C0), ...
        constexpr uint32_t kVehicleLeanMatrix = 0x3C0u, kVehicleMoveSpeed = 0x140u; // speed x 50 = m/s
        // CDraw::SetFOV(f12) (0x487484) from CCamera::Process with the frame's field of view: the game's cameras' own
        // field of view (on foot, in vehicles) scaled there leaves their state (the car camera's speed widening) alone
        constexpr uint32_t kSetFov = 0x002653E0u, kSetFovInCameraProcess = 0x0037ADDCu;
        constexpr int kGameFov = 70;
        constexpr uint32_t kLooksAtPlayer = kTheCamera + 0x888u; // 0 under a script camera
        constexpr uint32_t kRwCamera = kTheCamera + 0x7F4u;       // the RW camera; its near plane at +0x78
        constexpr uint32_t kRwCameraNear = 0x78u;

        // the hooks (game-internals.md 12c)
        constexpr uint32_t kCamHeightRoutines = 0x002A2ED0u;       // CCam::ProcessSpecialHeightRoutines(cam a0)
        constexpr uint32_t kTreeUpdate = 0x0024E8D0u;              // bone matrices of an anim tree (a0)
        constexpr uint32_t kTreeUpdateInPedRender = 0x003C3280u;   // its return address in FUN_003c3250 (s0 = the ped)
        constexpr uint32_t kAfterBones = 0x0020C310u;              // a ped getter the ped's render calls next
        constexpr uint32_t kAfterBonesInPedRender = 0x00200740u;   // its return address there (a0 = the ped)
        constexpr uint32_t kTreeNodes = 0x4u, kTreeMatrices = 0x28u; // RslTAnimTree: node count, world matrices (0x40 each)
        constexpr uint32_t kPedBones = 25, kNeckBone = 4, kHeadBone = 5, kHeadEndBone = 6;

        // DualShock 2 buttons (vcs.exe's src/game/vcs_input.cpp)
        constexpr uint16_t kR1 = 0x0800, kCircle = 0x2000;

        constexpr float kPi = 3.14159265f, kTwoPi = 6.2831855f;
        constexpr float kPitchUp = 60.0f * kPi / 180.0f, kPitchDown = -80.0f * kPi / 180.0f; // UltimateFP's limits
        constexpr int kNearCm = 10;                          // the near plane (the game's 0.3 m cuts into the arms)
        constexpr float kEyeUp = 0.09f, kEyeForward = 0.075f; // the eyes from the head joint (the top of the neck)
        constexpr float kEyeJump = 0.35f; // m in one update: held out
        constexpr float kEyeAwayAhead = 0.15f; // m along the view per unit of (1 - view . body forward): 0.3 looking back
        // free aim's camera over the right shoulder: the pivot over the ped's position (its hips), the camera behind it
        // along the view, to its right and up a little; eased in and out from the game's camera
        constexpr float kAimPivotUp = 0.58f, kAimBack = 1.5f, kAimSide = 0.48f, kAimUp = 0.1f, kAimMinBack = 0.4f;
        constexpr float kAimBlendSeconds = 0.12f;

        // in a vehicle: the eyes from the head joint over the seat, ahead + 0.075 m, up 0.09 m (vehicle space); before
        // the first draw about a driver's
        constexpr float kSeatEye[3] = {-0.39f, -0.115f, 0.48f};
        constexpr float kRecentreAfter = 0.75f, kRecentreTime = 0.35f, kRecentreSpeed = 3.0f; // s, s, m/s
        // the vehicle's turn into the view eased over this (s): its body's jolts on bumps at speed would shake the view
        constexpr float kViewSmoothTime = 0.08f;
        // CCamera's shake (CamShake FUN_00376988 / FUN_00376a28: explosions, impacts ...): its force, which CCamera::Process
        // turns into a random offset of the final camera after the CCam's update (0x37AC5C..0x37AD5C)
        constexpr uint32_t kCamShakeForce = 0x006F44D0u + 0x7C0u;
        // a drive-by's camera in third person (freeaim.h): the pivot over the vehicle (its up), the camera behind
        // it along the aim, a little to the aim's right and up; the aim (radians about the vehicle's ahead, left +) kept
        // within kDriveArc of straight out of the chosen side
        constexpr float kDriveLift = 1.2f, kDriveLiftBike = 0.95f, kDriveBack = 3.2f, kDriveMinBack = 1.2f, kDriveSide = 0.35f,
                        kDriveUp = 0.35f;
        constexpr float kDriveArc = 1.35f, kDrivePitchStart = -0.08f, kDrivePitchDown = -0.6f, kDrivePitchUp = 0.5f;

        VcsGuestFn g_origHeight = nullptr, g_origTreeUpdate = nullptr, g_origAfterBones = nullptr,
                                       g_origSetFov = nullptr;
        std::atomic<bool> g_installed{false};
        std::atomic<bool> g_enabled{false};  // the setting / the key
        std::atomic<bool> g_vehicles{true};  // first person in vehicles too
        std::atomic<bool> g_looking{false};  // this frame: our view (the mouse turns it)
        std::atomic<int> g_fov{90};
        std::atomic<int> g_fovFoot{kGameFov}, g_fovVehicle{kGameFov}; // the game's own cameras' field of view
        std::atomic<int> g_nearCm{kNearCm};       // first person's near plane (cm)
        std::atomic<int> g_eyeCm[3] = {{0}, {0}, {0}}; // the eyes moved right / ahead / up (cm; the ped's or vehicle's axes)

        // game thread
        bool g_on = false;        // the camera is ours this frame
        float g_yaw = 0.0f, g_pitch = 0.0f;
        float g_aimBlend = 0.0f;  // the aim camera's share of the third-person view (0: the game's camera)
        float g_gameDistance = 0.0f; // the game's camera from the aim's pivot, this frame (status)
        uint32_t g_vehicle = 0;   // in a vehicle: the one the view is in (0 on foot)
        float g_lookIdle = 0.0f;  // in a vehicle: seconds without look input (recentring after)
        float g_viewF[3] = {}, g_viewU[3] = {}; // in a vehicle: its forward / up, eased (kViewSmoothTime)
        uint32_t g_headVehicle = 0, g_headFrame = 0; // the head joint in that vehicle's space, from the last draw
        float g_headLocal[3] = {};
        uint32_t g_tree = 0;      // the player's anim tree, from its last draw
        bool g_hideHead = false;  // the player's next draw collapses the head
        bool g_eyeValid = false;  // the eyes' offset from the ped's position, from its last draw
        float g_eye[3] = {};
        uint32_t g_dbgTreeFrame = 0, g_dbgAfterFrame = 0, g_dbgDoneFrame = 0, g_dbgOurs = 0; // status
        float g_fpView[3][3] = {}; // first person's source, front, up this update (setFov makes it the frame's camera)
        uint32_t g_fpViewFrame = 0;
        float g_camHead[3] = {}; // in / getting in or out of a vehicle: the head joint the camera assumed this update
        uint32_t g_camHeadFrame = 0;
        constexpr float kBodyFollowMax = 0.8f;
        constexpr float kCabinHalfWidth = 0.42f, kCabinSoft = 0.35f; // m from a car's middle; past it the head moves 35% // m: the drawn body moved to the camera at most
        float g_lastEye[3] = {}, g_lastPed[3] = {};
        uint32_t g_seatVehicle = 0; // the vehicle he last sat in and his head's height in it then
        float g_seatHeadZ = 0.0f;
        constexpr float kRoofAbove = 0.05f;  // m over the seated head: the roof's limit getting in and out
        constexpr float kDoorReach = 0.45f;  // m past the cabin half width: the door opening, under the roof edge
        constexpr uint32_t kPedStateDriving = 0x37u; // the eyes of the last update (jumps held out)
        bool g_lastEyeValid = false;
        uint32_t g_lastEyeFrame = 0, g_eyeHeld = 0;
        float g_eyeHead[3] = {kEyeForward, 0.0f, kEyeUp}; // the eyes from the head joint along its axes (world)
        uint32_t g_eyeFrame = 0;
        // the head joint and its axes (x up the head, y the face's forward) from the last draw: a ragdoll's view
        float g_headJoint[3] = {}, g_headUp[3] = {0.0f, 0.0f, 1.0f}, g_headAhead[3] = {1.0f, 0.0f, 0.0f};
        // Vic's body a ragdoll (JoltPhysics.asi: knocked down, thrown off a bike, dead): first person looks out of its
        // head, eased in and out (0 the mouse's view .. 1 the head's)
        constexpr float kRagdollIn = 0.2f, kRagdollOut = 0.5f; // seconds
        float g_ragdollBlend = 0.0f;
        float g_ragdollDir[3] = {1.0f, 0.0f, 0.0f}, g_ragdollUp[3] = {0.0f, 0.0f, 1.0f};
        bool g_nearSaved = false; // the game's near plane, given back when first person ends
        uint32_t g_nearCamera = 0;
        float g_nearGame = 0.3f;
        bool g_driveOn = false;   // the drive-by camera (third person) is ours
        float g_driveBlend = 0.0f; // its share of the view (0: the game's vehicle camera)
        uint32_t g_driveSide = 1; // 1 left, 2 right (the game's look direction)
        float g_driveYaw = 0.0f, g_drivePitch = 0.0f;

        std::mutex g_lookMutex;
        float g_lookYaw = 0.0f, g_lookPitch = 0.0f;

        // the VR mod's head look (setHeadLook, VR.asi): the head's yaw (left +) / pitch (up +) / roll (right +) about the
        // body's view, radians; first person on foot only
        std::mutex g_headMutex;
        bool g_headOn = false;
        float g_headYaw = 0.0f, g_headPitch = 0.0f, g_headRoll = 0.0f;
        float g_headPos[3] = {}; // the head's offset from where it was recentred: right, ahead (the body's), up - metres
        // VR: what of the player's body is drawn while the head look is on (camera.vr_body): 0 none (nothing between the
        // eyes and the world, the gun hidden too), 1 the arms and gun (the viewmodel; head, neck, torso, legs hidden),
        // 2 all but the head and neck (as on a monitor)
        std::atomic<int> g_vrBody{0};
        constexpr int kVrNearCm = 5; // VR's near plane at most: a headset's eye is right at the joint
        constexpr float kVrHeadReach = 0.6f; // m: the head's offset kept within this (leaning, not walking)
        float g_walkHeadYaw = 0.0f; // the head's yaw in this frame's view: taken out of the heading the player walks by
        std::atomic<uint32_t> g_headAppliedFrame{0}; // the game frame (CTimer::m_FrameCounter + 1) first person on foot last took the head look

        // for the control channel's "firstperson" (another thread)
        struct Status
        {
            bool on = false;
            float yaw = 0, pitch = 0, eye[3] = {};
            uint32_t tree = 0;
            float aim = 0, gameDistance = 0, drive = 0;
        };
        std::mutex g_statusMutex;
        Status g_status;

        void publish()
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status = {g_on, g_yaw, g_pitch, {g_eye[0], g_eye[1], g_eye[2]}, g_tree, g_aimBlend, g_gameDistance, g_driveBlend};
        }

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
        void wrVec(uint8_t *ram, uint32_t a, float x, float y, float z)
        {
            wrf(ram, a, x);
            wrf(ram, a + 4, y);
            wrf(ram, a + 8, z);
        }
        float wrap(float a) { return std::remainder(a, kTwoPi); } // -pi..pi

        // The game's near plane back (first person's is short).
        void restoreNear(uint8_t *ram)
        {
            if (g_nearSaved)
            {
                if (rd32(ram, kRwCamera) == g_nearCamera)
                    wrf(ram, g_nearCamera + kRwCameraNear, g_nearGame);
                g_nearSaved = false;
            }
        }

        // Our camera over (first person, the aim): the game's near plane back, its camera from the next update.
        void leave(uint8_t *ram)
        {
            g_looking = false;
            g_hideHead = false;
            g_aimBlend = 0.0f;
            restoreNear(ram);
            viewmodel::off(ram); // the arms the game's again
            if (g_on)
            {
                g_on = false;
                publish();
                mod::log("[firstperson] off\n");
            }
        }

        // The matrix a vehicle's view hangs on: a bike's lean matrix (it rolls into turns; its own stays level), else its own.
        uint32_t vehicleMatrix(const uint8_t *ram, uint32_t veh)
        {
            const uint32_t type = rd32(ram, veh + kVehicleType);
            return veh + (type == 6 || type == 8 ? kVehicleLeanMatrix : 0u);
        }

        // A car's driver's head sideways in its space, softly kept in the cabin: leaning out to open or close the door
        // put the eyes in the door and its pillar (the drawn body follows the camera, afterBones)
        float cabinX(float x)
        {
            const float a = std::fabs(x);
            return a <= kCabinHalfWidth ? x : std::copysign(kCabinHalfWidth + (a - kCabinHalfWidth) * kCabinSoft, x);
        }

        // First person's view written into the other two CCams as well: when the mode changes (getting in: 4 -> 18,
        // out: 18 -> 4) CCamera::Process blends from the previous CCam to the new one for a few frames, and the previous
        // one held the game's own camera - the blend went through the car's door and roof (dark frames). Blending
        // between equal views changes nothing; the game sets a CCam up afresh when it takes it for a new mode.
        void mirrorToOtherCams(uint8_t *ram, uint32_t cam)
        {
            for (uint32_t i = 0; i < 3; ++i)
            {
                const uint32_t other = kCams + i * kCamSize;
                if (other == cam)
                    continue;
                for (uint32_t off : {kCamFront, kCamSource, kCamUp, kCamTarget})
                    for (uint32_t k = 0; k < 12; k += 4)
                        wrf(ram, other + off + k, rdf(ram, cam + off + k));
                wrf(ram, other + kCamFov, rdf(ram, cam + kCamFov));
            }
        }

        // A short near plane while the camera is in a head.
        void shortNear(uint8_t *ram)
        {
            const uint32_t rw = rd32(ram, kRwCamera);
            if (!rw)
                return;
            if (!g_nearSaved || g_nearCamera != rw)
            {
                g_nearGame = rdf(ram, rw + kRwCameraNear);
                g_nearCamera = rw;
                g_nearSaved = true;
            }
            int nearCm = g_nearCm.load(std::memory_order_relaxed);
            if (headLookApplied()) // VR: the eye is at the joint and the head turns anywhere
                nearCm = std::min(nearCm, kVrNearCm);
            wrf(ram, rw + kRwCameraNear, static_cast<float>(nearCm) / 100.0f);
        }

        // One camera update in a vehicle with first person on: the eyes over the seat (the head joint of the last draw,
        // in the vehicle's space), the mouse / right stick look about the vehicle's ahead and ease back to it after a
        // while moving, L2 / R2 (both: behind) still look aside; the view rolls with the vehicle (a bike's lean too).
        // The game's camera keeps running underneath (nothing of it written but the view).
        bool updateVehicle(uint8_t *ram, uint32_t cam, uint32_t ped, uint32_t veh)
        {
            float dYaw, dPitch;
            {
                std::lock_guard<std::mutex> lock(g_lookMutex);
                dYaw = g_lookYaw;
                dPitch = g_lookPitch;
                g_lookYaw = g_lookPitch = 0.0f;
            }
            const float dt = std::clamp(rdf(ram, kTimeStep), 0.0f, 3.0f) / 50.0f;
            const bool fresh = !g_on || g_vehicle != veh;
            if (fresh)
            {
                g_on = true;
                g_vehicle = veh;
                g_yaw = g_pitch = g_lookIdle = 0.0f;
                mod::log("[firstperson] vehicle\n");
            }
            else
            {
                g_yaw = wrap(g_yaw + dYaw);
                g_pitch = std::clamp(g_pitch + dPitch, kPitchDown, kPitchUp);
            }
            g_lookIdle = dYaw != 0.0f || dPitch != 0.0f ? 0.0f : g_lookIdle + dt;
            const float vx = rdf(ram, veh + kVehicleMoveSpeed), vy = rdf(ram, veh + kVehicleMoveSpeed + 4),
                        vz = rdf(ram, veh + kVehicleMoveSpeed + 8);
            if (g_lookIdle > kRecentreAfter && std::sqrt(vx * vx + vy * vy + vz * vz) * 50.0f > kRecentreSpeed)
            {
                const float k = 1.0f - std::exp(-dt / kRecentreTime);
                g_yaw -= g_yaw * k;
                g_pitch -= g_pitch * k;
            }
            g_looking = true;
            g_hideHead = true;
            carglass::hide(veh); // its windows not drawn from inside (carglass.h)

            const uint32_t m = vehicleMatrix(ram, veh);
            float r[3], f[3], u[3], p[3];
            for (int i = 0; i < 3; ++i)
            {
                r[i] = rdf(ram, m + 4u * i);
                f[i] = rdf(ram, m + 0x10u + 4u * i);
                u[i] = rdf(ram, m + 0x20u + 4u * i);
                p[i] = rdf(ram, m + 0x30u + 4u * i);
            }
            // its turn eased (the body's jolts on bumps at speed stay out of the view; turns, slopes and a bike's lean
            // come through), its position as it is
            if (fresh)
            {
                std::copy(f, f + 3, g_viewF);
                std::copy(u, u + 3, g_viewU);
            }
            else
            {
                const float k = 1.0f - std::exp(-dt / kViewSmoothTime);
                for (int i = 0; i < 3; ++i)
                {
                    g_viewF[i] += (f[i] - g_viewF[i]) * k;
                    g_viewU[i] += (u[i] - g_viewU[i]) * k;
                }
            }
            auto normalise = [](float *v) {
                const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (l > 1e-5f)
                    for (int i = 0; i < 3; ++i)
                        v[i] /= l;
            };
            normalise(g_viewF);
            const float along = g_viewU[0] * g_viewF[0] + g_viewU[1] * g_viewF[1] + g_viewU[2] * g_viewF[2];
            for (int i = 0; i < 3; ++i)
                g_viewU[i] -= g_viewF[i] * along;
            normalise(g_viewU);
            std::copy(g_viewF, g_viewF + 3, f);
            std::copy(g_viewU, g_viewU + 3, u);
            r[0] = f[1] * u[2] - f[2] * u[1]; // right = forward x up
            r[1] = f[2] * u[0] - f[0] * u[2];
            r[2] = f[0] * u[1] - f[1] * u[0];
            wrf(ram, kCamShakeForce, 0.0f); // no camera shake inside (the third-person cameras keep theirs)
            const uint32_t look = rd32(ram, cam + kCamLookDirection);
            const float a = g_yaw + (look == 0 ? kPi : look == 1 ? kPi / 2.0f : look == 2 ? -kPi / 2.0f : 0.0f);
            const float sa = std::sin(a), ca = std::cos(a), sp = std::sin(g_pitch), cp = std::cos(g_pitch);
            const float dl[3] = {-sa * cp, ca * cp, sp}, ul[3] = {sa * sp, -ca * sp, cp}; // vehicle space: x right, y ahead
            const bool head = g_headVehicle == veh && rd32(ram, kFrameCounter) - g_headFrame <= 4u;
            // the head joint (a car's driver kept inside the cabin: closing the door he leans out into its pillar)
            float hl[3] = {head ? g_headLocal[0] : kSeatEye[0], head ? g_headLocal[1] : kSeatEye[1] - kEyeForward,
                           head ? g_headLocal[2] : kSeatEye[2] - kEyeUp};
            if (rd32(ram, veh + kVehicleType) == 0)
                hl[0] = cabinX(hl[0]);
            if (head && rd32(ram, ped + kPedState) == kPedStateDriving)
            {
                g_seatVehicle = veh; // seated: his head's height in it (the roof's limit getting in and out)
                g_seatHeadZ = g_headLocal[2];
            }
            const float el[3] = {hl[0] + g_eyeCm[0].load(std::memory_order_relaxed) / 100.0f,
                                 hl[1] + kEyeForward + g_eyeCm[1].load(std::memory_order_relaxed) / 100.0f,
                                 hl[2] + kEyeUp + g_eyeCm[2].load(std::memory_order_relaxed) / 100.0f};
            float src[3], dir[3], up[3];
            for (int i = 0; i < 3; ++i)
            {
                src[i] = p[i] + r[i] * el[0] + f[i] * el[1] + u[i] * el[2];
                dir[i] = r[i] * dl[0] + f[i] * dl[1] + u[i] * dl[2];
                up[i] = r[i] * ul[0] + f[i] * ul[1] + u[i] * ul[2];
            }
            for (int i = 0; i < 3; ++i) // where this view has the head joint: the draw puts it there (afterBones)
                g_camHead[i] = p[i] + r[i] * hl[0] + f[i] * hl[1] + u[i] * hl[2];
            g_camHeadFrame = head ? rd32(ram, kFrameCounter) : 0u;
            wrVec(ram, cam + kCamSource, src[0], src[1], src[2]);
            wrVec(ram, cam + kCamFront, dir[0], dir[1], dir[2]);
            wrVec(ram, cam + kCamUp, up[0], up[1], up[2]);
            wrVec(ram, cam + kCamTarget, src[0] + dir[0], src[1] + dir[1], src[2] + dir[2]);
            wrf(ram, cam + kCamFov, static_cast<float>(g_fov.load(std::memory_order_relaxed)));
            mirrorToOtherCams(ram, cam);
            shortNear(ram);
            // a drive-by (the game's: L2 / R2 looking aside with an SMG) aims along this view
            freeaim::frameDriveBy(ram, ped, (look == 1 || look == 2) && freeaim::wantsDriveBy(ram, ped),
                                  freeaim::View::FirstPerson, src, dir);
            publish();
            return true;
        }

        // The drive-by camera given back: the game's vehicle camera from the next update.
        void endDriveBy(uint8_t *ram)
        {
            g_driveOn = false;
            g_driveBlend = 0.0f;
            leave(ram);
        }

        // Third person in a vehicle while the player aims a drive-by (the game's own: L2 / R2 looking aside with an SMG,
        // Circle fires): a camera over the vehicle on the far side, looking out along the aim - the mouse / right stick
        // aim it within kDriveArc of that side - eased in from the game's camera and back out. Its line carries the shots
        // and the dot (freeaim.h). False once eased out: the game's camera.
        bool updateDriveBy(uint8_t *ram, uint32_t cam, uint32_t ped, uint32_t veh, bool aiming, uint32_t look)
        {
            float dYaw, dPitch;
            {
                std::lock_guard<std::mutex> lock(g_lookMutex);
                dYaw = g_lookYaw;
                dPitch = g_lookPitch;
                g_lookYaw = g_lookPitch = 0.0f;
            }
            const float step = std::clamp(rdf(ram, kTimeStep), 0.0f, 3.0f) / (50.0f * kAimBlendSeconds);
            g_driveBlend = std::clamp(g_driveBlend + (aiming ? step : -step), 0.0f, 1.0f);
            if (!aiming && g_driveBlend <= 0.0f)
            {
                endDriveBy(ram);
                return false;
            }
            if (aiming && (!g_driveOn || look != g_driveSide))
            {
                // straight out of that side's windows, a little down
                g_driveSide = look;
                g_driveYaw = look == 1 ? kPi / 2.0f : -kPi / 2.0f;
                g_drivePitch = kDrivePitchStart;
                dYaw = dPitch = 0.0f;
            }
            if (!g_driveOn)
            {
                g_driveOn = true;
                g_on = true; // ours: the field-of-view hook leaves it alone
                mod::log("[firstperson] drive-by\n");
            }
            if (aiming)
            {
                const float mid = g_driveSide == 1 ? kPi / 2.0f : -kPi / 2.0f;
                g_driveYaw = std::clamp(g_driveYaw + dYaw, mid - kDriveArc, mid + kDriveArc);
                g_drivePitch = std::clamp(g_drivePitch + dPitch, kDrivePitchDown, kDrivePitchUp);
            }
            g_looking = aiming; // the mouse and the right stick aim (the game's camera gets no stick)

            // the aim about the vehicle's heading (its pitch and roll left out: the view stays level)
            float f[3], u[3], p[3];
            for (int i = 0; i < 3; ++i)
            {
                f[i] = rdf(ram, veh + 0x10u + 4u * i);
                u[i] = rdf(ram, veh + 0x20u + 4u * i);
                p[i] = rdf(ram, veh + 0x30u + 4u * i);
            }
            const float yaw = std::atan2(f[1], f[0]) + g_driveYaw;
            const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(g_drivePitch), sp = std::sin(g_drivePitch);
            const float front[3] = {cp * cy, cp * sy, sp}, right[3] = {sy, -cy, 0.0f};
            const uint32_t type = rd32(ram, veh + kVehicleType);
            const float lift = type == 6 || type == 8 ? kDriveLiftBike : kDriveLift;
            const float pivot[3] = {p[0] + u[0] * lift, p[1] + u[1] * lift, p[2] + u[2] * lift};
            // no further out than the game's camera (looking aside, its line of sight checked)
            const float gs[3] = {rdf(ram, cam + kCamSource), rdf(ram, cam + kCamSource + 4), rdf(ram, cam + kCamSource + 8)};
            const float gf[3] = {rdf(ram, cam + kCamFront), rdf(ram, cam + kCamFront + 4), rdf(ram, cam + kCamFront + 8)};
            const float gameFov = rdf(ram, cam + kCamFov);
            g_gameDistance = std::sqrt((gs[0] - pivot[0]) * (gs[0] - pivot[0]) + (gs[1] - pivot[1]) * (gs[1] - pivot[1]) +
                                       (gs[2] - pivot[2]) * (gs[2] - pivot[2]));
            const float back = std::clamp(g_gameDistance - 0.2f, kDriveMinBack, kDriveBack), k = back / kDriveBack;
            const float t = g_driveBlend * g_driveBlend * (3.0f - 2.0f * g_driveBlend); // smoothstep
            float src[3], dir[3], len = 0.0f;
            for (int i = 0; i < 3; ++i)
            {
                const float aim = pivot[i] - front[i] * back + right[i] * kDriveSide * k + (i == 2 ? kDriveUp * k : 0.0f);
                src[i] = gs[i] + (aim - gs[i]) * t;
                dir[i] = gf[i] + (front[i] - gf[i]) * t;
                len += dir[i] * dir[i];
            }
            len = std::sqrt(len);
            for (float &v : dir)
                v = len > 1e-4f ? v / len : 0.0f;
            if (len <= 1e-4f)
                std::copy(front, front + 3, dir);
            const float fov = (gameFov >= 10.0f && gameFov <= 130.0f ? gameFov : 70.0f) * (1.0f - t) + freeaim::fov() * t;
            const float h = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]); // no roll
            wrVec(ram, cam + kCamSource, src[0], src[1], src[2]);
            wrVec(ram, cam + kCamFront, dir[0], dir[1], dir[2]);
            wrVec(ram, cam + kCamUp, h > 1e-4f ? -dir[0] / h * dir[2] : 0.0f, h > 1e-4f ? -dir[1] / h * dir[2] : 0.0f, h);
            wrVec(ram, cam + kCamTarget, src[0] + dir[0], src[1] + dir[1], src[2] + dir[2]);
            wrf(ram, cam + kCamFov, fov);
            restoreNear(ram);
            freeaim::frameDriveBy(ram, ped, aiming, freeaim::View::Aim, src, dir);
            publish();
            return true;
        }

        // The game's own first-person aims on foot - the sniper rifles' scope (mode 7), the rocket launcher (8), the camera
        // (46) and binoculars (47): the game's own camera aims them from the stick only (the mouse came through as a
        // right stick it does not read for them), so their front is ours here - the mouse / right stick, slowed with the
        // scope's zoom (its field of view against 70). The game keeps the camera's place and zoom; its shot goes along
        // the camera's front (game-internals.md 12t). Leaving it, first person goes on facing the same way.
        bool g_scope = false;
        bool updateScope(uint8_t *ram, uint32_t cam, uint32_t ped)
        {
            float dYaw, dPitch;
            {
                std::lock_guard<std::mutex> lock(g_lookMutex);
                dYaw = g_lookYaw;
                dPitch = g_lookPitch;
                g_lookYaw = g_lookPitch = 0.0f;
            }
            const float gf[3] = {rdf(ram, cam + kCamFront), rdf(ram, cam + kCamFront + 4), rdf(ram, cam + kCamFront + 8)};
            if (!g_scope)
            {
                g_scope = true;
                if (!g_on) // from the game's camera; out of first person its view
                {
                    g_yaw = std::atan2(gf[1], gf[0]);
                    g_pitch = std::clamp(std::asin(std::clamp(gf[2], -1.0f, 1.0f)), kPitchDown, kPitchUp);
                }
                dYaw = dPitch = 0.0f;
                viewmodel::off(ram);
                mod::log("[firstperson] scope\n");
            }
            const float fov = rdf(ram, cam + kCamFov);
            const float zoom = std::clamp((fov >= 1.0f && fov <= 130.0f ? fov : 70.0f) / 70.0f, 0.03f, 1.5f);
            g_yaw = wrap(g_yaw + dYaw * zoom);
            g_pitch = std::clamp(g_pitch + dPitch * zoom, kPitchDown, kPitchUp);
            g_on = true;
            g_looking = true;
            g_hideHead = false;
            restoreNear(ram);
            const float cy = std::cos(g_yaw), sy = std::sin(g_yaw), cp = std::cos(g_pitch), sp = std::sin(g_pitch);
            const float dir[3] = {cp * cy, cp * sy, sp}, up[3] = {-cy * sp, -sy * sp, cp};
            const float src[3] = {rdf(ram, cam + kCamSource), rdf(ram, cam + kCamSource + 4), rdf(ram, cam + kCamSource + 8)};
            wrVec(ram, cam + kCamFront, dir[0], dir[1], dir[2]);
            wrVec(ram, cam + kCamUp, up[0], up[1], up[2]);
            wrVec(ram, cam + kCamTarget, src[0] + dir[0], src[1] + dir[1], src[2] + dir[2]);
            // the body with the aim
            const float heading = wrap(g_yaw - kPi / 2.0f);
            wrf(ram, ped + kPedRotationCur, heading);
            wrf(ram, ped + kPedRotationDest, heading);
            freeaim::frame(ram, ped, false, freeaim::View::Game, nullptr, nullptr, 0.0f, 0.0f);
            publish();
            return true;
        }

        // One update of the active camera: in a vehicle (first person), on foot - first person's view, free aim's
        // camera over the shoulder (eased in from the game's and back out) - or the game's. False: the game's camera
        // (its height routines run).
        // JoltPhysics.asi's JoltPhysicsPlayerRagdoll (looked up until found: the mods load in name order)
        bool playerRagdoll()
        {
#if defined(_WIN32)
            using RagdollFn = int (*)(void);
            static RagdollFn fn = nullptr;
            static unsigned tries = 0;
            if (!fn && tries < 600)
            {
                ++tries;
                if (HMODULE m = GetModuleHandleW(L"JoltPhysics.asi"))
                    fn = reinterpret_cast<RagdollFn>(GetProcAddress(m, "JoltPhysicsPlayerRagdoll"));
            }
            return fn && fn() != 0;
#else
            return false;
#endif
        }

        // A ragdoll's first person: the eyes on its head's axes and the view along its face, rolling with it - eased in
        // when the body goes limp and back out to the mouse's view (from where the head ended up) when he is up again.
        void ragdollView(const uint8_t *ram, bool ragdoll, uint32_t frame, float *src, float *dir)
        {
            const float step = std::clamp(rdf(ram, kTimeStep), 0.0f, 3.0f) / 50.0f;
            const bool fresh = g_eyeValid && frame - g_eyeFrame <= 4u;
            const float was = g_ragdollBlend;
            g_ragdollBlend = ragdoll && fresh ? std::min(1.0f, g_ragdollBlend + step / kRagdollIn)
                                              : std::max(0.0f, g_ragdollBlend - step / kRagdollOut);
            if (ragdoll && fresh)
            {
                std::copy(g_headAhead, g_headAhead + 3, g_ragdollDir);
                std::copy(g_headUp, g_headUp + 3, g_ragdollUp);
            }
            if (g_ragdollBlend <= 0.0f)
                return;
            if (was > 0.0f && g_ragdollBlend < was && !ragdoll)
            {
                // easing out: the mouse's view starts level, the way the face last looked
                g_yaw = std::atan2(g_ragdollDir[1], g_ragdollDir[0]);
                g_pitch = 0.0f;
            }
            float l = 0.0f;
            for (int i = 0; i < 3; ++i)
            {
                if (fresh)
                    src[i] += (g_headJoint[i] + g_eyeHead[i] - src[i]) * g_ragdollBlend;
                dir[i] += (g_ragdollDir[i] - dir[i]) * g_ragdollBlend;
                l += dir[i] * dir[i];
            }
            l = std::sqrt(l);
            for (int i = 0; i < 3; ++i)
                dir[i] = l > 1e-4f ? dir[i] / l : g_ragdollDir[i];
        }

        bool update(uint8_t *ram, uint32_t cam)
        {
            const uint32_t ped = rd32(ram, kPlayerPed);
            const uint32_t mode = rd32(ram, cam + kCamMode) & 0xFFFFu;
            const uint32_t veh = ped ? rd32(ram, ped + kPedVehicle) : 0;
            const bool looksAtPlayer = rd32(ram, kLooksAtPlayer) == 1;
            if (veh && g_enabled && g_vehicles && looksAtPlayer && (mode == kModeCar || mode == kModeBoat || mode == kModeInCar))
            {
                if (g_driveOn)
                    endDriveBy(ram); // first person switched on mid drive-by: its own view (it aims the drive-by too)
                return updateVehicle(ram, cam, ped, veh);
            }
            carglass::hide(0);
            if (g_vehicle)
            {
                leave(ram); // out of the vehicle's view: on foot starts from the game's camera
                g_vehicle = 0;
            }
            // third person behind a vehicle: a drive-by's camera while the game's drive-by is aimed (L2 / R2 with an SMG)
            if (veh && looksAtPlayer && mode == kModeCar)
            {
                const uint32_t look = rd32(ram, cam + kCamLookDirection);
                const bool aiming = (look == 1 || look == 2) && freeaim::wantsDriveBy(ram, ped);
                if ((aiming || g_driveOn) && updateDriveBy(ram, cam, ped, veh, aiming, look))
                    return true;
            }
            else if (g_driveOn)
                endDriveBy(ram); // out of the vehicle, under a script camera, another mode: the game's camera
            const bool onFoot = ped && rd32(ram, ped + kPedVehicle) == 0 && rd32(ram, kLooksAtPlayer) == 1;
            if (onFoot && (mode == kModeSniper || mode == kModeRocket || mode == kModeCamera || mode == kModeBinoculars))
                return updateScope(ram, cam, ped); // the scopes: aimed by the mouse
            if (g_scope)
            {
                g_scope = false;
                if (!g_enabled)
                    leave(ram); // third person: the game's camera again
            }
            // first person on foot - and while getting in or out of a vehicle: the ped has it (+0x480) from the start of
            // the enter anim to the end of the exit anim, the game's follow camera (mode 4) still on (game-internals.md 12t)
            const bool ragdoll = g_enabled && ped && rd32(ram, ped + kPedVehicle) == 0 && playerRagdoll();
            const bool first = g_enabled && ped && rd32(ram, kLooksAtPlayer) == 1 &&
                               (mode == kModeFollowPed || mode == kModeLockOn || ragdoll);
            // free aim (freeaim.h): R1 with a gun, the game's follow camera behind the player
            const bool aiming = onFoot && mode == kModeFollowPed && freeaim::wantsAim(ram, ped);
            const float step = std::clamp(rdf(ram, kTimeStep), 0.0f, 3.0f) / (50.0f * kAimBlendSeconds);
            g_aimBlend = first || !onFoot || mode != kModeFollowPed ? 0.0f : std::clamp(g_aimBlend + (aiming ? step : -step), 0.0f, 1.0f);
            if (!first && g_aimBlend <= 0.0f)
            {
                leave(ram);
                freeaim::frame(ram, ped, false, freeaim::View::Game, nullptr, nullptr, 0.0f, 0.0f);
                return false;
            }
            const float gs[3] = {rdf(ram, cam + kCamSource), rdf(ram, cam + kCamSource + 4), rdf(ram, cam + kCamSource + 8)};
            const float gf[3] = {rdf(ram, cam + kCamFront), rdf(ram, cam + kCamFront + 4), rdf(ram, cam + kCamFront + 8)};
            const float gameFov = rdf(ram, cam + kCamFov);
            float dYaw, dPitch;
            {
                std::lock_guard<std::mutex> lock(g_lookMutex);
                dYaw = g_lookYaw;
                dPitch = g_lookPitch;
                g_lookYaw = g_lookPitch = 0.0f;
            }
            if (!g_on)
            {
                // from where the game's camera looks: no jump; level
                g_on = true;
                g_yaw = std::atan2(gf[1], gf[0]);
                g_pitch = 0.0f;
                dYaw = dPitch = 0.0f;
                mod::log("[firstperson] %s\n", first ? "on" : "aim");
            }
            if (mode == kModeLockOn)
            {
                // locked on: the game turns the player to the target, and the gun points where he faces - the view
                // with him. (The lock-on camera itself stands behind his shoulder looking past him: its direction is
                // not the one from his eyes.) The pitch is the lock-on camera's tilt.
                g_yaw = std::atan2(rdf(ram, ped + kPedForward + 4), rdf(ram, ped + kPedForward));
                g_pitch = std::clamp(std::asin(std::clamp(gf[2], -1.0f, 1.0f)), kPitchDown, kPitchUp);
            }
            else if ((first || aiming) && !ragdoll && g_ragdollBlend <= 0.0f) // (the aim camera easing out stays where it was)
            {
                g_yaw = wrap(g_yaw + dYaw);
                g_pitch = std::clamp(g_pitch + dPitch, kPitchDown, kPitchUp);
            }
            // the mouse turns our view (not a ragdoll's: the body is the physics', the view its head's)
            const bool turning = mode == kModeFollowPed && (first || aiming) && !ragdoll && g_ragdollBlend <= 0.0f;
            g_looking = turning;
            g_hideHead = first;

            // the VR mod's head look: the view is the body's yaw (mouse / stick) turned by the head; the head alone pitches
            bool head = false;
            float headYaw = 0.0f, headPitch = 0.0f, headRoll = 0.0f, headPos[3] = {};
            {
                std::lock_guard<std::mutex> lock(g_headMutex);
                head = g_headOn && first && (mode == kModeFollowPed || mode == kModeLockOn) && !ragdoll;
                headYaw = g_headYaw, headPitch = g_headPitch, headRoll = g_headRoll;
                std::copy(g_headPos, g_headPos + 3, headPos);
            }
            if (head)
            {
                g_headAppliedFrame.store(rd32(ram, kFrameCounter) + 1u, std::memory_order_relaxed);
                g_walkHeadYaw = headYaw;
                g_pitch = 0.0f;
            }
            const float viewYaw = head ? wrap(g_yaw + headYaw) : g_yaw;
            const float viewPitch = head ? std::clamp(headPitch, -1.55f, 1.55f) : g_pitch;

            const float px = rdf(ram, ped + kPedPosition), py = rdf(ram, ped + kPedPosition + 4), pz = rdf(ram, ped + kPedPosition + 8);
            const float cy = std::cos(viewYaw), sy = std::sin(viewYaw), cp = std::cos(viewPitch), sp = std::sin(viewPitch);
            const float front[3] = {cp * cy, cp * sy, sp};
            float src[3], dir[3], fov;
            if (first)
            {
                // the eyes: the head joint of the last draw, carried with the ped (no lag while running)
                const uint32_t frame = rd32(ram, kFrameCounter);
                src[0] = px, src[1] = py, src[2] = pz + 0.75f; // before the first draw: about eye height over the hips
                if (g_eyeValid && frame - g_eyeFrame <= 4u)
                {
                    src[0] = px + g_eye[0];
                    src[1] = py + g_eye[1];
                    src[2] = pz + g_eye[2];
                }
                // getting on or off a vehicle: the head joint kept in its space (a bike's rider is drawn in its lean
                // matrix, off his own position while the anim plays)
                if (const uint32_t veh = rd32(ram, ped + kPedVehicle); veh && g_headVehicle == veh && frame - g_headFrame <= 4u)
                {
                    const uint32_t m = vehicleMatrix(ram, veh);
                    const bool car = rd32(ram, veh + kVehicleType) == 0;
                    const float hx0 = car ? cabinX(g_headLocal[0]) : g_headLocal[0];
                    // in a car's cabin the head kept under its roof: the anims stand him up through it getting out
                    float hz0 = g_headLocal[2];
                    if (car && std::fabs(g_headLocal[0]) < kCabinHalfWidth + kDoorReach)
                    {
                        const float roof = (g_seatVehicle == veh ? g_seatHeadZ : kSeatEye[2] - kEyeUp) + kRoofAbove;
                        if (hz0 > roof)
                            hz0 = roof + (hz0 - roof) * kCabinSoft;
                    }
                    for (int i = 0; i < 3; ++i)
                        src[i] = rdf(ram, m + 0x30u + 4u * i) + rdf(ram, m + 4u * i) * hx0 +
                                 rdf(ram, m + 0x10u + 4u * i) * g_headLocal[1] + rdf(ram, m + 0x20u + 4u * i) * hz0;
                    for (int i = 0; i < 3; ++i)
                        src[i] += g_eyeHead[i];
                    for (int i = 0; i < 3; ++i) // the head joint this view assumes: the draw brings it there (afterBones)
                        g_camHead[i] = src[i] - g_eyeHead[i];
                    g_camHeadFrame = frame;
                }
                // the settings' offset, along the ped's right / forward and up
                const float ox = g_eyeCm[0].load(std::memory_order_relaxed) / 100.0f,
                            oy = g_eyeCm[1].load(std::memory_order_relaxed) / 100.0f,
                            oz = g_eyeCm[2].load(std::memory_order_relaxed) / 100.0f;
                for (int i = 0; i < 2; ++i)
                    src[i] += rdf(ram, ped + 4u * i) * ox + rdf(ram, ped + kPedForward + 4u * i) * oy;
                src[2] += oz;
                // looking away from where the body faces (running backwards or sideways the game turns him to the run):
                // the eyes moved out along the view, ahead of the collar and shoulders that would fill the picture
                {
                    const float fx = rdf(ram, ped + kPedForward), fy = rdf(ram, ped + kPedForward + 4);
                    // (not in VR: the head turns there itself, and the body the eyes would clear is hidden)
                    const float away = head ? 0.0f : 1.0f - std::clamp(fx * cy + fy * sy, -1.0f, 1.0f); // 0 ahead .. 2 behind
                    src[0] += cy * kEyeAwayAhead * away;
                    src[1] += sy * kEyeAwayAhead * away;
                }
                // VR: the head's own movement (leaning, ducking) about where it was recentred, in the body's frame
                if (head)
                {
                    const float by = g_yaw, bc = std::cos(by), bs = std::sin(by);
                    src[0] += bs * headPos[0] + bc * headPos[1];
                    src[1] += -bc * headPos[0] + bs * headPos[1];
                    src[2] += headPos[2];
                }
                // a jump of the eyes in one update (the frame a rider gets off a bike: his position and the drawn
                // skeleton disagree for a frame or two) is held out, three updates at most
                {
                    const float d[3] = {src[0] - g_lastEye[0], src[1] - g_lastEye[1], src[2] - g_lastEye[2]};
                    const bool jump = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > kEyeJump * kEyeJump;
                    // (on foot only the frame his position is set anew - out of a vehicle: the eyes from the last draw
                    // are carried with a position the draw did not have)
                    const float pj[3] = {px - g_lastPed[0], py - g_lastPed[1], pz - g_lastPed[2]};
                    const bool snapped = pj[0] * pj[0] + pj[1] * pj[1] + pj[2] * pj[2] > kEyeJump * kEyeJump;
                    if (g_lastEyeValid && jump && g_eyeHeld < 3 && frame - g_lastEyeFrame <= 2u && (rd32(ram, ped + kPedVehicle) || snapped))
                    {
                        std::copy(g_lastEye, g_lastEye + 3, src);
                        ++g_eyeHeld;
                    }
                    else
                        g_eyeHeld = 0;
                    std::copy(src, src + 3, g_lastEye);
                    g_lastPed[0] = px, g_lastPed[1] = py, g_lastPed[2] = pz;
                    g_lastEyeValid = true;
                    g_lastEyeFrame = frame;
                }
                std::copy(front, front + 3, dir);
                fov = static_cast<float>(g_fov.load(std::memory_order_relaxed));
                ragdollView(ram, ragdoll, frame, src, dir);
            }
            else
            {
                // over the right shoulder: behind the pivot along the view, to its right. The game's camera, worked out
                // this frame before us, stands closer when something is behind the player (its line of sight): no
                // further out than it
                const float pivot[3] = {px, py, pz + kAimPivotUp}, right[3] = {sy, -cy, 0.0f};
                g_gameDistance = std::sqrt((gs[0] - pivot[0]) * (gs[0] - pivot[0]) + (gs[1] - pivot[1]) * (gs[1] - pivot[1]) +
                                           (gs[2] - pivot[2]) * (gs[2] - pivot[2]));
                const float back = std::clamp(g_gameDistance - 0.2f, kAimMinBack, kAimBack), k = back / kAimBack;
                float aim[3];
                for (int i = 0; i < 3; ++i)
                    aim[i] = pivot[i] - front[i] * back + right[i] * kAimSide * k;
                aim[2] += kAimUp * k;
                // eased from the game's camera (smoothstep)
                const float t = g_aimBlend * g_aimBlend * (3.0f - 2.0f * g_aimBlend);
                float len = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    src[i] = gs[i] + (aim[i] - gs[i]) * t;
                    dir[i] = gf[i] + (front[i] - gf[i]) * t;
                    len += dir[i] * dir[i];
                }
                len = std::sqrt(len);
                for (float &v : dir)
                    v = len > 1e-4f ? v / len : 0.0f;
                if (len <= 1e-4f)
                    std::copy(front, front + 3, dir);
                fov = (gameFov >= 10.0f && gameFov <= 130.0f ? gameFov : 70.0f) * (1.0f - t) + freeaim::fov() * t;
            }
            // no roll: up from the view's own pitch
            const float h = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
            float up[3] = {h > 1e-4f ? -dir[0] / h * dir[2] : 0.0f, h > 1e-4f ? -dir[1] / h * dir[2] : 0.0f, h};
            if (first && g_ragdollBlend > 0.0f) // a ragdoll's view rolls with its head
            {
                float l = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    up[i] += (g_ragdollUp[i] - up[i]) * g_ragdollBlend;
                    l += up[i] * up[i];
                }
                l = std::sqrt(l);
                for (float &v : up)
                    v = l > 1e-4f ? v / l : 0.0f;
                if (l <= 1e-4f)
                    up[2] = 1.0f;
            }
            if (head && headRoll != 0.0f) // the head's roll (VR): up tilted toward the view's right
            {
                const float right[3] = {sy, -cy, 0.0f}, cr = std::cos(headRoll), sr = std::sin(headRoll);
                for (int i = 0; i < 3; ++i)
                    up[i] = up[i] * cr + right[i] * sr;
            }
            wrVec(ram, cam + kCamSource, src[0], src[1], src[2]);
            wrVec(ram, cam + kCamFront, dir[0], dir[1], dir[2]);
            wrVec(ram, cam + kCamUp, up[0], up[1], up[2]);
            wrVec(ram, cam + kCamTarget, src[0] + dir[0], src[1] + dir[1], src[2] + dir[2]);
            wrf(ram, cam + kCamFov, fov);

            if (turning)
            {
                // the follow camera stands behind the view (heading beta = the view's + pi, its stand points turned
                // with it about the player), so the player walks where he looks and the third-person view takes over
                // from there
                const float beta = std::fmod(g_yaw + kPi + kTwoPi, kTwoPi);
                const float turn = wrap(beta - rdf(ram, cam + kCamBeta));
                wrf(ram, cam + kCamBeta, beta);
                const float c = std::cos(turn), s = std::sin(turn);
                for (uint32_t at : {cam + kCamStandA, cam + kCamStandB})
                {
                    const float ox = rdf(ram, at) - px, oy = rdf(ram, at + 4) - py;
                    wrf(ram, at, px + ox * c - oy * s);
                    wrf(ram, at + 4, py + ox * s + oy * c);
                }

                // the body faces the view standing still (idle) and attacking (states 0x10 / 0x11, or Circle / R1 held):
                // shots go where the player looks, and R1's target search - in front of the body, in the frame R1 is
                // read, before the camera - finds the one in view. Moving, the game turns him where he goes (relative
                // to the view), aiming too (his gun still points along the aim). Locked on (mode 11) the body is the
                // game's.
                const mod::PadState pad = mod::lastPad();
                const uint32_t state = rd32(ram, ped + kPedState);
                const bool idle = state == kPedStateIdle, attacking = state == kPedStateAttack || state == kPedStateFight;
                if (idle || attacking)
                {
                    const float want = wrap(g_yaw - kPi / 2.0f); // heading of a ped facing the view
                    const bool moving = std::fabs(pad.lx) > 0.1f || std::fabs(pad.ly) > 0.1f;
                    if (attacking || !moving || (!aiming && (pad.buttons & (kCircle | kR1))))
                    {
                        wrf(ram, ped + kPedRotationCur, want);
                        wrf(ram, ped + kPedRotationDest, want);
                    }
                }
            }

            if (first)
            {
                shortNear(ram); // the camera is inside the head, the arms and the gun are close
                mirrorToOtherCams(ram, cam);
            }
            else
                restoreNear(ram);
            freeaim::frame(ram, ped, aiming, first ? freeaim::View::FirstPerson : freeaim::View::Aim, src, dir, viewYaw, viewPitch);
            viewmodel::frame(ram, ped, first, mode == kModeLockOn, aiming, src, dir, viewYaw, viewPitch); // the arms in view
            publish();
            return true;
        }

        // CCam::ProcessSpecialHeightRoutines(cam a0), right after the active CCam's update (as the free camera's, which
        // runs first when it flies).
        VCS_GUEST_CC void camHeight(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            const uint32_t cam = mod::gpr32(ctx, 4);
            if (cam != kCams + rdram[kActiveCam] * kCamSize)
                return g_origHeight(rdram, ctx, runtime);
            reload::update(rdram); // the reload key, on the game thread (reload.h)
            saanims::update(rdram); // SA's armed holds on VCS's anims (saanims.h)
            if (mod::freecamFlying())
            {
                endDriveBy(rdram); // the free camera's (the head shown, the game's near plane, the windows)
                g_vehicle = 0;
                carglass::hide(0);
                freeaim::frame(rdram, rd32(rdram, kPlayerPed), false, freeaim::View::Game, nullptr, nullptr, 0.0f, 0.0f);
                return g_origHeight(rdram, ctx, runtime);
            }
            const bool ours = update(rdram, cam);
            viewmodel::animate(rdram, ctx, runtime); // the aim raise's anim (viewmodel.h)
            if (!ours)
                return g_origHeight(rdram, ctx, runtime);
            // first person's view noted for setFov: switching modes (on foot <-> vehicle) CCamera blends the old camera
            // into the new one for about half a second from its own copy of the old - the game's third-person camera -
            // and the blend went through the car's roof and door (dark frames getting in and out)
            if (g_hideHead)
            {
                for (uint32_t k = 0; k < 3; ++k)
                {
                    g_fpView[0][k] = rdf(rdram, cam + kCamSource + 4u * k);
                    g_fpView[1][k] = rdf(rdram, cam + kCamFront + 4u * k);
                    g_fpView[2][k] = rdf(rdram, cam + kCamUp + 4u * k);
                }
                g_fpViewFrame = rd32(rdram, kFrameCounter);
            }
            mod::setPc(ctx, mod::gpr32(ctx, 31));
        }

        // The anim tree's bone matrices: from the ped's render (FUN_003c3250, s0 = the ped), the player's tree noted.
        VCS_GUEST_CC void treeUpdate(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            if (mod::gpr32(ctx, 31) == kTreeUpdateInPedRender && mod::gpr32(ctx, 16) && mod::gpr32(ctx, 16) == rd32(rdram, kPlayerPed))
            {
                g_tree = mod::gpr32(ctx, 4);
                g_dbgTreeFrame = rd32(rdram, kFrameCounter);
            }
            return g_origTreeUpdate(rdram, ctx, runtime);
        }

        // Called by the ped's render right after its bones are built and before it is drawn: for the player the eyes'
        // place is noted, and in first person the head's two bones are collapsed to the head joint (nothing of the head
        // left to see from inside it).
        VCS_GUEST_CC void afterBones(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            const uint32_t ped = mod::gpr32(ctx, 4);
            if (mod::gpr32(ctx, 31) == kAfterBonesInPedRender && ped && ped == rd32(rdram, kPlayerPed))
                g_dbgAfterFrame = rd32(rdram, kFrameCounter);
            if (mod::gpr32(ctx, 31) == kAfterBonesInPedRender && ped && ped == rd32(rdram, kPlayerPed) && g_tree &&
                rd32(rdram, g_tree + kTreeNodes) == kPedBones)
            {
                const uint32_t bones = rd32(rdram, g_tree + kTreeMatrices);
                const uint32_t head = bones + kHeadBone * 0x40u;
                const float px = rdf(rdram, ped + kPedPosition), py = rdf(rdram, ped + kPedPosition + 4), pz = rdf(rdram, ped + kPedPosition + 8);
                const float hx = rdf(rdram, head + 0x30), hy = rdf(rdram, head + 0x34), hz = rdf(rdram, head + 0x38);
                const float dx = hx - px, dy = hy - py, dz = hz - pz;
                // the tree is this ped's: its head within 2 m of his position - or, while he has a vehicle, 3 m of the
                // vehicle's (getting in and out his position lags the anim by metres: the head was left drawn and the
                // eyes stale for those frames)
                bool ours = dx * dx + dy * dy + dz * dz < 4.0f;
                if (const uint32_t v = rd32(rdram, ped + kPedVehicle); !ours && v)
                {
                    const float vx = hx - rdf(rdram, v + 0x30), vy = hy - rdf(rdram, v + 0x34), vz = hz - rdf(rdram, v + 0x38);
                    ours = vx * vx + vy * vy + vz * vz < 9.0f;
                }
                g_dbgOurs = ours ? 1u : 0u;
                if (bones && ours)
                {
                    g_dbgDoneFrame = rd32(rdram, kFrameCounter);
                    const float fx = rdf(rdram, ped + kPedForward), fy = rdf(rdram, ped + kPedForward + 4);
                    g_eye[0] = dx + fx * kEyeForward;
                    g_eye[1] = dy + fy * kEyeForward;
                    g_eye[2] = dz + kEyeUp;
                    // the eyes along the head's own axes (x up the head, y forward): getting on or off a vehicle he leans
                    // and ducks, and world up / the view's ahead from the joint put them in his chest
                    for (int i = 0; i < 3; ++i)
                        g_eyeHead[i] = rdf(rdram, head + 4u * i) * kEyeUp + rdf(rdram, head + 0x10u + 4u * i) * kEyeForward;
                    g_headJoint[0] = hx, g_headJoint[1] = hy, g_headJoint[2] = hz;
                    for (int i = 0; i < 3; ++i)
                    {
                        g_headUp[i] = rdf(rdram, head + 4u * i);
                        g_headAhead[i] = rdf(rdram, head + 0x10u + 4u * i);
                    }
                    g_eyeFrame = rd32(rdram, kFrameCounter);
                    g_eyeValid = true;
                    // in a vehicle: the head joint in its space (its matrix rows are orthonormal)
                    if (const uint32_t veh = rd32(rdram, ped + kPedVehicle))
                    {
                        const uint32_t m = vehicleMatrix(rdram, veh);
                        const float w[3] = {hx - rdf(rdram, m + 0x30), hy - rdf(rdram, m + 0x34), hz - rdf(rdram, m + 0x38)};
                        float l[3];
                        for (int row = 0; row < 3; ++row)
                            l[row] = w[0] * rdf(rdram, m + row * 0x10u) + w[1] * rdf(rdram, m + row * 0x10u + 4) +
                                     w[2] * rdf(rdram, m + row * 0x10u + 8);
                        if (l[0] * l[0] + l[1] * l[1] + l[2] * l[2] < 9.0f)
                        {
                            std::copy(l, l + 3, g_headLocal);
                            g_headVehicle = veh;
                            g_headFrame = g_eyeFrame;
                        }
                    }
                    // getting in or out of a vehicle the head moves fast (he ducks onto the seat) and the camera, worked out
                    // before this draw, used the last one's: the drawn body is moved by the difference so the head is
                    // at the camera this frame (no gap between the view and the neck)
                    bool entering = false;
                    if (g_hideHead && rd32(rdram, ped + kPedVehicle) && g_camHeadFrame && rd32(rdram, kFrameCounter) - g_camHeadFrame <= 1u)
                    {
                        const float d[3] = {g_camHead[0] - hx, g_camHead[1] - hy, g_camHead[2] - hz};
                        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < kBodyFollowMax * kBodyFollowMax)
                        {
                            for (uint32_t b = 0; b < kPedBones; ++b)
                                for (int i = 0; i < 3; ++i)
                                {
                                    const uint32_t a = bones + b * 0x40u + 0x30u + 4u * i;
                                    wrf(rdram, a, rdf(rdram, a) + d[i]);
                                }
                            entering = true;
                        }
                    }
                    // VR: the body's bones collapsed (scale 0 at their joints) except what camera.vr_body keeps
                    const bool vr = g_hideHead && headLookApplied();
                    const int vrBody = g_vrBody.load(std::memory_order_relaxed);
                    if (!vr || vrBody == 1)
                        viewmodel::apply(rdram, ped, bones); // first person: the arms and gun in view (viewmodel.h)
                    if (vr && vrBody != 2)
                        for (uint32_t b = 0; b < kPedBones; ++b)
                            if (vrBody == 0 || b < 7u || b > 16u) // 7..16: the clavicles, arms and hands (the gun's)
                                for (uint32_t row = 0; row < 3; ++row)
                                    wrVec(rdram, bones + b * 0x40u + row * 0x10u, 0.0f, 0.0f, 0.0f);
                    if (vr) // the neck as well: the view looks down past it
                        for (uint32_t row = 0; row < 3; ++row)
                            wrVec(rdram, bones + kNeckBone * 0x40u + row * 0x10u, 0.0f, 0.0f, 0.0f);
                    if (g_hideHead && entering) // the neck too: the view looks down past it onto the seat
                        for (uint32_t row = 0; row < 3; ++row)
                            wrVec(rdram, bones + kNeckBone * 0x40u + row * 0x10u, 0.0f, 0.0f, 0.0f);
                    if (g_hideHead)
                        for (uint32_t b : {kHeadBone, kHeadEndBone})
                            for (uint32_t row = 0; row < 3; ++row)
                                wrVec(rdram, bones + b * 0x40u + row * 0x10u, 0.0f, 0.0f, 0.0f);
                }
            }
            return g_origAfterBones(rdram, ctx, runtime);
        }

        // The frame's field of view, from CCamera::Process: the game's own on-foot and vehicle cameras scaled by their
        // settings (70 = the game's), ours (first person, the aim, the free camera) and every other camera left alone.
        VCS_GUEST_CC void setFov(uint8_t *rdram, VcsGuestContext *ctx, void *runtime)
        {
            // first person: the frame's camera is its view, whatever CCamera::Process blended or shook (TheCamera's matrix:
            // rows up x forward, forward, up, position; the RW camera is set from it after this)
            if (mod::gpr32(ctx, 31) == kSetFovInCameraProcess && g_on && g_hideHead && !mod::freecamFlying() &&
                g_fpViewFrame == rd32(rdram, kFrameCounter))
            {
                const float *f = g_fpView[1], *u = g_fpView[2];
                const float l[3] = {u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]};
                wrVec(rdram, kTheCamera + 0x00u, l[0], l[1], l[2]);
                wrVec(rdram, kTheCamera + 0x10u, f[0], f[1], f[2]);
                wrVec(rdram, kTheCamera + 0x20u, u[0], u[1], u[2]);
                wrVec(rdram, kTheCamera + 0x30u, g_fpView[0][0], g_fpView[0][1], g_fpView[0][2]);
            }
            // VR: the player walks by the body's heading (mouse / stick), not the head's. CCamera::Process has just set
            // the heading the pad is read against (+0x7E8, FUN_0037b2c8: atan2 of TheCamera's front x, y) from the
            // view the head turned
            if (mod::gpr32(ctx, 31) == kSetFovInCameraProcess && g_on && g_hideHead && !mod::freecamFlying() &&
                g_headAppliedFrame.load(std::memory_order_relaxed) == rd32(rdram, kFrameCounter) + 1u)
            {
                const float fx = rdf(rdram, kTheCamera + 0x10u), fy = rdf(rdram, kTheCamera + 0x14u),
                            o = rdf(rdram, kTheCamera + kCamOrientation);
                // which way round the game's atan2 is: the heading against both (it may be turned by pi, looking back)
                const auto off = [o](float a) { return std::min(std::fabs(wrap(o - a)), std::fabs(wrap(o - a - kPi))); };
                const bool xy = off(std::atan2(fx, fy)) <= off(std::atan2(fy, fx)); // heading = pi/2 - yaw, else = yaw
                wrf(rdram, kTheCamera + kCamOrientation, wrap(o + (xy ? g_walkHeadYaw : -g_walkHeadYaw)));
            }
            if (mod::gpr32(ctx, 31) == kSetFovInCameraProcess && !g_on && !mod::freecamFlying())
            {
                const uint32_t mode = rd32(rdram, kCams + rdram[kActiveCam] * kCamSize + kCamMode) & 0xFFFFu;
                const int want = mode == kModeFollowPed ? g_fovFoot.load(std::memory_order_relaxed)
                                 : mode == kModeCar || mode == kModeBoat ? g_fovVehicle.load(std::memory_order_relaxed)
                                                                         : kGameFov;
                if (want != kGameFov)
                    mod::fpr(ctx)[12] *= static_cast<float>(want) / kGameFov;
            }
            return g_origSetFov(rdram, ctx, runtime);
        }
    }

    void registerSettings()
    {
        using namespace mod;
        Spec s;
        s.key = "camera.first_person";
        s.page = "Game";
        s.section = "First person";
        s.label = "First person";
        s.help = "The camera at the player's eyes, his head hidden; the mouse looks around. In vehicles too unless switched "
                 "off below; cutscenes and the sniper / rocket views keep the game's camera. The key (Keyboard page, B) "
                 "switches it.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_FIRSTPERSON";
        s.onChange = [](int v) { g_enabled = v != 0; };
        s.read = [] { return g_enabled ? 1 : 0; };
        add(s);

        s = {};
        s.key = "camera.first_person_fov";
        s.page = "Game";
        s.section = "First person";
        s.label = "First person field of view";
        s.help = "How wide the first-person view is (the game's own camera: 70).";
        s.type = Type::Integer;
        s.min = 60;
        s.max = 110;
        s.format = "%d deg";
        s.defaultValue = 90;
        s.onChange = [](int v) { g_fov = v; };
        add(s);

        s = {};
        s.key = "camera.first_person_near";
        s.page = "Game";
        s.section = "First person";
        s.label = "First person near clip";
        s.help = "How close to the eyes things still show (10 cm by default; the game's own camera 30). Shorter shows more "
                 "of the arms and the gun; too short can make far surfaces flicker.";
        s.type = Type::Integer;
        s.min = 2;
        s.max = 30;
        s.format = "%d cm";
        s.defaultValue = kNearCm;
        s.onChange = [](int v) { g_nearCm = v; };
        add(s);

        s = {};
        s.key = "camera.vr_body";
        s.page = "Game";
        s.section = "First person";
        s.label = "In VR: your body";
        s.help = "What of the player you see in a VR headset (VR.asi): nothing (no arms, neck or gun in the way when you look "
                 "around or down), the arms and gun only, or the whole body but the head.";
        s.type = Type::Choice;
        s.choices = {{"Hidden", 0}, {"Arms and gun", 1}, {"Body (not the head)", 2}};
        s.defaultValue = 0;
        s.onChange = [](int v) { g_vrBody = v; };
        add(s);
        static const char *const kEyeKeys[3] = {"camera.first_person_eye_right", "camera.first_person_eye_ahead",
                                                "camera.first_person_eye_up"};
        static const char *const kEyeLabels[3] = {"Eye position: right", "Eye position: ahead", "Eye position: up"};
        for (int axis = 0; axis < 3; ++axis)
        {
            s = {};
            s.key = kEyeKeys[axis];
            s.page = "Game";
            s.section = "First person";
            s.label = kEyeLabels[axis];
            s.help = "Moves the first-person eyes from the head (on foot along the player's body, in a vehicle along it); "
                     "0 = the eyes.";
            s.type = Type::Integer;
            s.min = -50;
            s.max = 50;
            s.format = "%d cm";
            s.defaultValue = 0;
            s.onChange = [axis](int v) { g_eyeCm[axis] = v; };
            add(s);
        }

        viewmodel::registerSettings(); // the arms and gun in view
        saanims::registerSettings();   // SA's armed holds
        reload::registerSettings();    // manual reload (its own section) and its key

        s = {};
        s.key = "camera.first_person_vehicle";
        s.page = "Game";
        s.section = "First person";
        s.label = "First person in vehicles";
        s.help = "With first person on, also in cars, bikes, boats and aircraft: the view from the seat, the mouse looks "
                 "around and it eases back ahead while driving; L2 / R2 still look aside.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.onChange = [](int v) { g_vehicles = v != 0; };
        add(s);

        for (const bool vehicle : {false, true})
        {
            s = {};
            s.key = vehicle ? "camera.fov_vehicle" : "camera.fov_foot";
            s.page = "Game";
            s.section = "Field of view";
            s.label = vehicle ? "Vehicles" : "On foot";
            s.help = vehicle ? "The game's camera behind cars, bikes, boats and aircraft (70 as on the PS2; its widening "
                               "at speed is kept on top)."
                             : "The game's camera behind the player on foot (70 as on the PS2).";
            s.type = Type::Integer;
            s.min = 50;
            s.max = 110;
            s.format = "%d deg";
            s.defaultValue = kGameFov;
            s.onChange = [vehicle](int v) { (vehicle ? g_fovVehicle : g_fovFoot) = v; };
            add(s);
        }
    }

    void install()
    {
        if (const char *e = std::getenv("VCS_FIRSTPERSON_HOOKS"); e && e[0] == '0')
            return;
        g_origHeight = mod::api->lookup_function(kCamHeightRoutines); // the free camera's, when it is installed
        g_origTreeUpdate = mod::api->lookup_function(kTreeUpdate);
        g_origAfterBones = mod::api->lookup_function(kAfterBones);
        g_origSetFov = mod::api->lookup_function(kSetFov);
        if (!g_origHeight || !g_origTreeUpdate || !g_origAfterBones || !g_origSetFov)
            return;
        mod::api->replace_function(kCamHeightRoutines, &camHeight);
        mod::api->replace_function(kTreeUpdate, &treeUpdate);
        mod::api->replace_function(kAfterBones, &afterBones);
        mod::api->replace_function(kSetFov, &setFov);
        g_installed = true;
        viewmodel::install();
        viewmodel::installAnims(); // the aim raise's calls into the game
        saanims::install();
        // firstperson -> ok on= looking= yaw= pitch= tree= eye= (tests: tools/rig.py firstperson)
        mod::addCommand("firstperson", [](const std::string &, mod::Reply reply) {
            Status st;
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                st = g_status;
            }
            char text[420];
            std::snprintf(text, sizeof(text),
                          "ok enabled=%d on=%d looking=%d yaw=%.4f pitch=%.4f tree=%08x eye=%.3f,%.3f,%.3f aim=%.2f gamedist=%.2f "
                          "drive=%.2f headveh=%.3f,%.3f,%.3f dbg=%u,%u,%u,%u,%u",
                          g_enabled ? 1 : 0, st.on ? 1 : 0, g_looking ? 1 : 0, st.yaw, st.pitch, st.tree, st.eye[0],
                          st.eye[1], st.eye[2], st.aim, st.gameDistance, st.drive, g_headLocal[0], g_headLocal[1],
                          g_headLocal[2], rd32(mod::api->rdram(), kFrameCounter), g_dbgTreeFrame, g_dbgAfterFrame, g_dbgDoneFrame, g_dbgOurs);
            reply(text);
        });
    }

    void toggle()
    {
        const int next = g_enabled ? 0 : 1;
        if (!mod::set("camera.first_person", next))
            g_enabled = next != 0; // overridden by VCS_FIRSTPERSON: for this run only
        mod::log("[firstperson] %s\n", next ? "switched on" : "switched off");
    }

    bool looking() { return g_installed.load(std::memory_order_relaxed) && g_looking.load(std::memory_order_relaxed); }

    void look(float yaw, float pitch)
    {
        std::lock_guard<std::mutex> lock(g_lookMutex);
        g_lookYaw += yaw;
        g_lookPitch += pitch;
    }

    void setHeadPosition(float right, float ahead, float up)
    {
        std::lock_guard<std::mutex> lock(g_headMutex);
        const float v[3] = {right, ahead, up};
        for (int i = 0; i < 3; ++i)
            g_headPos[i] = std::isfinite(v[i]) ? std::clamp(v[i], -kVrHeadReach, kVrHeadReach) : 0.0f;
    }

    void setHeadLook(bool on, float yaw, float pitch, float roll)
    {
        std::lock_guard<std::mutex> lock(g_headMutex);
        g_headOn = on;
        if (!on)
            g_headPos[0] = g_headPos[1] = g_headPos[2] = 0.0f;
        g_headYaw = std::isfinite(yaw) ? yaw : 0.0f;
        g_headPitch = std::isfinite(pitch) ? pitch : 0.0f;
        g_headRoll = std::isfinite(roll) ? roll : 0.0f;
    }

    bool headLookApplied()
    {
        // in one of the last few game frames (read on another thread: no flicker while an update runs)
        const uint32_t at = g_headAppliedFrame.load(std::memory_order_relaxed);
        const uint8_t *ram = mod::api->rdram();
        return g_installed.load(std::memory_order_relaxed) && at && ram && rd32(ram, kFrameCounter) + 1u - at <= 3u;
    }
}
