#define _GNU_SOURCE
#include <link.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unistd.h>

#include "OvrDirectModeComponent.h"
#include "SteamVrSync.h"
#include "depth/RuntimeClockProbe.hpp"

#include "alvr_server/Logger.h"
#include "alvr_server/driverlog.h"
#include "alvr_server/include/openvr_math.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>

// Multiples of the frame interval. A job older than this when the worker picks
// it up is from a stall, and the swap texture sets are only three deep, so the
// slot is close to its next writer.
constexpr double JobAgeLimitFrames = 1.5;

// Consecutive writer-fence poll timeouts before the GPU wait disarms for the
// session, so a permanently wedged writer degrades the stream once instead of
// taxing every frame.
constexpr uint32_t GpuWaitDisarmThreshold = 30;

// Cap on the writer-fence poll, in frame intervals. The render submission
// waits on these fences unboundedly while holding the encoder mutex, which
// Present's rebuild paths also take, so an unsignalled fence must be caught
// here or it wedges SteamVR's frame loop through the worker built to keep it
// free.
constexpr int64_t GpuWaitPollCapFrames = 2;

// Guard sleep before render, ms, only for a frame with a texture that has no
// SteamVR timeline semaphore (see SteamVrSync.h). The compositor's own scene
// writes carry only detached pre-signaled placeholder fences, and on NVIDIA
// the dma-bufs carry none at all, so the fence wait cannot see them; this
// bounds how fresh such a write can be when the read starts. Runs on the
// worker, so it costs pipeline depth, not compositor-thread budget.
constexpr int GuardSleepMs = 4;

// Gap filling waits this long past a display slot for the application's
// Present before synthesizing the slot's picture.
constexpr std::chrono::milliseconds GapFillSlack { 1 };
// How long past its slot a failed fill is retried.
constexpr std::chrono::milliseconds GapFillRetryWindow { 5 };

// For trials: 0 in ~/.config/alvr/encoder_rewarp sends each frame as it was
// rendered, stamped with the pose it was rendered for, and leaves all
// reprojection to the headset. Read once.
static bool ReadRewarpEnabled() {
    int value = 1;
    if (const char* home = getenv("HOME")) {
        std::string path = std::string(home) + "/.config/alvr/encoder_rewarp";
        if (FILE* file = fopen(path.c_str(), "r")) {
            int read = 1;
            if (fscanf(file, "%d", &read) == 1 && (read == 0 || read == 1)) {
                value = read;
            }
            fclose(file);
        }
    }
    return value != 0;
}
static const bool RewarpEnabled = ReadRewarpEnabled();

// Cap on the wait for a texture's timeline semaphore, in frame intervals.
// On timeout the frame is read anyway, as it would have been without it.
constexpr int64_t TimelineWaitCapFrames = 2;

// The negotiated refresh rate expressed as a frame period.
static std::chrono::nanoseconds frameInterval() {
    return std::chrono::nanoseconds((int64_t)(1e9 / Settings_Instance()->m_displayRefreshRate));
}

// Export the sync file a reader must wait on (the writer's fences) from a
// dma-buf fd. Returns the sync_file fd, or -1 when there is nothing to wait
// on or the kernel lacks the ioctl. The caller owns the returned fd.
static int exportReadSyncFile(int dmabufFd) {
#if defined(DMA_BUF_IOCTL_EXPORT_SYNC_FILE)
    struct dma_buf_export_sync_file exportInfo = {};
    exportInfo.flags = DMA_BUF_SYNC_READ;
    exportInfo.fd = -1;
    if (ioctl(dmabufFd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exportInfo) != 0) {
        return -1;
    }
    return exportInfo.fd;
#else
    (void)dmabufFd;
    return -1;
#endif
}

// Close whichever of a job's fence fds are open. Every path that drops a
// job without presenting it owes this call.
template <typename Waits> static void closeFenceFds(Waits& waits) {
    for (auto& wait : waits) {
        if (wait.fenceFd >= 0) {
            close(wait.fenceFd);
            wait.fenceFd = -1;
        }
    }
}

// Loose on purpose: it rejects zeroed and uninitialized poses, which miss by
// orders of magnitude, not accumulated rounding drift.
constexpr double UnitQuatNormSqTolerance = 0.1;

static bool IsUnitQuat(double w, double x, double y, double z) {
    double normSq = w * w + x * x + y * y + z * z;
    return normSq > 1.0 - UnitQuatNormSqTolerance && normSq < 1.0 + UnitQuatNormSqTolerance;
}

// Fill the warp push constants for a rotational reprojection from
// renderOrientation to latestOrientation. The shaders sample backwards, so the
// rotation they need is source-from-destination, R_render^T * R_latest; with
// the Hamilton product convention of openvr_math.h, conj(q_render) * q_latest
// composes the same way. Both quaternions are world-from-head, so the world
// frame cancels and the playspace transform never enters. Returns false, with
// params left disabled, on a degenerate FOV or a non-unit orientation.
static bool BuildWarpParams(
    const vr::HmdQuaternion_t& renderOrientation,
    const FfiQuat& latestOrientation,
    const FfiFov& leftFov,
    const FfiFov& rightFov,
    alvr::render::WarpParams& params
) {
    float leftTans[4]
        = { tanf(leftFov.left), tanf(leftFov.right), tanf(leftFov.up), tanf(leftFov.down) };
    float rightTans[4]
        = { tanf(rightFov.left), tanf(rightFov.right), tanf(rightFov.up), tanf(rightFov.down) };
    if (leftTans[1] - leftTans[0] == 0.f || rightTans[1] - rightTans[0] == 0.f
        || leftTans[2] - leftTans[3] == 0.f || rightTans[2] - rightTans[3] == 0.f) {
        return false;
    }
    if (!IsUnitQuat(
            renderOrientation.w, renderOrientation.x, renderOrientation.y, renderOrientation.z
        )
        || !IsUnitQuat(
            latestOrientation.w, latestOrientation.x, latestOrientation.y, latestOrientation.z
        )) {
        return false;
    }

    vr::HmdQuaternion_t qLatest
        = { latestOrientation.w, latestOrientation.x, latestOrientation.y, latestOrientation.z };
    vr::HmdQuaternion_t qDelta = vrmath::quaternionConjugate(renderOrientation) * qLatest;

    vr::HmdMatrix34_t rot;
    HmdMatrix_QuatToMat(qDelta.w, qDelta.x, qDelta.y, qDelta.z, &rot);

    // GLSL mat4 is column-major: rotation[col * 4 + row].
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            params.rotation[col * 4 + row]
                = (row < 3 && col < 3) ? rot.m[row][col] : (float)(row == col);
        }
    }
    memcpy(params.leftTans, leftTans, sizeof(leftTans));
    memcpy(params.rightTans, rightTans, sizeof(rightTans));
    params.enabled = 1;
    return true;
}

