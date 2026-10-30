#include <NF/Animation/Inertialization.hpp>
#include <algorithm>
#include <cmath>

namespace nf::animation {

namespace {

inline Vec3 quat_to_angle_axis(Quat q) {
    if (q.w < 0.0f) {
        q.x = -q.x;
        q.y = -q.y;
        q.z = -q.z;
        q.w = -q.w;
    }

    float clampedW = std::clamp(q.w, -1.0f, 1.0f);
    float angle = 2.0f * std::acos(clampedW);
    float sinHalf = std::sqrt(std::max(0.0f, 1.0f - clampedW * clampedW));

    if (sinHalf > 1e-6f && angle > 1e-6f) {
        float factor = angle / sinHalf;
        return Vec3(q.x * factor, q.y * factor, q.z * factor);
    }
    return Vec3(0.0f, 0.0f, 0.0f);
}

inline Quat angle_axis_to_quat(const Vec3& aa) {
    float angleSq = aa.x * aa.x + aa.y * aa.y + aa.z * aa.z;
    if (angleSq > 1e-8f) {
        float angle = std::sqrt(angleSq);
        Vec3 axis(aa.x / angle, aa.y / angle, aa.z / angle);
        return Quat::from_axis_angle(axis, angle);
    }
    return Quat::identity();
}

} // namespace

float Inertializer::decay_quintic(float x0, float v0, float t, float t0) {
    if (t >= t0 || t0 <= 1e-6f) {
        return 0.0f;
    }

    float u = 1.0f - (t / t0);
    float u3 = u * u * u;

    float invT0 = 1.0f / t0;
    float invT0Sq = invT0 * invT0;

    float a = x0;
    float b = v0 + 3.0f * x0 * invT0;
    float c = 3.0f * v0 * invT0 + 6.0f * x0 * invT0Sq;

    return (a + b * t + c * t * t) * u3;
}

Vec3 Inertializer::decay_vec3(const Vec3& x0, const Vec3& v0, float t, float t0) {
    return Vec3(
        decay_quintic(x0.x, v0.x, t, t0),
        decay_quintic(x0.y, v0.y, t, t0),
        decay_quintic(x0.z, v0.z, t, t0)
    );
}

void Inertializer::reset() {
    m_duration = 0.0f;
    m_elapsedTime = 0.0f;
    m_boneStates.clear();
}

void Inertializer::transition(
    const std::vector<LocalPose>& prevPose,
    const std::vector<LocalPose>& prevVel,
    const std::vector<LocalPose>& targetPose,
    const std::vector<LocalPose>& targetVel,
    float blendDuration
) {
    if (blendDuration <= 1e-4f || prevPose.empty() || targetPose.empty()) {
        reset();
        return;
    }

    m_duration = blendDuration;
    m_elapsedTime = 0.0f;

    const usize boneCount = std::min(prevPose.size(), targetPose.size());
    m_boneStates.resize(boneCount);

    for (usize i = 0; i < boneCount; ++i) {
        BoneInertialState& state = m_boneStates[i];
        const LocalPose& pPrev = prevPose[i];
        const LocalPose& pTarg = targetPose[i];

        // Translation offsets
        state.transOffset = Vec3(
            pPrev.translation.x - pTarg.translation.x,
            pPrev.translation.y - pTarg.translation.y,
            pPrev.translation.z - pTarg.translation.z
        );

        if (i < prevVel.size() && i < targetVel.size()) {
            state.transVelocity = Vec3(
                prevVel[i].translation.x - targetVel[i].translation.x,
                prevVel[i].translation.y - targetVel[i].translation.y,
                prevVel[i].translation.z - targetVel[i].translation.z
            );
        } else {
            state.transVelocity = Vec3(0, 0, 0);
        }

        // Rotation offsets (relative quaternion difference)
        Quat targInv = pTarg.rotation.inverse();
        Quat rotDiff = targInv * pPrev.rotation;
        state.rotAngleAxis = quat_to_angle_axis(rotDiff);

        // Approximate angular velocity offset
        state.rotAngularVelocity = Vec3(0, 0, 0);

        // Scale offsets
        state.scaleOffset = Vec3(
            pPrev.scale.x - pTarg.scale.x,
            pPrev.scale.y - pTarg.scale.y,
            pPrev.scale.z - pTarg.scale.z
        );
        state.scaleVelocity = Vec3(0, 0, 0);
    }
}

void Inertializer::transition_from_previous(
    const std::vector<LocalPose>& currentPose,
    const std::vector<LocalPose>& previousPose,
    const std::vector<LocalPose>& targetPose,
    float dt,
    float blendDuration
) {
    if (currentPose.empty() || targetPose.empty() || blendDuration <= 1e-4f) {
        reset();
        return;
    }

    std::vector<LocalPose> velocities(currentPose.size());
    float invDt = dt > 1e-5f ? (1.0f / dt) : 0.0f;

    for (usize i = 0; i < currentPose.size(); ++i) {
        if (i < previousPose.size()) {
            velocities[i].translation = Vec3(
                (currentPose[i].translation.x - previousPose[i].translation.x) * invDt,
                (currentPose[i].translation.y - previousPose[i].translation.y) * invDt,
                (currentPose[i].translation.z - previousPose[i].translation.z) * invDt
            );
        }
    }

    std::vector<LocalPose> zeroVel(targetPose.size());
    transition(currentPose, velocities, targetPose, zeroVel, blendDuration);
}

bool Inertializer::update_and_blend(
    float dt,
    const std::vector<LocalPose>& targetPose,
    std::vector<LocalPose>& outPose
) {
    if (!is_active()) {
        outPose = targetPose;
        return false;
    }

    m_elapsedTime += dt;

    if (m_elapsedTime >= m_duration) {
        outPose = targetPose;
        m_elapsedTime = m_duration;
        return false;
    }

    const usize boneCount = std::min(m_boneStates.size(), targetPose.size());
    outPose.resize(targetPose.size());

    // Copy any untracked tail bones directly
    for (usize i = boneCount; i < targetPose.size(); ++i) {
        outPose[i] = targetPose[i];
    }

    for (usize i = 0; i < boneCount; ++i) {
        const BoneInertialState& state = m_boneStates[i];
        const LocalPose& targ = targetPose[i];

        LocalPose& result = outPose[i];

        // 1. Decaying translation
        Vec3 decayedTrans = decay_vec3(state.transOffset, state.transVelocity, m_elapsedTime, m_duration);
        result.translation = Vec3(
            targ.translation.x + decayedTrans.x,
            targ.translation.y + decayedTrans.y,
            targ.translation.z + decayedTrans.z
        );

        // 2. Decaying rotation
        Vec3 decayedRotAA = decay_vec3(state.rotAngleAxis, state.rotAngularVelocity, m_elapsedTime, m_duration);
        Quat rotOffset = angle_axis_to_quat(decayedRotAA);
        result.rotation = (targ.rotation * rotOffset).normalized();

        // 3. Decaying scale
        Vec3 decayedScale = decay_vec3(state.scaleOffset, state.scaleVelocity, m_elapsedTime, m_duration);
        result.scale = Vec3(
            targ.scale.x + decayedScale.x,
            targ.scale.y + decayedScale.y,
            targ.scale.z + decayedScale.z
        );
    }

    return true;
}

} // namespace nf::animation
