#include <NF/Scene/Spline.hpp>
#include <algorithm>
#include <cmath>

namespace nf::scene {

// ----------------------------------------------------------------------------
// Spline3D
// ----------------------------------------------------------------------------

void Spline3D::add_point(const SplinePoint& point) {
    m_points.push_back(point);
    m_dirty = true;
}

void Spline3D::add_point(const Vec3& pos, SplinePointType type) {
    m_points.emplace_back(pos, type);
    m_dirty = true;
}

void Spline3D::insert_point(size_t index, const SplinePoint& point) {
    if (index >= m_points.size()) {
        m_points.push_back(point);
    } else {
        m_points.insert(m_points.begin() + static_cast<std::ptrdiff_t>(index), point);
    }
    m_dirty = true;
}

void Spline3D::remove_point(size_t index) {
    if (index < m_points.size()) {
        m_points.erase(m_points.begin() + static_cast<std::ptrdiff_t>(index));
        m_dirty = true;
    }
}

void Spline3D::clear() {
    m_points.clear();
    m_arc_length_table.clear();
    m_total_length = 0.0f;
    m_dirty = true;
}

void Spline3D::set_point_position(size_t index, const Vec3& pos) {
    if (index < m_points.size()) {
        m_points[index].position = pos;
        m_dirty = true;
    }
}

void Spline3D::set_point_tangents(size_t index, const Vec3& arrive, const Vec3& leave) {
    if (index < m_points.size()) {
        m_points[index].arrive_tangent = arrive;
        m_points[index].leave_tangent = leave;
        m_points[index].type = SplinePointType::CurveCustom;
        m_dirty = true;
    }
}

void Spline3D::set_point_rotation(size_t index, const Quat& rot) {
    if (index < m_points.size()) {
        m_points[index].rotation = rot;
        m_dirty = true;
    }
}

void Spline3D::set_point_scale(size_t index, const Vec3& scale) {
    if (index < m_points.size()) {
        m_points[index].scale = scale;
        m_dirty = true;
    }
}

void Spline3D::set_point_type(size_t index, SplinePointType type) {
    if (index < m_points.size()) {
        m_points[index].type = type;
        m_dirty = true;
    }
}

const SplinePoint& Spline3D::get_point(size_t index) const {
    return m_points.at(index);
}

SplinePoint& Spline3D::get_point(size_t index) {
    m_dirty = true;
    return m_points.at(index);
}

void Spline3D::set_closed(bool closed) {
    if (m_is_closed != closed) {
        m_is_closed = closed;
        m_dirty = true;
    }
}

size_t Spline3D::segment_count() const noexcept {
    if (m_points.size() < 2) {
        return 0;
    }
    return m_is_closed ? m_points.size() : (m_points.size() - 1);
}

void Spline3D::update_auto_tangents() {
    const size_t n = m_points.size();
    if (n < 2) {
        return;
    }

    for (size_t i = 0; i < n; ++i) {
        if (m_points[i].type != SplinePointType::CurveAuto &&
            m_points[i].type != SplinePointType::CurveClamped) {
            continue;
        }

        Vec3 prev_pos;
        Vec3 next_pos;

        if (m_is_closed) {
            prev_pos = m_points[(i + n - 1) % n].position;
            next_pos = m_points[(i + 1) % n].position;
        } else {
            if (i == 0) {
                prev_pos = m_points[0].position;
                next_pos = m_points[1].position;
                const Vec3 tan = next_pos - prev_pos;
                m_points[0].arrive_tangent = tan;
                m_points[0].leave_tangent = tan;
                continue;
            } else if (i == n - 1) {
                prev_pos = m_points[n - 2].position;
                next_pos = m_points[n - 1].position;
                const Vec3 tan = next_pos - prev_pos;
                m_points[n - 1].arrive_tangent = tan;
                m_points[n - 1].leave_tangent = tan;
                continue;
            } else {
                prev_pos = m_points[i - 1].position;
                next_pos = m_points[i + 1].position;
            }
        }

        Vec3 tan = (next_pos - prev_pos) * 0.5f;

        if (m_points[i].type == SplinePointType::CurveClamped) {
            const f32 d1 = (m_points[i].position - prev_pos).length();
            const f32 d2 = (next_pos - m_points[i].position).length();
            const f32 max_len = std::min(d1, d2);
            if (tan.length() > max_len && max_len > 1e-5f) {
                tan = tan.normalized() * max_len;
            }
        }

        m_points[i].arrive_tangent = tan;
        m_points[i].leave_tangent = tan;
    }
}

Vec3 Spline3D::evaluate_segment_position(size_t seg_idx, f32 u) const {
    const size_t n = m_points.size();
    if (n < 2 || seg_idx >= segment_count()) {
        return m_points.empty() ? Vec3{0, 0, 0} : m_points[0].position;
    }

    const size_t idx0 = seg_idx;
    const size_t idx1 = m_is_closed ? ((seg_idx + 1) % n) : (seg_idx + 1);

    const auto& pt0 = m_points[idx0];
    const auto& pt1 = m_points[idx1];

    if (pt0.type == SplinePointType::Linear && pt1.type == SplinePointType::Linear) {
        return pt0.position + (pt1.position - pt0.position) * u;
    }

    Vec3 m0 = pt0.leave_tangent;
    Vec3 m1 = pt1.arrive_tangent;
    if (pt0.type == SplinePointType::Linear) {
        m0 = pt1.position - pt0.position;
    }
    if (pt1.type == SplinePointType::Linear) {
        m1 = pt1.position - pt0.position;
    }

    const f32 u2 = u * u;
    const f32 u3 = u2 * u;

    const f32 h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    const f32 h10 = u3 - 2.0f * u2 + u;
    const f32 h01 = -2.0f * u3 + 3.0f * u2;
    const f32 h11 = u3 - u2;

    return pt0.position * h00 + m0 * h10 + pt1.position * h01 + m1 * h11;
}

Vec3 Spline3D::evaluate_segment_tangent(size_t seg_idx, f32 u) const {
    const size_t n = m_points.size();
    if (n < 2 || seg_idx >= segment_count()) {
        return Vec3{0, 0, 1};
    }

    const size_t idx0 = seg_idx;
    const size_t idx1 = m_is_closed ? ((seg_idx + 1) % n) : (seg_idx + 1);

    const auto& pt0 = m_points[idx0];
    const auto& pt1 = m_points[idx1];

    if (pt0.type == SplinePointType::Linear && pt1.type == SplinePointType::Linear) {
        return pt1.position - pt0.position;
    }

    Vec3 m0 = pt0.leave_tangent;
    Vec3 m1 = pt1.arrive_tangent;
    if (pt0.type == SplinePointType::Linear) {
        m0 = pt1.position - pt0.position;
    }
    if (pt1.type == SplinePointType::Linear) {
        m1 = pt1.position - pt0.position;
    }

    const f32 u2 = u * u;

    const f32 dh00 = 6.0f * u2 - 6.0f * u;
    const f32 dh10 = 3.0f * u2 - 4.0f * u + 1.0f;
    const f32 dh01 = -6.0f * u2 + 6.0f * u;
    const f32 dh11 = 3.0f * u2 - 2.0f * u;

    return pt0.position * dh00 + m0 * dh10 + pt1.position * dh01 + m1 * dh11;
}

Vec3 Spline3D::evaluate_segment_acceleration(size_t seg_idx, f32 u) const {
    const size_t n = m_points.size();
    if (n < 2 || seg_idx >= segment_count()) {
        return Vec3{0, 0, 0};
    }

    const size_t idx0 = seg_idx;
    const size_t idx1 = m_is_closed ? ((seg_idx + 1) % n) : (seg_idx + 1);

    const auto& pt0 = m_points[idx0];
    const auto& pt1 = m_points[idx1];

    Vec3 m0 = pt0.leave_tangent;
    Vec3 m1 = pt1.arrive_tangent;
    if (pt0.type == SplinePointType::Linear) {
        m0 = pt1.position - pt0.position;
    }
    if (pt1.type == SplinePointType::Linear) {
        m1 = pt1.position - pt0.position;
    }

    const f32 d2h00 = 12.0f * u - 6.0f;
    const f32 d2h10 = 6.0f * u - 4.0f;
    const f32 d2h01 = -12.0f * u + 6.0f;
    const f32 d2h11 = 6.0f * u - 2.0f;

    return pt0.position * d2h00 + m0 * d2h10 + pt1.position * d2h01 + m1 * d2h11;
}

void Spline3D::rebuild_arc_length_table(size_t samples_per_segment) {
    update_auto_tangents();
    m_arc_length_table.clear();
    m_total_length = 0.0f;

    const size_t seg_count = segment_count();
    if (seg_count == 0) {
        m_dirty = false;
        return;
    }

    const size_t total_samples = seg_count * std::max<size_t>(2, samples_per_segment);
    m_arc_length_table.reserve(total_samples + 1);

    m_arc_length_table.push_back({0.0f, 0.0f});
    Vec3 prev_pos = evaluate_segment_position(0, 0.0f);

    for (size_t seg = 0; seg < seg_count; ++seg) {
        const size_t steps = std::max<size_t>(2, samples_per_segment);
        for (size_t step = 1; step <= steps; ++step) {
            const f32 u = static_cast<f32>(step) / static_cast<f32>(steps);
            const f32 global_t = (static_cast<f32>(seg) + u) / static_cast<f32>(seg_count);
            const Vec3 cur_pos = evaluate_segment_position(seg, u);
            const f32 step_dist = (cur_pos - prev_pos).length();
            m_total_length += step_dist;
            m_arc_length_table.push_back({global_t, m_total_length});
            prev_pos = cur_pos;
        }
    }

    m_dirty = false;
}

f32 Spline3D::distance_to_time(f32 distance) const {
    if (m_dirty || m_arc_length_table.empty()) {
        const_cast<Spline3D*>(this)->rebuild_arc_length_table();
    }

    if (m_arc_length_table.empty() || m_total_length <= 1e-5f) {
        return 0.0f;
    }

    if (distance <= 0.0f) {
        return 0.0f;
    }
    if (distance >= m_total_length) {
        return 1.0f;
    }

    // Binary search in cumulative distance
    auto it = std::lower_bound(
        m_arc_length_table.begin(), m_arc_length_table.end(), distance,
        [](const ArcLengthSample& sample, f32 dist) {
            return sample.cumulative_distance < dist;
        }
    );

    if (it == m_arc_length_table.begin()) {
        return it->t;
    }
    if (it == m_arc_length_table.end()) {
        return 1.0f;
    }

    const auto prev_it = it - 1;
    const f32 dist_range = it->cumulative_distance - prev_it->cumulative_distance;
    if (dist_range <= 1e-6f) {
        return it->t;
    }

    const f32 alpha = (distance - prev_it->cumulative_distance) / dist_range;
    return prev_it->t + (it->t - prev_it->t) * alpha;
}

f32 Spline3D::time_to_distance(f32 t) const {
    if (m_dirty || m_arc_length_table.empty()) {
        const_cast<Spline3D*>(this)->rebuild_arc_length_table();
    }

    if (m_arc_length_table.empty() || m_total_length <= 1e-5f) {
        return 0.0f;
    }

    const f32 clamped_t = std::clamp(t, 0.0f, 1.0f);
    auto it = std::lower_bound(
        m_arc_length_table.begin(), m_arc_length_table.end(), clamped_t,
        [](const ArcLengthSample& sample, f32 time_val) {
            return sample.t < time_val;
        }
    );

    if (it == m_arc_length_table.begin()) {
        return it->cumulative_distance;
    }
    if (it == m_arc_length_table.end()) {
        return m_total_length;
    }

    const auto prev_it = it - 1;
    const f32 t_range = it->t - prev_it->t;
    if (t_range <= 1e-6f) {
        return it->cumulative_distance;
    }

    const f32 alpha = (clamped_t - prev_it->t) / t_range;
    return prev_it->cumulative_distance + (it->cumulative_distance - prev_it->cumulative_distance) * alpha;
}

SplineSample Spline3D::evaluate(f32 t) const {
    if (m_dirty) {
        const_cast<Spline3D*>(this)->rebuild_arc_length_table();
    }

    SplineSample sample;
    const size_t seg_count = segment_count();
    if (seg_count == 0) {
        if (!m_points.empty()) {
            sample.position = m_points[0].position;
            sample.rotation = m_points[0].rotation;
            sample.scale = m_points[0].scale;
        }
        return sample;
    }

    f32 clamped_t = t;
    if (m_is_closed) {
        clamped_t = std::fmod(clamped_t, 1.0f);
        if (clamped_t < 0.0f) {
            clamped_t += 1.0f;
        }
    } else {
        clamped_t = std::clamp(clamped_t, 0.0f, 1.0f);
    }

    sample.t = clamped_t;

    f32 scaled_t = clamped_t * static_cast<f32>(seg_count);
    size_t seg_idx = static_cast<size_t>(scaled_t);
    if (seg_idx >= seg_count) {
        seg_idx = seg_count - 1;
        scaled_t = static_cast<f32>(seg_count);
    }

    f32 u = scaled_t - static_cast<f32>(seg_idx);
    if (!m_is_closed && clamped_t >= 1.0f) {
        seg_idx = seg_count - 1;
        u = 1.0f;
    }

    sample.position = evaluate_segment_position(seg_idx, u);
    const Vec3 deriv1 = evaluate_segment_tangent(seg_idx, u);
    const Vec3 deriv2 = evaluate_segment_acceleration(seg_idx, u);

    const f32 len1 = deriv1.length();
    if (len1 > 1e-5f) {
        sample.tangent = deriv1 / len1;
        const Vec3 cross_p = deriv1.cross(deriv2);
        const f32 cross_len = cross_p.length();
        sample.curvature = cross_len / (len1 * len1 * len1);
    } else {
        sample.tangent = Vec3(0.0f, 0.0f, 1.0f);
        sample.curvature = 0.0f;
    }

    const size_t n = m_points.size();
    const size_t idx0 = seg_idx;
    const size_t idx1 = m_is_closed ? ((seg_idx + 1) % n) : (seg_idx + 1);

    sample.rotation = Quat::slerp(m_points[idx0].rotation, m_points[idx1].rotation, u);
    sample.scale = m_points[idx0].scale * (1.0f - u) + m_points[idx1].scale * u;

    sample.up = sample.rotation.rotate(Vec3(0.0f, 1.0f, 0.0f)).normalized();
    sample.right = sample.tangent.cross(sample.up).normalized();
    sample.up = sample.right.cross(sample.tangent).normalized();

    sample.distance = time_to_distance(clamped_t);
    return sample;
}

SplineSample Spline3D::evaluate_at_distance(f32 distance) const {
    const f32 t = distance_to_time(distance);
    return evaluate(t);
}

SplineSample Spline3D::find_nearest_point(const Vec3& world_pos, f32& out_distance, size_t subdivisions) const {
    if (m_dirty) {
        const_cast<Spline3D*>(this)->rebuild_arc_length_table();
    }

    const size_t steps = std::max<size_t>(8, subdivisions);
    f32 best_t = 0.0f;
    f32 min_dist_sq = 1e30f;

    for (size_t i = 0; i <= steps; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
        const auto sample = evaluate(t);
        const f32 d2 = (sample.position - world_pos).length_sq();
        if (d2 < min_dist_sq) {
            min_dist_sq = d2;
            best_t = t;
        }
    }

    // Local refinement (golden section search)
    const f32 step_size = 1.0f / static_cast<f32>(steps);
    f32 low = std::max(0.0f, best_t - step_size);
    f32 high = std::min(1.0f, best_t + step_size);

    constexpr f32 phi = 0.6180339887f;
    f32 x1 = high - phi * (high - low);
    f32 x2 = low + phi * (high - low);
    f32 d1 = (evaluate(x1).position - world_pos).length_sq();
    f32 d2 = (evaluate(x2).position - world_pos).length_sq();

    for (int iter = 0; iter < 16; ++iter) {
        if (d1 < d2) {
            high = x2;
            x2 = x1;
            d2 = d1;
            x1 = high - phi * (high - low);
            d1 = (evaluate(x1).position - world_pos).length_sq();
        } else {
            low = x1;
            x1 = x2;
            d1 = d2;
            x2 = low + phi * (high - low);
            d2 = (evaluate(x2).position - world_pos).length_sq();
        }
    }

    best_t = (low + high) * 0.5f;
    const auto result = evaluate(best_t);
    out_distance = result.distance;
    return result;
}

// ----------------------------------------------------------------------------
// SplineFollower
// ----------------------------------------------------------------------------

SplineFollower::SplineFollower(const Spline3D* spline)
    : m_spline(spline) {
    if (m_spline) {
        m_current_sample = m_spline->evaluate(0.0f);
    }
}

void SplineFollower::set_spline(const Spline3D* spline) {
    m_spline = spline;
    reset();
}

void SplineFollower::reset() {
    m_current_distance = 0.0f;
    m_finished = false;
    m_reverse = false;
    if (m_spline) {
        m_current_sample = m_spline->evaluate(0.0f);
    }
}

void SplineFollower::seek_distance(f32 distance) {
    if (!m_spline) {
        return;
    }
    const f32 total = m_spline->total_length();
    m_current_distance = std::clamp(distance, 0.0f, total);
    m_current_sample = m_spline->evaluate_at_distance(m_current_distance);
}

void SplineFollower::seek_ratio(f32 ratio) {
    if (!m_spline) {
        return;
    }
    const f32 total = m_spline->total_length();
    seek_distance(ratio * total);
}

void SplineFollower::update(f32 dt) {
    if (!m_spline || m_finished) {
        return;
    }

    const f32 total = m_spline->total_length();
    if (total <= 1e-4f) {
        return;
    }

    const f32 step = m_speed * dt;

    if (m_mode == SplineFollowMode::Once) {
        m_current_distance += step;
        if (m_current_distance >= total) {
            m_current_distance = total;
            m_finished = true;
        }
    } else if (m_mode == SplineFollowMode::Loop) {
        m_current_distance += step;
        m_current_distance = std::fmod(m_current_distance, total);
        if (m_current_distance < 0.0f) {
            m_current_distance += total;
        }
    } else if (m_mode == SplineFollowMode::PingPong) {
        if (!m_reverse) {
            m_current_distance += step;
            if (m_current_distance >= total) {
                m_current_distance = total - (m_current_distance - total);
                m_reverse = true;
            }
        } else {
            m_current_distance -= step;
            if (m_current_distance <= 0.0f) {
                m_current_distance = -m_current_distance;
                m_reverse = false;
            }
        }
    }

    m_current_sample = m_spline->evaluate_at_distance(m_current_distance);

    // Lateral banking tilt for camera/aircraft/coasters
    if (std::fabs(m_bank_multiplier) > 1e-4f && m_current_sample.curvature > 1e-4f) {
        // Project acceleration vector onto the right vector to determine turn direction
        const f32 t = m_spline->distance_to_time(m_current_distance);
        const size_t seg_count = m_spline->segment_count();
        if (seg_count > 0) {
            const f32 scaled_t = t * static_cast<f32>(seg_count);
            const size_t seg_idx = std::min(static_cast<size_t>(scaled_t), seg_count - 1);
            const f32 u = scaled_t - static_cast<f32>(seg_idx);
            const Vec3 accel = m_spline->evaluate_segment_acceleration(seg_idx, std::clamp(u, 0.0f, 1.0f));
            const f32 lateral = accel.dot(m_current_sample.right);
            const f32 roll_angle = -lateral * m_bank_multiplier * 0.05f;

            const Quat roll_q = Quat::from_axis_angle(m_current_sample.tangent, roll_angle);
            m_current_sample.rotation = (roll_q * m_current_sample.rotation).normalized();
            m_current_sample.up = roll_q.rotate(m_current_sample.up);
            m_current_sample.right = roll_q.rotate(m_current_sample.right);
        }
    }
}

} // namespace nf::scene