// Same source-from-destination convention as the real-frame rewarp.
// sourcePosition/destinationPosition (metres, tracking space) add the head
// translation, expressed in the source head frame: a point x in the
// destination frame is delta*x + translation in the source frame.
static bool BuildMotionRotation(const vr::HmdQuaternion_t& source,
    const FfiQuat& destination, const FfiViewParams (&views)[2],
    alvr::motion::NativeMotion::Rotation& result,
    const float* sourcePosition = nullptr, const float* destinationPosition = nullptr) {
    alvr::render::WarpParams validated {};
    if (!BuildWarpParams(source,destination,views[0].fov,views[1].fov,validated)) return false;
    vr::HmdQuaternion_t dest {destination.w,destination.x,destination.y,destination.z};
    auto delta=vrmath::quaternionConjugate(source)*dest;
    result.quaternion[0]=delta.x; result.quaternion[1]=delta.y;
    result.quaternion[2]=delta.z; result.quaternion[3]=delta.w;
    memcpy(result.leftTans,validated.leftTans,sizeof(result.leftTans));
    memcpy(result.rightTans,validated.rightTans,sizeof(result.rightTans));
    if (sourcePosition && destinationPosition) {
        vr::HmdQuaternion_t moved {0, destinationPosition[0]-sourcePosition[0],
            destinationPosition[1]-sourcePosition[1], destinationPosition[2]-sourcePosition[2]};
        auto local=vrmath::quaternionConjugate(source)*moved*source;
        result.translation[0]=float(local.x); result.translation[1]=float(local.y);
        result.translation[2]=float(local.z);
    }
    return true;
}

void OvrDirectModeComponent::SetViewParams(const FfiViewParams params[2]) {
    std::unique_lock<std::mutex> lock(m_viewParamsMutex);
    m_viewParams[0] = params[0];
    m_viewParams[1] = params[1];
    m_viewParamsValid = true;
}

OvrDirectModeComponent::OvrDirectModeComponent(std::shared_ptr<PoseHistory> poseHistory)
    : m_poseHistory(poseHistory)
    , m_submitLayer(0) {
    m_encodeWorker = std::thread(&OvrDirectModeComponent::EncodeWorkerLoop, this);
    StartVsyncAnnouncer();
}

OvrDirectModeComponent::~OvrDirectModeComponent() {
    // Stops before the encode worker: the announcer's grid is what
    // PostPresent holds against, and a hold outliving the worker leaves a
    // present waiting on a frame nothing will encode.
    StopVsyncAnnouncer();
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_workerExit = true;
        m_jobPending = false;
    }
    m_jobCv.notify_one();
    if (m_encodeWorker.joinable()) {
        m_encodeWorker.join();
    }
    // The worker is gone; whatever job was pending will never present.
    closeFenceFds(m_job.waits);
}

void OvrDirectModeComponent::DrainPendingJob() {
    uint64_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        if (m_jobPending) {
            closeFenceFds(m_job.waits);
            dropped = m_job.frame;
        }
        m_jobPending = false;
    }
    if (dropped != 0) {
        MarkRead(dropped);
    }
}

void OvrDirectModeComponent::MarkRead(uint64_t frame) {
    {
        std::lock_guard<std::mutex> lock(m_readMutex);
        m_lastReadFrame = std::max(m_lastReadFrame, frame);
    }
    m_readCv.notify_all();
}

