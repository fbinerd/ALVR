#include "PoseHistory.h"
#include "Logger.h"
#include "Utils.h"
#include "include/openvr_math.h"
#include <mutex>
#include <optional>

// A submitted frame was rendered from a recent pose, so matches older than this
// are stale no matter how well the rotation agrees.
static constexpr uint64_t MATCH_WINDOW_NS = 250000000;

void PoseHistory::OnPoseUpdated(uint64_t targetTimestampNs, FfiDeviceMotion motion) {
    // Put pose history buffer
    TrackingHistoryFrame history;
    history.targetTimestampNs = targetTimestampNs;
    history.motion = motion;

    HmdMatrix_QuatToMat(
        motion.pose.orientation.w,
        motion.pose.orientation.x,
        motion.pose.orientation.y,
        motion.pose.orientation.z,
        &history.rotationMatrix
    );

    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_transformIdentity) {
        vr::HmdMatrix34_t rotation = vrmath::matMul33(m_transform, history.rotationMatrix);
        history.rotationMatrix = rotation;
    }

    if (m_poseBuffer.size() == 0) {
        m_poseBuffer.push_back(history);
    } else {
        if (m_poseBuffer.back().targetTimestampNs != targetTimestampNs) {
            // New track info
            m_poseBuffer.push_back(history);
        }
    }
    // The value should match with the client's MAXIMUM_TRACKING_FRAMES in ovr_context.cpp
    if (m_poseBuffer.size() > 120 * 3) {
        m_poseBuffer.pop_front();
    }
}

std::optional<PoseHistory::TrackingHistoryFrame>
PoseHistory::GetBestPoseMatch(const vr::HmdMatrix34_t& pose) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_poseBuffer.empty()) {
        Debug("PoseHistory::GetBestPoseMatch: No pose matched.");
        return {};
    }

    // The buffer holds about 1.7 seconds of tracking, so sweeping the head back
    // over an orientation it recently held leaves several entries matching
    // equally well. Search newest first so a tie resolves to the most recent
    // entry, and stop at the window bound.
    const uint64_t newestTimestampNs = m_poseBuffer.back().targetTimestampNs;
    float minDiff = 100000;
    auto minIt = m_poseBuffer.rbegin();
    for (auto it = m_poseBuffer.rbegin(); it != m_poseBuffer.rend(); ++it) {
        if (newestTimestampNs - it->targetTimestampNs > MATCH_WINDOW_NS) {
            break;
        }
        float distance = 0;
        // Rotation matrix composes a part of ViewMatrix of TrackingInfo.
        // Be carefull of transpose.
        // And bottom side and right side of matrix should not be compared, because pPose does not
        // contain that part of matrix.
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                distance += pow(it->rotationMatrix.m[j][i] - pose.m[j][i], 2);
            }
        }
        if (minDiff > distance) {
            minIt = it;
            minDiff = distance;
        }
    }
    return *minIt;
}

std::optional<PoseHistory::TrackingHistoryFrame> PoseHistory::GetPoseAt(uint64_t timestampNs
) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    for (auto it = m_poseBuffer.rbegin(), end = m_poseBuffer.rend(); it != end; ++it) {
        if (it->targetTimestampNs == timestampNs)
            return *it;
    }

    Debug("PoseHistory::GetPoseAt: No pose matched.");
    return {};
}

std::optional<PoseHistory::TrackingHistoryFrame>
PoseHistory::GetPoseNearest(uint64_t timestampNs) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    std::optional<TrackingHistoryFrame> best;
    uint64_t bestDistance = UINT64_MAX;
    for (auto const& frame : m_poseBuffer) {
        uint64_t distance = frame.targetTimestampNs > timestampNs
            ? frame.targetTimestampNs - timestampNs
            : timestampNs - frame.targetTimestampNs;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = frame;
        }
    }
    return best;
}

std::optional<PoseHistory::TrackingHistoryFrame> PoseHistory::GetLatestPose() const {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_poseBuffer.empty()) {
        return {};
    }
    return m_poseBuffer.back();
}

std::optional<std::array<float, 3>> PoseHistory::GetPositionInterpolated(uint64_t timestampNs) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    TrackingHistoryFrame const* lower = nullptr;
    TrackingHistoryFrame const* upper = nullptr;
    for (auto const& frame : m_poseBuffer) {
        auto t = frame.targetTimestampNs;
        if (t <= timestampNs && (!lower || t > lower->targetTimestampNs)) lower = &frame;
        if (t >= timestampNs && (!upper || t < upper->targetTimestampNs)) upper = &frame;
    }
    if (!lower || !upper || upper->targetTimestampNs - lower->targetTimestampNs > 25'000'000)
        return {};
    auto span = upper->targetTimestampNs - lower->targetTimestampNs;
    double weight = span ? double(timestampNs - lower->targetTimestampNs) / double(span) : 0;
    std::array<float, 3> position {};
    for (unsigned i = 0; i < 3; ++i) {
        double a = lower->motion.pose.position[i], b = upper->motion.pose.position[i];
        if (!std::isfinite(a) || !std::isfinite(b)) return {};
        position[i] = float(a + weight * (b - a));
    }
    return position;
}

void PoseHistory::SetTransform(const vr::HmdMatrix34_t& transform) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_transform = transform;

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (transform.m[i][j] != (i == j ? 1 : 0)) {
                m_transformIdentity = false;
                return;
            }
        }
    }
    m_transformIdentity = true;
}

vr::HmdMatrix34_t PoseHistory::TrackingFromSubmitted(vr::HmdMatrix34_t const& pose) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    vr::HmdMatrix34_t raw{};
    for(unsigned i=0;i<3;i++) {
        for(unsigned j=0;j<3;j++)for(unsigned k=0;k<3;k++)
            raw.m[i][j]+=m_transform.m[k][i]*pose.m[k][j];
        for(unsigned k=0;k<3;k++)raw.m[i][3]+=m_transform.m[k][i]*(pose.m[k][3]-m_transform.m[k][3]);
    }
    return raw;
}


std::optional<PoseHistory::TrackingHistoryFrame>
PoseHistory::GetUniqueDepthPoseMatch(vr::HmdMatrix34_t const& pose) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    if(m_poseBuffer.empty())return {};
    auto newest=m_poseBuffer.back().targetTimestampNs;
    std::optional<TrackingHistoryFrame> result;
    for(auto it=m_poseBuffer.rbegin();it!=m_poseBuffer.rend();++it){
        if(newest-it->targetTimestampNs>MATCH_WINDOW_NS)break;
        double positionError=0,rotationError=0;
        for(unsigned i=0;i<3;i++){
            double p=m_transform.m[i][3];
            for(unsigned j=0;j<3;j++){
                p+=m_transform.m[i][j]*it->motion.pose.position[j];
                double d=it->rotationMatrix.m[i][j]-pose.m[i][j];rotationError+=d*d;
            }
            double d=p-pose.m[i][3];positionError+=d*d;
        }
        if(!std::isfinite(positionError)||!std::isfinite(rotationError)||positionError>1e-12||rotationError>1e-10)continue;
        if(result)return {}; // Still/ambiguous poses are not clock evidence.
        result=*it;
    }
    return result;
}