void OvrDirectModeComponent::EncodeWorkerLoop() {
    // Gap filling (diag_native_motion=4): when no Present arrives for a
    // display slot, send a synthesized picture instead of leaving the slot
    // empty. Read once; off by default.
    bool const gapFill = alvr::motion::nativeMotionMode() >= 4;
    std::chrono::steady_clock::time_point nextSlot {}, lastRealOutput {};
    uint64_t lastStamp = 0, lastRealStamp = 0, slotIndex = 0, poseWaits = 0;
    uint64_t filled = 0, attempts = 0, noPose = 0, notNewer = 0, notReady = 0, rejected = 0;
    uint64_t jobsSeen = 0, timeouts = 0, retries = 0, presentsShort = 0, presentsLong = 0;
    uint64_t jobsWithOverlays = 0, overlaySkips = 0;
    // Overlays (the SteamVR dashboard, other layers over the scene) are
    // composited before synthesis and would be extrapolated with the scene,
    // leaving trails; while the newest real picture has any, slots are left
    // to the headset's own reprojection.
    bool newestHasOverlays = false;
    std::chrono::steady_clock::time_point retryAt {};
    auto nextReport = std::chrono::steady_clock::now();
    // Synthesizes and sends the picture for the current slot, stamped with
    // the newest tracking pose: a real picture already carries (nearly) the
    // newest pose when presented, so this slot's pose has only just arrived,
    // and the prediction extrapolates the image to exactly that pose time.
    bool const separateRotation = alvr::motion::nativeMotionMode() >= 5;
    // Mode 9 interpolates: when the application skipped a slot, the picture
    // between the previous and the newest real picture is sent first and the
    // newest real picture is held back one slot.
    bool const interpolate = alvr::motion::nativeMotionMode() == 9;
    bool heldPending = false;
    uint64_t heldStamp = 0, interpolated = 0, sentDirect = 0;
    std::chrono::steady_clock::time_point heldSendAt {};
    vr::HmdQuaternion_t lastMotionOrientation {1,0,0,0};
    std::array<float, 3> lastMotionPosition {};
    bool motionPoseValid = false, motionGenerationValid = false;
    uint64_t motionGeneration = 0;
    auto tryFill = [&](bool lastChance) -> bool {
        ++attempts;
        // Aim at the slot's exact time after the real picture, so pictures
        // stay evenly spaced; the tracking pose for that time may still be on
        // its way, then wait for it (retry) unless this is the last chance.
        auto const intervalNs
            = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(frameInterval())
                  .count();
        uint64_t const wanted = lastRealStamp + slotIndex * intervalNs;
        auto pose = m_poseHistory->GetPoseNearest(wanted);
        if (!pose) {
            ++noPose;
            return false;
        }
        if (!lastChance && pose->targetTimestampNs + intervalNs / 6 < wanted) {
            ++poseWaits;
            return false;
        }
        if (pose->targetTimestampNs <= lastStamp) {
            ++notNewer;
            return false;
        }
        if (m_encoderState.load() != EncoderState::Streaming) {
            ++notReady;
            return false;
        }
        if (newestHasOverlays) {
            ++overlaySkips;
            return false;
        }
        std::lock_guard<std::mutex> encLock(m_encMutex);
        try {
            auto const slotClockNs = (uint64_t)std::chrono::duration_cast<
                                         std::chrono::nanoseconds>(nextSlot.time_since_epoch())
                                         .count();
            alvr::motion::NativeMotion::Rotation rotation;
            if (separateRotation) {
                FfiViewParams views[2];
                { std::unique_lock<std::mutex> lock(m_viewParamsMutex);
                  if (!m_viewParamsValid || !motionPoseValid) return false;
                  views[0]=m_viewParams[0]; views[1]=m_viewParams[1]; }
                if (!BuildMotionRotation(lastMotionOrientation,pose->motion.pose.orientation,views,rotation,
                        lastMotionPosition.data(),pose->motion.pose.position)) return false;
            }
            if (m_encoderReady && enc.fill(pose->targetTimestampNs, slotClockNs, separateRotation?&rotation:nullptr)) {
                lastStamp = pose->targetTimestampNs;
                ++filled;
                return true;
            }
            ++rejected;
        } catch (std::exception const& e) {
            Error("Gap fill: %s. Tearing down encoder and going idle.\n", e.what());
            m_encGeneration++;
            try {
                enc.shutdown();
            } catch (std::exception const& e2) {
                Error("Gap fill: teardown also failed: %s\n", e2.what());
            }
            m_encoderState = EncoderState::Idle;
        }
        return false;
    };
    while (true) {
        FrameJob job;
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            auto ready = [this] { return m_jobPending || m_workerExit; };
            if (gapFill && std::chrono::steady_clock::now() >= nextReport) {
                nextReport = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                Info(
                    "Native motion gap filling: jobs=%llu slots=%llu attempts=%llu retries=%llu "
                    "pose_waits=%llu filled=%llu no_pose=%llu not_newer=%llu not_streaming=%llu "
                    "rejected=%llu presents<20ms=%llu presents>34ms=%llu with_overlays=%llu "
                    "overlay_skips=%llu interpolated=%llu sent_direct=%llu\n",
                    (unsigned long long)jobsSeen,
                    (unsigned long long)timeouts,
                    (unsigned long long)attempts,
                    (unsigned long long)retries,
                    (unsigned long long)poseWaits,
                    (unsigned long long)filled,
                    (unsigned long long)noPose,
                    (unsigned long long)notNewer,
                    (unsigned long long)notReady,
                    (unsigned long long)rejected,
                    (unsigned long long)presentsShort,
                    (unsigned long long)presentsLong,
                    (unsigned long long)jobsWithOverlays,
                    (unsigned long long)overlaySkips,
                    (unsigned long long)interpolated,
                    (unsigned long long)sentDirect
                );
            }
            if (interpolate) {
                if (heldPending && !m_jobCv.wait_until(lock, heldSendAt, ready)) {
                    lock.unlock();
                    std::lock_guard<std::mutex> encLock(m_encMutex);
                    try {
                        if (m_encoderReady && m_encoderState.load() == EncoderState::Streaming)
                            enc.sendNewestReal(heldStamp);
                    } catch (std::exception const& e) {
                        Error("Interpolation: %s. Tearing down encoder and going idle.\n", e.what());
                        m_encGeneration++;
                        try {
                            enc.shutdown();
                        } catch (std::exception const&) {
                        }
                        m_encoderState = EncoderState::Idle;
                    }
                    heldPending = false;
                    continue;
                }
                if (!heldPending)
                    m_jobCv.wait(lock, ready);
            } else if (gapFill && nextSlot != std::chrono::steady_clock::time_point {}) {
                auto const deadline = retryAt != std::chrono::steady_clock::time_point {}
                    ? retryAt
                    : nextSlot + GapFillSlack;
                if (!m_jobCv.wait_until(lock, deadline, ready)) {
                    lock.unlock();
                    auto const now = std::chrono::steady_clock::now();
                    // No real picture for several intervals (client gone,
                    // application paused or loading): stop until one arrives.
                    if (now - lastRealOutput > frameInterval() * 4) {
                        nextSlot = {};
                        retryAt = {};
                        continue;
                    }
                    if (retryAt == std::chrono::steady_clock::time_point {}) {
                        ++timeouts;
                        ++slotIndex;
                    }
                    bool const lastChance
                        = now + std::chrono::milliseconds(1) >= nextSlot + GapFillRetryWindow;
                    bool const filledSlot = tryFill(lastChance);
                    // A slot that failed only because the newest pose or the
                    // prediction was not ready yet is retried every
                    // millisecond for a few milliseconds before it is left
                    // to the headset's own reprojection.
                    if (!filledSlot && now < nextSlot + GapFillRetryWindow) {
                        retryAt = now + std::chrono::milliseconds(1);
                        ++retries;
                    } else {
                        retryAt = {};
                        // After a pause (no client, application loading)
                        // start a new grid instead of filling every slot
                        // that passed.
                        nextSlot = now - nextSlot > frameInterval() * 2
                            ? now + frameInterval()
                            : nextSlot + frameInterval();
                    }
                    continue;
                }
            } else {
                m_jobCv.wait(lock, ready);
            }
            if (m_workerExit) {
                break;
            }
            job = m_job;
            m_jobPending = false;
            ++jobsSeen;
            jobsWithOverlays += job.layers.size() > 1;
            newestHasOverlays = job.layers.size() > 1;
        }

        auto const ageLimit = std::chrono::duration_cast<std::chrono::nanoseconds>(
            frameInterval() * JobAgeLimitFrames
        );
        if (std::chrono::steady_clock::now() - job.enqueueTime > ageLimit) {
            closeFenceFds(job.waits);
            MarkRead(job.frame);
            continue;
        }

        // Fallback for textures without a SteamVR semaphore: bounded gate on
        // the writer's implicit fences, before the encoder lock. On timeout
        // close the fds and present unguarded instead of wedging. Devices: on
        // Mesa (AMD, Intel) these fences carry the writer's work; on NVIDIA
        // the dma-bufs carry none and nothing is exported.
        bool needsGuard = false;
        std::vector<struct pollfd> pfds;
        for (auto const& wait : job.waits) {
            needsGuard = needsGuard || wait.semaphore == 0;
            if (wait.fenceFd >= 0) {
                pfds.push_back({ .fd = wait.fenceFd, .events = POLLIN, .revents = 0 });
            }
        }
        if (!pfds.empty() && !m_gpuWaitPollDisarmed) {
            auto const capNs = frameInterval() * GpuWaitPollCapFrames;
            auto const deadline = std::chrono::steady_clock::now() + capNs;
            bool allSignaled = false;
            while (!allSignaled) {
                int remainingMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                                      deadline - std::chrono::steady_clock::now()
                )
                                      .count();
                if (remainingMs < 0) {
                    break;
                }
                if (poll(pfds.data(), pfds.size(), remainingMs) <= 0) {
                    break;
                }
                allSignaled = true;
                for (auto const& pfd : pfds) {
                    if ((pfd.revents & POLLIN) == 0) {
                        allSignaled = false;
                    }
                }
            }
            if (allSignaled) {
                m_gpuWaitConsecTimeouts = 0;
            } else if (++m_gpuWaitConsecTimeouts >= GpuWaitDisarmThreshold) {
                m_gpuWaitPollDisarmed = true;
                Info(
                    "Encode worker: writer fences timed out %u frames in a row; "
                    "GPU wait disarmed for this session\n",
                    m_gpuWaitConsecTimeouts
                );
            }
        }
        closeFenceFds(job.waits);

        if (needsGuard && GuardSleepMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(GuardSleepMs));
        }

        try {
            std::lock_guard<std::mutex> encLock(m_encMutex);

            if (job.generation != m_encGeneration) {
                MarkRead(job.frame);
                continue;
            }

            if (separateRotation && (!motionGenerationValid || motionGeneration != job.generation)) {
                motionGeneration=job.generation; motionGenerationValid=true;
                lastStamp=0; lastRealStamp=0; motionPoseValid=false;
                nextSlot={}; retryAt={}; lastRealOutput={}; slotIndex=0;
            }

            // Wait for the exact end of SteamVR's write into each texture, as
            // Steam Link does (SteamVrSync.h). Most textures are already
            // finished here; on NVIDIA the waits measured reach ~4 ms in
            // SteamVR Home and ~6 ms when a heavy application's frame ends
            // late.
            auto const timelineCapNs
                = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                      frameInterval() * TimelineWaitCapFrames
                )
                      .count();
            for (auto const& wait : job.waits) {
                if (wait.semaphore == 0) {
                    continue;
                }
                uint64_t const semaphore = wait.semaphore;
                if (!enc.waitTimeline(
                        semaphore,
                        [semaphore] { return steamvrsync::Sync::Get().DupFd(semaphore); },
                        wait.value,
                        timelineCapNs
                    )) {
                    Warn(
                        "SteamVR texture semaphore %llx not reached or not importable; "
                        "reading the frame anyway\n",
                        (unsigned long long)semaphore
                    );
                }
            }

            // Rotationally reproject the frame from the pose it was rendered
            // with to the newest tracking pose, then stamp it with that pose's
            // timestamp: after the warp the newest pose is the image's pose,
            // and the client reprojects from whatever the stamp names. Moving
            // one without the other is what breaks world lock. Sampled here
            // rather than in Present so the target is as fresh as possible.
            uint64_t stampTimestampNs = job.targetTimestampNs;
            alvr::render::WarpParams warpParams {};
            auto stampOrientation=job.renderOrientation;
            if (RewarpEnabled && enc.warpCapable()) {
                auto latest = m_poseHistory->GetLatestPose();

                FfiViewParams viewParams[2];
                bool viewParamsValid;
                {
                    std::unique_lock<std::mutex> vpLock(m_viewParamsMutex);
                    viewParams[0] = m_viewParams[0];
                    viewParams[1] = m_viewParams[1];
                    viewParamsValid = m_viewParamsValid;
                }

                if (latest && viewParamsValid
                    && BuildWarpParams(
                        job.renderOrientation,
                        latest->motion.pose.orientation,
                        viewParams[0].fov,
                        viewParams[1].fov,
                        warpParams
                    )) {
                    stampTimestampNs = latest->targetTimestampNs;
                    auto q=latest->motion.pose.orientation;
                    stampOrientation={q.w,q.x,q.y,q.z};
                }
            }

            uint64_t const frame = job.frame;
            auto const presentClockNs = (uint64_t)std::chrono::duration_cast<
                                            std::chrono::nanoseconds>(
                                            job.enqueueTime.time_since_epoch()
            )
                                            .count();
            alvr::motion::NativeMotion::Rotation historyRotation;
            bool validHistoryRotation=false;
            if (separateRotation) {
                FfiViewParams views[2];
                { std::unique_lock<std::mutex> lock(m_viewParamsMutex);
                  views[0]=m_viewParams[0]; views[1]=m_viewParams[1];
                  if (m_viewParamsValid) {
                    FfiQuat destination {float(stampOrientation.x),float(stampOrientation.y),float(stampOrientation.z),float(stampOrientation.w)};
                    validHistoryRotation=BuildMotionRotation(motionPoseValid?lastMotionOrientation:stampOrientation,destination,views,historyRotation,
                        motionPoseValid?lastMotionPosition.data():job.renderPosition.data(),job.renderPosition.data());
                    if (validHistoryRotation) {
                        // Stereo depth geometry: the composed picture's tangents
                        // and the eye separation from the view poses.
                        alvr::motion::StereoParams stereo;
                        auto const* l=historyRotation.leftTans; auto const* r=historyRotation.rightTans;
                        stereo.leftTans={l[0],l[1],l[2],l[3]}; stereo.rightTans={r[0],r[1],r[2],r[3]};
                        float ipd=0;
                        for (int i=0;i<3;i++) { float d=views[1].pose.position[i]-views[0].pose.position[i]; ipd+=d*d; }
                        stereo.ipd=std::sqrt(ipd);
                        if (stereo.ipd>0.01f) enc.setStereoViews(stereo);
                    }
                  } }
            }
            if (separateRotation) {
                auto stampPose=m_poseHistory->GetPoseNearest(stampTimestampNs);
                if (!stampTimestampNs || (lastStamp && stampTimestampNs<=lastStamp)
                    || !stampPose || stampPose->targetTimestampNs!=stampTimestampNs) {
                    // Never move a stamp without moving the image. A stale
                    // real job is released; it must not rewind the stream.
                    MarkRead(frame);
                    continue;
                }
            }
            // A real picture still held from the previous Present goes first.
            if (interpolate && heldPending) {
                enc.sendNewestReal(heldStamp);
                heldPending = false;
            }
            // Depth-only bridge: reject warped/foveated/overlay pictures until
            // a depth reprojection/layout mapping exists. Snapshot the exact
            // local eye parameters used for this composed frame.
            alvr::depth::ComposedDepthPicture depthPicture;
            depthPicture.timestamp = stampTimestampNs;
            depthPicture.runtime = job.depthRuntime;
            depthPicture.head = {{job.renderPosition[0], job.renderPosition[1], job.renderPosition[2]},
                {float(job.renderOrientation.x), float(job.renderOrientation.y), float(job.renderOrientation.z), float(job.renderOrientation.w)}};
            // Validate game depth against the actual submitted picture pose,
            // not the newer pose selected by the driver's rotational matcher.
            if(job.depthRuntime.display)depthPicture.head=job.depthRuntime.head;
            depthPicture.unwarpedSingleLayer = job.layers.size() == 1 && !warpParams.enabled
                && stampTimestampNs == job.targetTimestampNs && !Settings_Instance()->m_enableFoveatedEncoding;
            {
                depthPicture.viewsValid = job.depthViewsValid;
                for (unsigned eye = 0; eye < 2; ++eye) {
                    auto const& v = job.depthViews[eye];
                    depthPicture.localEyes[eye] = {{v.pose.position[0], v.pose.position[1], v.pose.position[2]},
                        {v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w}};
                    depthPicture.fov[eye] = {v.fov.left, v.fov.right, v.fov.up, v.fov.down};
                    if (!job.layers.empty()) {
                        auto const& b = job.layers[0].eyes[eye];
                        depthPicture.sourceUv[eye] = {b.uMin, b.vMin, b.uMax, b.vMax};
                    }
                }
            }
            enc.prepareGameDepth(depthPicture);
            enc.present(
                stampTimestampNs,
                warpParams,
                job.layers,
                [this, frame] { MarkRead(frame); },
                presentClockNs, validHistoryRotation?&historyRotation:nullptr,
                !interpolate
            );
            lastMotionOrientation=stampOrientation;
            lastMotionPosition=job.renderPosition;
            motionPoseValid=validHistoryRotation;
            MarkRead(frame);
            if (interpolate) {
                // The application skipped a slot (gap of about two intervals):
                // send the picture halfway between the previous and this real
                // picture now, and this real picture one slot later.
                bool sentMiddle = false;
                auto const gap = job.enqueueTime - lastRealOutput;
                if (lastRealOutput != std::chrono::steady_clock::time_point {}
                    && gap * 2 >= frameInterval() * 3 && gap < std::chrono::milliseconds(100)
                    && lastRealStamp && lastRealStamp < stampTimestampNs && !newestHasOverlays
                    && motionPoseValid) {
                    auto const previousClockNs
                        = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                              lastRealOutput.time_since_epoch()
                        )
                              .count();
                    uint64_t const middleClockNs = previousClockNs + (presentClockNs - previousClockNs) / 2;
                    auto pose = m_poseHistory->GetPoseNearest(
                        lastRealStamp + (stampTimestampNs - lastRealStamp) / 2
                    );
                    alvr::motion::NativeMotion::Rotation rotation;
                    FfiViewParams views[2];
                    bool viewsValid;
                    {
                        std::unique_lock<std::mutex> vpLock(m_viewParamsMutex);
                        views[0] = m_viewParams[0];
                        views[1] = m_viewParams[1];
                        viewsValid = m_viewParamsValid;
                    }
                    if (pose && pose->targetTimestampNs > lastRealStamp
                        && pose->targetTimestampNs < stampTimestampNs && viewsValid
                        && BuildMotionRotation(
                            lastMotionOrientation, pose->motion.pose.orientation, views, rotation,
                            lastMotionPosition.data(), pose->motion.pose.position
                        )
                        && enc.fill(pose->targetTimestampNs, middleClockNs, &rotation)) {
                        sentMiddle = true;
                        ++interpolated;
                        heldPending = true;
                        heldStamp = stampTimestampNs;
                        heldSendAt = job.enqueueTime + frameInterval();
                    }
                }
                if (!sentMiddle) {
                    // The output still holds this real picture.
                    enc.sendOutput(stampTimestampNs);
                    ++sentDirect;
                }
            }
            if (gapFill) {
                // Slots count from the Present that brought this picture,
                // not from the end of its encode (which can take most of a
                // frame interval and would push every deadline past the
                // application's next Present).
                if (lastRealOutput != std::chrono::steady_clock::time_point {}) {
                    auto gap = job.enqueueTime - lastRealOutput;
                    presentsShort += gap < std::chrono::milliseconds(20);
                    presentsLong += gap > std::chrono::milliseconds(34);
                }
                lastRealOutput = job.enqueueTime;
                nextSlot = lastRealOutput + frameInterval();
                retryAt = {};
                lastRealStamp = stampTimestampNs;
                slotIndex = 0;
                // A reconnected client starts a new timestamp timeline; an
                // older stamp than the last one sent means exactly that.
                lastStamp = stampTimestampNs;
            }
        } catch (std::exception const& e) {
            // An exception escaping a thread's start function is
            // std::terminate, which takes vrserver with it. Same recovery as
            // the compositor-thread paths: log, tear the encoder down, go
            // idle until the next client connect.
            Error("Encode worker: %s. Tearing down encoder and going idle.\n", e.what());
            MarkRead(job.frame);
            std::lock_guard<std::mutex> encLock(m_encMutex);
            // The try block's lock released when the exception left it, so a
            // rebuild may have swapped the encoder in between. A moved
            // generation means this failure belongs to an encoder that no
            // longer exists; leave the new one alone.
            if (job.generation != m_encGeneration) {
                continue;
            }
            m_encGeneration++;
            try {
                enc.shutdown();
            } catch (std::exception const& e2) {
                // Teardown of already-broken state must not become the
                // std::terminate this handler exists to prevent.
                Error("Encode worker: teardown also failed: %s\n", e2.what());
            }
            m_encoderState = EncoderState::Idle;
        }
    }
}

void OvrDirectModeComponent::CreateSwapTextureSet(
    uint32_t unPid,
    const SwapTextureSetDesc_t* pSwapTextureSetDesc,
    SwapTextureSet_t* pOutSwapTextureSet
) {
    Info(
        "CreateSwapTextureSet pid=%d Format=%d %dx%d SampleCount=%d\n",
        unPid,
        pSwapTextureSetDesc->nFormat,
        pSwapTextureSetDesc->nWidth,
        pSwapTextureSetDesc->nHeight,
        pSwapTextureSetDesc->nSampleCount
    );

    {
        static bool const traceDepthSets = [] {
            auto home = std::getenv("HOME");
            return home && std::ifstream(std::string(home) + "/.config/alvr/diag_submit_depth").good();
        }();
        // Persisted in vrserver.txt: a depth swap set (depth format) shows an
        // application that intends to submit depth.
        if (traceDepthSets)
            DriverLog("Submit depth swap set pid=%u format=%u %ux%u samples=%u",
                unPid, pSwapTextureSetDesc->nFormat, pSwapTextureSetDesc->nWidth,
                pSwapTextureSetDesc->nHeight, pSwapTextureSetDesc->nSampleCount);
    }
    ProcessResource* processResource = new ProcessResource();
    processResource->textDesc = *pSwapTextureSetDesc;
    processResource->pid = unPid;
    {
        char path[32];
        snprintf(path, sizeof(path), "/proc/%u/comm", unPid);
        if (FILE* comm = fopen(path, "r")) {
            char name[32] = {};
            if (fgets(name, sizeof(name), comm)) {
                processResource->compositorOwned = strncmp(name, "vrcompositor", 12) == 0;
            }
            fclose(comm);
        }
    }
    m_swapchainIndices[processResource] = 0;

    {
        auto pid = getpid();
        Info("VrServer PID %d\n", pid);
    }

    uint32_t usageFlags = static_cast<uint32_t>(
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled
        | vk::ImageUsageFlagBits::eInputAttachment
    );

    uint32_t creationFlags = static_cast<uint32_t>(vk::ImageCreateFlagBits::eMutableFormat);

    for (int i = 0; i < 3; i++) {
        vr::SharedTextureHandle_t myHandle = 0;
        bool success = vr::VRIPCResourceManager()->NewSharedVulkanImage(
            pSwapTextureSetDesc->nFormat,
            pSwapTextureSetDesc->nWidth,
            pSwapTextureSetDesc->nHeight,
            true,
            false,
            // No compute access: sRGB formats cannot be storage images, and
            // with it SteamVR's own scene arrives as linear values.
            false,
            1,
            1,
            creationFlags,
            usageFlags,
            &myHandle
        );

        uint64_t ipcHandle = 0;
        vr::VRIPCResourceManager()->RefResource(myHandle, &ipcHandle);

        if (!success) {
            Error("VRCIPCResourceManager: Failed to create shared texture\n");
            CleanupProcessResource(processResource);
            return;
        }

        int fd = 0;
        auto ret = vr::VRIPCResourceManager()->ReceiveSharedFd(ipcHandle, &fd);
        if (ret == false) {
            Error("Failed to get fd for texture\n");
            vr::VRIPCResourceManager()->UnrefResource(myHandle);
            CleanupProcessResource(processResource);
            return;
        }

        processResource->fds[i] = fd;
        processResource->sharedHandles[i] = myHandle;

        m_handleMap.insert(
            std::make_pair(processResource->sharedHandles[i], std::make_pair(processResource, i))
        );
        pOutSwapTextureSet->rSharedTextureHandles[i] = myHandle;
        Info("Created Texture %d %p\n", i, processResource->sharedHandles[i]);
    }
}

/** Used to textures created using CreateSwapTextureSet.  Only one of the set's handles needs to be
 * used to destroy the entire set. */
void OvrDirectModeComponent::DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) {
    Info("DestroySwapTextureSet %p\n", sharedTextureHandle);

    m_presentMutex.lock();
    auto id = m_handleMap.find(sharedTextureHandle);
    if (id != m_handleMap.end()) {
        ProcessResource* p = id->second.first;
        ForgetTextures({ p->sharedHandles[0], p->sharedHandles[1], p->sharedHandles[2] });
        CleanupProcessResource(p);
    } else {
        Debug("Requested to destroy not managing texture. handle:%p\n", sharedTextureHandle);
    }
    m_presentMutex.unlock();
}

/** Used to purge all texture sets for a given process. */
void OvrDirectModeComponent::DestroyAllSwapTextureSets(uint32_t unPid) {
    Info("DestroyAllSwapTextureSets pid=%d\n", unPid);

    m_presentMutex.lock();
    std::vector<ProcessResource*> resourcesToDestroy;
    for (auto it = m_handleMap.begin(); it != m_handleMap.end(); ++it) {
        if (it->second.first->pid == unPid && it->second.second == 0) {
            resourcesToDestroy.push_back(it->second.first);
        }
    }
    std::vector<vr::SharedTextureHandle_t> textures;
    for (auto* p : resourcesToDestroy) {
        textures.insert(textures.end(), p->sharedHandles, p->sharedHandles + 3);
    }
    if (!textures.empty()) {
        ForgetTextures(textures);
    }
    for (auto* p : resourcesToDestroy) {
        CleanupProcessResource(p);
    }
    m_presentMutex.unlock();
}

void OvrDirectModeComponent::ForgetTextures(std::vector<vr::SharedTextureHandle_t> const& textures
) {
    // A pending or in-flight job may still name these textures. Drop the
    // pending one, then take the encoder lock (waits for a frame being read)
    // and move the generation so a job the worker already holds is dropped
    // instead of importing an fd that is about to be closed.
    {
        std::lock_guard<std::mutex> lock(m_destroyedMutex);
        for (auto texture : textures) {
            m_destroyed.push_back(texture);
        }
        while (m_destroyed.size() > MaxDestroyedRemembered) {
            m_destroyed.pop_front();
        }
    }
    DrainPendingJob();
    std::vector<uint64_t> semaphores = steamvrsync::Sync::Get().Forget(textures);
    std::lock_guard<std::mutex> encLock(m_encMutex);
    m_encGeneration++;
    enc.releaseTextures(textures, semaphores);
}

bool OvrDirectModeComponent::WasDestroyed(vr::SharedTextureHandle_t texture) {
    std::lock_guard<std::mutex> lock(m_destroyedMutex);
    return std::find(m_destroyed.begin(), m_destroyed.end(), texture) != m_destroyed.end();
}

void OvrDirectModeComponent::CleanupProcessResource(ProcessResource* processResource) {
    for (int i = 0; i < 3; i++) {
        if (processResource->sharedHandles[i]) {
            vr::VRIPCResourceManager()->UnrefResource(processResource->sharedHandles[i]);
            m_handleMap.erase(processResource->sharedHandles[i]);
            processResource->sharedHandles[i] = 0;
        }
        if (processResource->fds[i] >= 0) {
            close(processResource->fds[i]);
            processResource->fds[i] = -1;
        }
    }
    m_swapchainIndices.erase(processResource);
    delete processResource;
}

/** After Present returns, calls this to get the next index to use for rendering. */
void OvrDirectModeComponent::GetNextSwapTextureSetIndex(
    vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t (*pIndices)[2]
) {
    Debug("OvrDirectModeComponent::GetNextSwapTextureSetIndex");

    m_presentMutex.lock();
    for (int eye = 0; eye < 2; eye++) {
        auto it = m_handleMap.find(sharedTextureHandles[eye]);
        if (it == m_handleMap.end()) {
            continue;
        }
        auto& idx = m_swapchainIndices[it->second.first];
        idx = (idx + 1) % 3;
        if (pIndices) {
            (*pIndices)[eye] = idx;
        }
    }
    m_presentMutex.unlock();
}

/** Call once per layer to draw for this frame.  One shared texture handle per eye.  Textures must
 * be created using CreateSwapTextureSet and should be alternated per frame.  Call Present once all
 * layers have been submitted. */
void OvrDirectModeComponent::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) {
    // Prediction is relative to callback entry, not to an eventual mutex
    // acquisition or a history lookup after the callback has already waited.
    auto entry=std::chrono::steady_clock::now().time_since_epoch();
    auto entryNs=std::chrono::duration_cast<std::chrono::nanoseconds>(entry).count();
    m_presentMutex.lock();
    if(m_submitLayer==0){
        auto now=std::chrono::steady_clock::now().time_since_epoch();
        auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        double predicted=double(entryNs)+double(perEye[0].flHmdPosePredictionTimeInSecondsFromNow)*1e9;
        m_depthDisplayMonotonicNs=std::isfinite(predicted)&&predicted>0&&std::abs(predicted-ns)<1e9?uint64_t(predicted):0;
    }

    static bool const traceDepth = [] {
        auto home=std::getenv("HOME");
        return home && std::ifstream(std::string(home)+"/.config/alvr/diag_submit_depth").good();
    }();
    static std::map<uint32_t,unsigned> depthCounts;
    // Per texture-owner totals: layers seen and layers that carried a depth
    // handle, summarized every ten seconds so the whole session is covered
    // (an application may submit depth only once its world is loaded).
    struct DepthTotals { uint64_t layers = 0, withDepth = 0, knownDepth = 0; };
    static std::map<uint32_t,DepthTotals> depthTotals;
    static auto nextDepthSummary = std::chrono::steady_clock::now();
    auto colorOwner=m_handleMap.find(perEye[0].hTexture);
    uint32_t ownerPid=colorOwner==m_handleMap.end()?0:colorOwner->second.first->pid;
    unsigned depthLayers=traceDepth?++depthCounts[ownerPid]:0;
    if (traceDepth) {
        auto& totals=depthTotals[ownerPid];
        ++totals.layers;
        totals.withDepth += perEye[0].hDepthTexture!=0 || perEye[1].hDepthTexture!=0;
        totals.knownDepth += m_handleMap.count(perEye[0].hDepthTexture)!=0;
        if (std::chrono::steady_clock::now() >= nextDepthSummary) {
            nextDepthSummary = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            for (auto const& [pid, sum] : depthTotals) {
                char comm[32] = "?";
                char path[32];
                snprintf(path, sizeof(path), "/proc/%u/comm", pid);
                if (FILE* f = fopen(path, "r")) {
                    if (fgets(comm, sizeof(comm), f))
                        comm[strcspn(comm, "\n")] = 0;
                    fclose(f);
                }
                DriverLog("Submit depth summary pid=%u comm=%s layers=%llu with_depth=%llu known_depth=%llu",
                    pid, comm, (unsigned long long)sum.layers, (unsigned long long)sum.withDepth,
                    (unsigned long long)sum.knownDepth);
            }
        }
    }
    // Detailed records: the first 120 layers of each owner, then one in 720.
    if (traceDepth && (depthLayers <= 120 || depthLayers % 720 == 0)) {
        for (int eye=0;eye<2;++eye) {
            auto const& layer=perEye[eye];
            bool sane=true;
            for(auto const& row:layer.mProjection.m) for(float v:row) sane=sane && std::isfinite(v);
            sane=sane && layer.mProjection.m[0][0]!=0 && layer.mProjection.m[1][1]!=0;
            auto found=m_handleMap.find(layer.hDepthTexture);
            auto desc=found==m_handleMap.end()?SwapTextureSetDesc_t{}:found->second.first->textDesc;
            DriverLog("Submit depth pid=%u layer=%u eye=%d present=%d handle=%llu known=%d format=%u size=%ux%u projection_sane=%d z22=%g z23=%g z32=%g z33=%g",
                ownerPid,depthLayers,eye,layer.hDepthTexture!=0,(unsigned long long)layer.hDepthTexture,
                found!=m_handleMap.end(),desc.nFormat,desc.nWidth,desc.nHeight,sane,
                layer.mProjection.m[2][2],layer.mProjection.m[2][3],layer.mProjection.m[3][2],layer.mProjection.m[3][3]);
        }
    }

    auto pPose = &perEye[0].mHmdPose; // TODO: are both poses the same? Name HMD suggests yes.

    if (m_submitLayer == 0) {
        // Detect FrameIndex of submitted frame by pPose.
        // This is important part to achieve smooth headtracking.
        // We search for history of TrackingInfo and find the TrackingInfo which have nearest matrix
        // value.

        auto pose = m_poseHistory->GetBestPoseMatch(*pPose);
        if (pose) {
            // found the frameIndex
            m_prevTargetTimestampNs = m_targetTimestampNs;
            m_targetTimestampNs = pose->targetTimestampNs;

            m_prevFramePoseRotation = m_framePoseRotation;
            m_framePoseRotation.x = pose->motion.pose.orientation.x;
            m_framePoseRotation.y = pose->motion.pose.orientation.y;
            m_framePoseRotation.z = pose->motion.pose.orientation.z;
            m_framePoseRotation.w = pose->motion.pose.orientation.w;
            std::copy_n(pose->motion.pose.position, 3, m_framePosePosition.begin());
        } else {
            m_targetTimestampNs = 0;
            m_framePoseRotation = HmdQuaternion_Init(0.0, 0.0, 0.0, 0.0);
        }
    }
    if (m_submitLayer == 0) {
        alvr::depth::observeRuntimeClock(m_targetTimestampNs, ownerPid, perEye[0], m_framePosePosition, entryNs);
    }
    if (m_submitLayer < MAX_LAYERS) {
        m_submitLayers[m_submitLayer][0] = perEye[0];
        m_submitLayers[m_submitLayer][1] = perEye[1];
        m_submitLayer++;
    } else {
        Warn("Too many layers submitted!\n");
    }

    m_presentMutex.unlock();
}

/** Submits queued layers for display. */
void OvrDirectModeComponent::Present(vr::SharedTextureHandle_t syncTexture) {
    int const layerCount = std::min(m_submitLayer, MAX_LAYERS);
    m_submitLayer = 0;
    // Set below when this Present queues a frame (read by PostPresent).
    m_holdUntilRead = false;
    m_queuedThisPresent = false;

    switch (m_encoderState.load()) {
    case EncoderState::RebuildRequested: {
        Info("Rebuilding encoder from negotiated settings\n");
        DrainPendingJob();
        std::lock_guard<std::mutex> encLock(m_encMutex);
        m_encGeneration++;
        enc.shutdown();
        m_encoderReady = false;
        m_encoderState = EncoderState::Streaming;
        break;
    }
    case EncoderState::ShutdownRequested: {
        DrainPendingJob();
        std::lock_guard<std::mutex> encLock(m_encMutex);
        m_encGeneration++;
        enc.shutdown();
        m_encoderReady = false;
        m_encoderState = EncoderState::Idle;
        return;
    }
    // Before a client connects there are no negotiated settings, so building
    // an encoder here would use wrong defaults and fail noisily. Do nothing
    // until streaming starts.
    case EncoderState::Idle:
        return;
    case EncoderState::Streaming:
        break;
    }

    std::optional<alvr::render::Layer> layer0 = SubmittedLayer(0);
    if (!layer0) {
        // When an application exits, SteamVR destroys its texture sets and
        // the compositor still presents one last frame naming them. That
        // frame is skipped quietly; textures never seen are still an error.
        if (WasDestroyed(m_submitLayers[0][0].hTexture)
            || WasDestroyed(m_submitLayers[0][1].hTexture)) {
            Debug(
                "Skipping a frame whose textures SteamVR just destroyed %llu, %llu\n",
                m_submitLayers[0][0].hTexture,
                m_submitLayers[0][1].hTexture
            );
            return;
        }
        Error(
            "Textures not found in handle map %llu, %llu\n",
            m_submitLayers[0][0].hTexture,
            m_submitLayers[0][1].hTexture
        );
        return;
    }

    if (!m_encoderReady) {
        auto const& settings = Settings_Instance();
        alvr::render::RendererCreateInfo rendererCI {
            .format = layer0->format,
            .inputEyeExtent = layer0->extent,
            .outputExtent {
                .width = settings->m_recommendedTargetWidth,
                .height = settings->m_recommendedTargetHeight,
            },
        };

        // Renderer and encoder setup can throw (Vulkan, VAAPI). An exception
        // escaping Present kills vrserver, so log and stay idle.
        DrainPendingJob();
        std::lock_guard<std::mutex> encLock(m_encMutex);
        m_encGeneration++;

        try {
            enc.createImages(rendererCI);
            enc.initEncoding();
            m_encoderReady = true;
        } catch (std::exception const& e) {
            Error(
                "Could not set up the encoder: %s. Staying idle until the next client connect.\n",
                e.what()
            );
            enc.shutdown();
            m_encoderState = EncoderState::Idle;
        }

        // We'll get em next time
        return;
    }

    // Drop before rendering/encoding: dropping encoded P frames would leave
    // the Quest decoder referencing pictures it never received. PostPresent
    // still paces every display slot, including those intentionally skipped.
    if (!ShouldEncodeAirLinkTestFrame()) {
        return;
    }

    FrameJob job {};
    job.frame = ++m_presentFrame;
    job.layers.push_back(*layer0);
    // Layers 1..n (SteamVR's dashboard and other overlays while a scene
    // application runs) are blended over layer 0. A layer whose textures are
    // not ours is skipped rather than dropping the frame.
    for (int i = 1; i < layerCount; ++i) {
        if (auto layer = SubmittedLayer(i)) {
            job.layers.push_back(*layer);
        }
    }
    job.targetTimestampNs = m_targetTimestampNs;
    job.renderOrientation = m_framePoseRotation;
    job.renderPosition = m_framePosePosition;
    {
        std::unique_lock<std::mutex> lock(m_viewParamsMutex);
        job.depthViews[0] = m_viewParams[0];
        job.depthViews[1] = m_viewParams[1];
        job.depthViewsValid = m_viewParamsValid;
    }
    if((mhl::depth::runtimeDepthMode()==1||mhl::depth::runtimeDepthMode()==2)&&m_depthDisplayMonotonicNs&&job.depthViewsValid){
        auto raw=m_poseHistory->TrackingFromSubmitted(m_submitLayers[0][0].mHmdPose);
        auto q=vrmath::quaternionFromRotationMatrix(raw);
        job.depthRuntime.target=job.targetTimestampNs;
        job.depthRuntime.display=m_depthDisplayMonotonicNs;
        job.depthRuntime.observed=mhl::depth::runtimeNow();
        auto owner=m_handleMap.find(m_submitLayers[0][0].hTexture);
        if(owner!=m_handleMap.end())job.depthRuntime.owner=owner->second.first->pid;
        job.depthRuntime.head={{raw.m[0][3],raw.m[1][3],raw.m[2][3]},
            {float(q.x),float(q.y),float(q.z),float(q.w)}};
        for(unsigned i=0;i<2;i++){
            auto const& v=job.depthViews[i];
            job.depthRuntime.localEyes[i]={{v.pose.position[0],v.pose.position[1],v.pose.position[2]},
                {v.pose.orientation.x,v.pose.orientation.y,v.pose.orientation.z,v.pose.orientation.w}};
            job.depthRuntime.fov[i]={v.fov.left,v.fov.right,v.fov.up,v.fov.down};
        }
    }
    job.generation = m_encGeneration;
    job.enqueueTime = std::chrono::steady_clock::now();

    // What marks the end of each texture's write. SteamVR's timeline
    // semaphore and its value, read now (by the worker SteamVR may already
    // have submitted the next write into the same texture); for a texture
    // without one, the dma-buf's implicit fences as before.
    auto& sync = steamvrsync::Sync::Get();
    for (auto const& layer : job.layers) {
        for (auto const& eye : layer.eyes) {
            FrameJob::TextureWait wait;
            wait.semaphore = sync.SemaphoreOf(eye.handle);
            if (wait.semaphore == 0 || !sync.Value(wait.semaphore, &wait.value)) {
                wait.semaphore = 0;
                wait.fenceFd = exportReadSyncFile(eye.fd);
                if (m_noSemaphoreLogged.insert(eye.handle).second) {
                    Info(
                        "SteamVR texture %llx has no timeline semaphore; using its implicit "
                        "fences and a %d ms guard\n",
                        (unsigned long long)eye.handle,
                        GuardSleepMs
                    );
                }
            }
            job.waits.push_back(wait);

            // vrcompositor's own textures: the value read here does not always
            // cover the right eye's write yet, and without a hold SteamVR goes
            // on and that write lands during the read (the right eye showed
            // the next frame). Scene applications' textures need no hold, and
            // holding for them would pass their late GPU frames on to the
            // compositor (VRChat fell to 60 fps).
            auto it = m_handleMap.find(eye.handle);
            if (it != m_handleMap.end() && it->second.first->compositorOwned) {
                m_holdUntilRead = true;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        if (m_workerExit) {
            // Teardown already drained the mailbox; do not repopulate it.
            closeFenceFds(job.waits);
            return;
        }
        // Latest wins: a still-pending job is replaced, and its fences will
        // never be waited on, so close them here.
        if (m_jobPending) {
            closeFenceFds(m_job.waits);
        }
        m_job = job;
        m_jobPending = true;
    }
    {
        std::lock_guard<std::mutex> lock(m_readMutex);
        m_lastQueuedFrame = job.frame;
    }
    m_queuedThisPresent = true;
    m_jobCv.notify_one();
}

std::optional<alvr::render::Layer> OvrDirectModeComponent::SubmittedLayer(int i) {
    alvr::render::Layer layer;
    for (int eye = 0; eye < 2; ++eye) {
        auto const& submitted = m_submitLayers[i][eye];
        auto it = m_handleMap.find(submitted.hTexture);
        if (it == m_handleMap.end()) {
            return std::nullopt;
        }
        auto const& b = submitted.bounds;
        if (!std::isfinite(b.uMin) || !std::isfinite(b.uMax) || !std::isfinite(b.vMin)
            || !std::isfinite(b.vMax) || b.uMin < 0.f || b.uMin > 1.f || b.uMax < 0.f
            || b.uMax > 1.f || b.vMin < 0.f || b.vMin > 1.f || b.vMax < 0.f || b.vMax > 1.f
            || b.uMin == b.uMax || b.vMin == b.vMax) {
            Warn("Invalid texture bounds for layer %d eye %d; skipping it\n", i, eye);
            return std::nullopt;
        }
        auto const* resource = it->second.first;
        layer.format = (vk::Format)(VkFormat)resource->textDesc.nFormat;
        layer.extent = vk::Extent2D { .width = resource->textDesc.nWidth,
                                      .height = resource->textDesc.nHeight };
        layer.eyes[eye] = {
            .handle = submitted.hTexture,
            .fd = resource->fds[it->second.second],
            .uMin = b.uMin,
            .vMin = b.vMin,
            .uMax = b.uMax,
            .vMax = b.vMax,
        };
    }
    return layer;
}

void OvrDirectModeComponent::PostPresent(const Throttling_t* pThrottling) {
    // Prop_Hmd_SupportsAppThrottling_Bool is set, so SteamVR sends throttle
    // hints here and predicts poses assuming we honor them. Ignoring them
    // while it throttles an app predicts poses for a cadence we are not
    // keeping, which reprojects the world against head motion.
    uint32_t throttleFrames = 0;
    if (pThrottling != nullptr) {
        throttleFrames = pThrottling->nFramesToThrottle;
        if (pThrottling->nFramesToThrottle != m_lastThrottleFrames
            || pThrottling->nAdditionalFramesToPredict != m_lastPredictFrames) {
            m_lastThrottleFrames = pThrottling->nFramesToThrottle;
            m_lastPredictFrames = pThrottling->nAdditionalFramesToPredict;
            Info(
                "Throttling: framesToThrottle=%u additionalFramesToPredict=%u\n",
                pThrottling->nFramesToThrottle,
                pThrottling->nAdditionalFramesToPredict
            );
        }
    }

    // Upstream's pacing against the vsync grid stays while nothing is being
    // streamed (before a client connects, while the encoder is set up) and
    // while SteamVR throttles an application, which is what honours the hint.
    // Every test of this change ran that way, and the one run without any
    // pacing from SteamVR's start hit an NVIDIA Xid 32 in vrcompositor before
    // the client connected. While frames are streamed it does not hold every
    // frame until the next tick: that is almost a whole frame, after which
    // vrcompositor's WaitForAcquire times out and it draws into the texture
    // being read (SteamVR Home shook). The vsync announcer alone kept the
    // compositor at the refresh rate then (72.0 fps measured). A throttled
    // application is untested with this change.
    if (throttleFrames > 0 || !m_queuedThisPresent) {
        PaceAfterPresent(throttleFrames);
        return;
    }

    // Hold SteamVR's submit thread until the worker has read this frame, but
    // only for frames that read vrcompositor's own textures (see Present),
    // and at most one frame interval. Devices: Linux only.
    if (m_holdUntilRead) {
        std::unique_lock<std::mutex> lock(m_readMutex);
        uint64_t const target = m_lastQueuedFrame;
        m_readCv.wait_for(lock, frameInterval(), [&] { return m_lastReadFrame >= target; });
    }
}

void OvrDirectModeComponent::GetFrameTiming(vr::DriverDirectMode_FrameTiming* pFrameTiming) {
    if (pFrameTiming == nullptr
        || pFrameTiming->m_nSize < sizeof(vr::DriverDirectMode_FrameTiming)) {
        return;
    }

    // Skipped ticks are not reported as dropped frames: the compositor reads a
    // drop as GPU overload and enables interleaved reprojection, which halves
    // the rate, so the grid then skips a tick every frame and reports another
    // drop, throttled from then on. Consume the counter without reporting it
    // until there is a channel that does not throttle.
    (void)TakeSkippedVsyncs();
    pFrameTiming->m_nNumFramePresents = 1;
    pFrameTiming->m_nNumMisPresented = 0;
    pFrameTiming->m_nNumDroppedFrames = 0;
    // These bits overlap the throttle mask and must not be echoed back
    // unhandled, per the interface header.
    pFrameTiming->m_nReprojectionFlags = 0;
}
