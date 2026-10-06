/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "notationautomationcontroller.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <set>

#include "uicomponents/qml/Muse/UiComponents/polylineplot.h"

#include "engraving/iengravingconfiguration.h" // IWYU pragma: keep
#include "engraving/automation/automationdata.h"
#include "engraving/automation/dynamicvalues.h"
#include "engraving/automation/tempovalues.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/staff.h"

#include "notation/imasternotation.h"
#include "notation/inotation.h"
#include "notation/inotationautomation.h"
#include "notation/inotationelements.h" // IWYU pragma: keep

#include "midieditor/midieditornotes.h"

#include "global/async/async.h"
#include "global/containers.h"
#include "global/realfn.h"

using namespace mu::notation;
using namespace mu::engraving;
using namespace muse::uicomponents;

using SetPoint = mu::engraving::AutomationPointEdit::SetPoint;
using MovePoint = mu::engraving::AutomationPointEdit::MovePoint;
using ErasePoint = mu::engraving::AutomationPointEdit::ErasePoint;

constexpr static qreal POLYLINE_LINE_WIDTH = 1.5;

constexpr static qreal POLYLINE_STANDARD_CENTER_RADIUS = 3.0;
constexpr static qreal POLYLINE_HOVERED_CENTER_RADIUS = 3.5;
constexpr static qreal POLYLINE_SELECTED_CENTER_RADIUS = 3.5;

constexpr static qreal POLYLINE_SELECTED_MIDDLE_RING_WIDTH = 1.5;

constexpr static int POLYLINE_SELECTED_HOVERED_ALPHA = 127;
constexpr static int POLYLINE_GENERATED_AREA_ALPHA = 51;
constexpr static int POLYLINE_EDITED_AREA_ALPHA = 102;

//! How far a bend handle is allowed to go along its own segment. Upstream reads `t <= 0` or `t >= 1`
//! as "no bend at all" (see `muse::mpe::evaluateAt`), so a handle dragged onto an end would silently
//! stop doing anything - the same margin the MIDI page keeps.
constexpr static double BEND_T_MIN = 0.05;
constexpr static double BEND_T_MAX = 0.95;

static bool polylinePointIndexIsValid(const PolylinePlot* polyline, int pointIdx)
{
    IF_ASSERT_FAILED(polyline) {
        return false;
    }
    return pointIdx > -1 && pointIdx < static_cast<int>(polyline->points().size());
}

// Rescale between the common dynamic value range [PPPP, FFFF] and the full [0, 1] display range
// (the staff box), so those points fill the staff
static const muse::real_t DYNAMICS_DISPLAY_RANGE_MIN = mu::engraving::ORDINARY_DYNAMIC_VALUES.at(mu::engraving::DynamicType::PPPP);
static const muse::real_t DYNAMICS_DISPLAY_RANGE_MAX = mu::engraving::ORDINARY_DYNAMIC_VALUES.at(mu::engraving::DynamicType::FFFF);

// Must match muse::audio::VOLUME_DB_MIN/MAX
static constexpr double VOLUME_RANGE_MIN_DB = -60.0;
static constexpr double VOLUME_RANGE_MAX_DB = 12.0;

// Mirrors VolumeSlider.qml's fader curve, so dragging a point feels like moving the mixer fader -
// a plain linear map would put 0dB at 83% up the lane instead of the center
static constexpr double VOLUME_LOCAL_CENTER_DB = -24.0;
static constexpr double VOLUME_LOGICAL_CENTER_DB = -12.0;
static constexpr double VOLUME_HIGH_ACCURACY_STEP = 1.5;
static constexpr double VOLUME_LOW_ACCURACY_STEP = 0.75;

// logical (actual) dB -> local (linear-in-display) dB
static double volumeLogicalDbToLocalDb(double logicalDb)
{
    if (logicalDb > VOLUME_LOGICAL_CENTER_DB) {
        const double diff = VOLUME_RANGE_MAX_DB - logicalDb;
        return VOLUME_RANGE_MAX_DB - diff * VOLUME_HIGH_ACCURACY_STEP;
    }

    const double diff = VOLUME_LOGICAL_CENTER_DB - logicalDb;
    return VOLUME_LOCAL_CENTER_DB - diff * VOLUME_LOW_ACCURACY_STEP;
}

// local (linear-in-display) dB -> logical (actual) dB
static double volumeLocalDbToLogicalDb(double localDb)
{
    if (localDb > VOLUME_LOCAL_CENTER_DB) {
        const double diff = VOLUME_RANGE_MAX_DB - localDb;
        return VOLUME_RANGE_MAX_DB - diff / VOLUME_HIGH_ACCURACY_STEP;
    }

    const double diff = VOLUME_LOCAL_CENTER_DB - localDb;
    return VOLUME_LOGICAL_CENTER_DB - diff / VOLUME_LOW_ACCURACY_STEP;
}

// Mirrors the volume fader curve above, anchored on the default tempo instead of 0dB - a plain
// linear map would put the default tempo (120bpm) at ~12% up the lane instead of the center
// This is a UI-only display cap - the score model still allows tempos up to Constants::MAX_TEMPO
static constexpr double TEMPO_RANGE_MAX_BPM = 300.0;
static const double TEMPO_LOGICAL_CENTER_BPM = mu::engraving::Constants::DEFAULT_TEMPO.toBPM().val;
static const double TEMPO_LOCAL_CENTER_BPM = TEMPO_RANGE_MAX_BPM / 2.0;
static const double TEMPO_HIGH_ACCURACY_STEP = (TEMPO_RANGE_MAX_BPM - TEMPO_LOCAL_CENTER_BPM)
                                               / (TEMPO_RANGE_MAX_BPM - TEMPO_LOGICAL_CENTER_BPM);
static const double TEMPO_LOW_ACCURACY_STEP = TEMPO_LOCAL_CENTER_BPM / TEMPO_LOGICAL_CENTER_BPM;

// logical (actual) bpm -> local (linear-in-display) bpm
static double tempoLogicalBpmToLocalBpm(double logicalBpm)
{
    if (logicalBpm > TEMPO_LOGICAL_CENTER_BPM) {
        const double diff = TEMPO_RANGE_MAX_BPM - logicalBpm;
        return TEMPO_RANGE_MAX_BPM - diff * TEMPO_HIGH_ACCURACY_STEP;
    }

    const double diff = TEMPO_LOGICAL_CENTER_BPM - logicalBpm;
    return TEMPO_LOCAL_CENTER_BPM - diff * TEMPO_LOW_ACCURACY_STEP;
}

// local (linear-in-display) bpm -> logical (actual) bpm
static double tempoLocalBpmToLogicalBpm(double localBpm)
{
    if (localBpm > TEMPO_LOCAL_CENTER_BPM) {
        const double diff = TEMPO_RANGE_MAX_BPM - localBpm;
        return TEMPO_RANGE_MAX_BPM - diff / TEMPO_HIGH_ACCURACY_STEP;
    }

    const double diff = TEMPO_LOCAL_CENTER_BPM - localBpm;
    return TEMPO_LOGICAL_CENTER_BPM - diff / TEMPO_LOW_ACCURACY_STEP;
}

// Values are stored normalized [0, 1]; Dynamics rescales that into its own sub-range for display,
// Volume and Tempo additionally apply a fader curve, other types map 1:1 onto the display range
static double automationValueToDisplay(AutomationType type, muse::real_t value)
{
    if (type == AutomationType::Dynamics) {
        const double display = (value - DYNAMICS_DISPLAY_RANGE_MIN) / (DYNAMICS_DISPLAY_RANGE_MAX - DYNAMICS_DISPLAY_RANGE_MIN);
        return std::clamp(display, 0.0, 1.0);
    }

    if (type == AutomationType::Volume) {
        const double logicalDb = VOLUME_RANGE_MIN_DB + static_cast<double>(value) * (VOLUME_RANGE_MAX_DB - VOLUME_RANGE_MIN_DB);
        const double localDb = volumeLogicalDbToLocalDb(logicalDb);
        const double display = (localDb - VOLUME_RANGE_MIN_DB) / (VOLUME_RANGE_MAX_DB - VOLUME_RANGE_MIN_DB);
        return std::clamp(display, 0.0, 1.0);
    }

    if (type == AutomationType::Tempo) {
        const double logicalBpm = mu::engraving::denormalizeTempo(value).toBPM().val;
        const double localBpm = tempoLogicalBpmToLocalBpm(logicalBpm);
        const double display = localBpm / TEMPO_RANGE_MAX_BPM;
        return std::clamp(display, 0.0, 1.0);
    }

    return std::clamp(static_cast<double>(value), 0.0, 1.0);
}

// existingValue preserves Tempo values above the display cap when the drag lands back at the same clamped edge
static muse::real_t automationValueFromDisplay(AutomationType type, double displayValue,
                                               std::optional<muse::real_t> existingValue = std::nullopt)
{
    if (type == AutomationType::Dynamics) {
        return DYNAMICS_DISPLAY_RANGE_MIN + displayValue * (DYNAMICS_DISPLAY_RANGE_MAX - DYNAMICS_DISPLAY_RANGE_MIN);
    }

    if (type == AutomationType::Volume) {
        const double localDb = VOLUME_RANGE_MIN_DB + displayValue * (VOLUME_RANGE_MAX_DB - VOLUME_RANGE_MIN_DB);
        const double logicalDb = volumeLocalDbToLogicalDb(localDb);
        const double normalized = (logicalDb - VOLUME_RANGE_MIN_DB) / (VOLUME_RANGE_MAX_DB - VOLUME_RANGE_MIN_DB);
        return muse::real_t(normalized);
    }

    if (type == AutomationType::Tempo) {
        if (existingValue && muse::RealIsEqualOrMore(displayValue, 1.0)
            && muse::RealIsEqualOrMore(automationValueToDisplay(type, *existingValue), 1.0)) {
            return *existingValue;
        }

        const double localBpm = displayValue * TEMPO_RANGE_MAX_BPM;
        const double logicalBpm = tempoLocalBpmToLogicalBpm(localBpm);
        const muse::real_t normalized = mu::engraving::normalizeTempo(mu::engraving::BeatsPerSecond::fromBPM(logicalBpm));
        return std::clamp(normalized, mu::engraving::MIN_NORMALIZED_TEMPO, mu::engraving::MAX_NORMALIZED_TEMPO);
    }

    return muse::real_t(displayValue);
}

//! Where the bend handle of one segment sits, in the same display coordinates as the polyline's own
//! points: the ease's position along the segment horizontally, and the value it bends the segment to
//! vertically. With `Ease::none()` that is the middle of the chord - which is exactly where the line
//! already passes, so the handle starts out sitting on the curve instead of floating next to it.
//!
//! ⚠️ 这是**记谱页车道的曲率画法**：手柄位置由"值 → 显示"的同一个映射算出来
//! （`automationValueToDisplay`），与画点用的是同一套，所以手柄所在的高度就是那个弯折值的高度。
static QPointF bendHandlePoint(const QPointF& from, const QPointF& to, AutomationType type,
                               muse::real_t prevOutValue, muse::real_t arrivalValue,
                               const std::optional<mu::engraving::AutomationPoint::Ease>& bend)
{
    const double t = bend ? std::clamp(double(bend->t), 0.0, 1.0) : 0.5;
    const double fraction = bend ? std::clamp(double(bend->value), 0.0, 1.0) : 0.5;

    const double bendValue = double(prevOutValue) + fraction * (double(arrivalValue) - double(prevOutValue));

    return QPointF(from.x() + (to.x() - from.x()) * t,
                   1.0 - automationValueToDisplay(type, bendValue));
}

static const Segment* lastSegmentOfSystem(const System* system)
{
    const mu::engraving::SegmentType type = mu::engraving::SegmentType::Duration;
    const Segment* seg = system->firstMeasure() ? system->firstMeasure()->first(type) : nullptr;
    const Segment* last = nullptr;
    while (seg && seg->system() == system) {
        last = seg;
        seg = seg->next1(type);
    }
    return last;
}

// Maps an x position to a tick via linear interpolation between the nearest Duration/barline segments on either side of it
static std::optional<int> tickFromCanvasX(const System* system, const muse::RectF& staffCanvasRect, qreal x)
{
    IF_ASSERT_FAILED(system) {
        return std::nullopt;
    }

    const double pointCanvasX = staffCanvasRect.x() + x * staffCanvasRect.width();
    const mu::engraving::SegmentType type = mu::engraving::SegmentType::Duration | mu::engraving::SegmentType::BarLineTypes;

    const Segment* prevSeg = nullptr;
    const Segment* nextSeg = nullptr;
    for (const Segment* seg = system->firstMeasure() ? system->firstMeasure()->first(type) : nullptr;
         seg && seg->system() == system; seg = seg->next1(type)) {
        if (seg->canvasX() <= pointCanvasX) {
            prevSeg = seg;
        } else {
            nextSeg = seg;
            break;
        }
    }

    if (!prevSeg) {
        return nextSeg ? std::make_optional(nextSeg->tick().ticks()) : std::nullopt;
    }

    // No next segment - use prevSeg's own end as a virtual next point
    const double nextCanvasX = nextSeg ? nextSeg->canvasX() : prevSeg->canvasX() + prevSeg->width();
    const int nextTick = nextSeg ? nextSeg->tick().ticks() : prevSeg->tick().ticks() + prevSeg->ticks().ticks();
    const double canvasSpan = nextCanvasX - prevSeg->canvasX();
    const double ratio = canvasSpan > 0.0 ? (pointCanvasX - prevSeg->canvasX()) / canvasSpan : 0.0;

    return prevSeg->tick().ticks() + static_cast<int>(ratio * (nextTick - prevSeg->tick().ticks()));
}

static AutomationCurveKey curveKeyFor(AutomationType type, const Staff* staff)
{
    switch (type) {
    case AutomationType::Dynamics:
        return AutomationCurveKey::staff(type, staff->id());
    case AutomationType::Tempo:
        return AutomationCurveKey::global(type);
    case AutomationType::Volume:
    case AutomationType::Pan: {
        const Part* part = staff->part();
        const InstrumentTrackId trackId { part->id(), part->instrumentId() };
        return AutomationCurveKey::instrument(type, trackId);
    }
    case AutomationType::Unknown:
        break;
    }

    return AutomationCurveKey();
}

static staff_idx_t firstVisibleStaffIdx(const Score* score)
{
    for (staff_idx_t i = 0; i < score->nstaves(); ++i) {
        if (score->staff(i)->show()) {
            return i;
        }
    }

    return muse::nidx;
}

static bool isStructuralChange(const mu::engraving::ScoreChanges& changes)
{
    if (!changes.changedObjects.empty() && !changes.isValidBoundary()) {
        return true;
    }

    static const std::unordered_set<mu::engraving::ElementType> STRUCTURAL_TYPES {
        mu::engraving::ElementType::MEASURE,
        mu::engraving::ElementType::PART,
    };

    for (const mu::engraving::ElementType type : changes.changedTypes) {
        if (muse::contains(STRUCTURAL_TYPES, type)) {
            return true;
        }
    }

    return false;
}

static qreal defaultValueFor(AutomationType type)
{
    if (type == AutomationType::Pan) {
        return 0.5;
    }
    return 0.0;
}

NotationAutomationController::NotationAutomationController(QQuickItem* linesParent, const muse::modularity::ContextPtr& iocCtx)
    : muse::Contextable(iocCtx), m_linesParent(linesParent)
{
}

void NotationAutomationController::init()
{
    IF_ASSERT_FAILED(automation() && currentNotation()) {
        return;
    }

    onCurrentNotationChanged();

    automation()->automationModeEnabledChanged().onNotify(this, [this]() {
        if (automation()->isAutomationModeEnabled() && !m_pendingChanges.isEmpty()) {
            applyAutomationChanges(m_pendingChanges);
            m_pendingChanges.clear();
        } else {
            updatePolylinesGeometry();
        }
    }, Asyncable::Mode::SetReplace /* FIXME */);

    notationConfiguration()->currentAutomationTypeChanged().onNotify(this, [this]() {
        rebuildAllPolylines();
    }, Asyncable::Mode::SetReplace /* FIXME */);

    globalContext()->currentNotationChanged().onNotify(this, [this]() {
        onCurrentNotationChanged();
    }, Asyncable::Mode::SetReplace /* FIXME */);

    notationConfiguration()->scoreInversionChanged().onNotify(this, [this]() {
        updatePolylinesColors();
    }, Asyncable::Mode::SetReplace /* FIXME */);

    notationConfiguration()->isOnlyInvertInDarkThemeChanged().onNotify(this, [this]() {
        updatePolylinesColors();
    }, Asyncable::Mode::SetReplace /* FIXME */);

    uiConfiguration()->currentThemeChanged().onNotify(this, [this]() {
        updatePolylinesColors();
    }, Asyncable::Mode::SetReplace /* FIXME */);

    engravingConfiguration()->selectionColorChanged().onReceive(this, [this](voice_idx_t idx, const muse::draw::Color&) {
        // voice 1 color is used for the centre of selected points, "all voices color" is used for the area under lines
        if (idx == 0 || idx == mu::engraving::VOICES) {
            updatePolylinesColors();
        }
    }, Asyncable::Mode::SetReplace /* FIXME */);
}

NotationAutomationController::SysStaffToPolylinesMap NotationAutomationController::createPolylinesForSystem(const System* system)
{
    IF_ASSERT_FAILED(system && m_linesParent && score()) {
        return {};
    }

    SysStaffToPolylinesMap map;

    staff_idx_t staffIdx = system->firstVisibleStaff();
    while (staffIdx != muse::nidx) {
        PolylinePlot* polyline = createPolylineForStaff(system, staffIdx);
        if (polyline) {
            map.emplace(SysStaffKey(system, staffIdx), PolylinesSet({ polyline }));
        }
        staffIdx = system->nextVisibleStaff(staffIdx);
    }

    return map;
}

muse::uicomponents::PolylinePlot* NotationAutomationController::createPolylineForStaff(const System* system, staff_idx_t staffIdx)
{
    IF_ASSERT_FAILED(system && m_linesParent && score()) {
        return nullptr;
    }

    const Staff* staff = score()->staff(staffIdx);
    const SysStaff* sysStaff = system->staff(staffIdx);
    if (!staff || !sysStaff || !staff->isPrimaryStaff()) {
        return nullptr;
    }

    const AutomationCurveKey curveKey = curveKeyFor(currentAutomationType(), staff);
    if (curveKey.isGlobal() && staffIdx != firstVisibleStaffIdx(score())) {
        // Score-scoped automation is only drawn on the score's first staff
        return nullptr;
    }

    if (curveKey.trackId().has_value() && !staff->isTop()) {
        // Instrument-scoped automation is only drawn on the instrument's first staff
        return nullptr;
    }

    const int systemStartTick = system->first()->tick().ticks();
    const int systemEndTick = system->last()->endTick().ticks();

    const Measure* firstMeasure = system->firstMeasure();
    const Segment* firstSeg = firstMeasure ? firstMeasure->first(mu::engraving::SegmentType::Duration) : nullptr;
    const Segment* lastSeg = lastSegmentOfSystem(system);

    // TODO: Staves can have multiple polylines due to horizontal frames, at the moment we're
    // providing a single polyline over the entire staff...
    PolylinePlot* polyline = new PolylinePlot(m_linesParent);

    const muse::RectF staffCanvasRect = sysStaff->bbox().translated(system->canvasPos());
    const QVector<PointData> pointsData = pointsDataInStaff(staff, staffCanvasRect, systemStartTick, systemEndTick);

    const SysStaffKey key(system, staffIdx);
    m_pointsDataByStaff[key] = pointsData;

    //! NOTE: There can't be a 1-to-1 match between the number of points in the automation model and
    //! points on the polyline. A point with equal in/out values (i.e. a "BOTH" point) is represented
    //! as 1 polyline point, whereas a point with different in/out values will be represented with 2
    //! separate polyline points...
    QVector<QPointF> pointsForPolyline;
    pointsForPolyline.reserve(pointsData.size());
    for (const PointData& pointData : pointsData) {
        pointsForPolyline.emplace_back(pointData.qPointF);
    }
    polyline->setPoints(pointsForPolyline);
    //! ⚠️ 弯折（曲率）与点**必须一起**交给 polyline：两者是按索引对齐的，只给一半就画错了。
    polyline->setBends(bendsForPolyline(pointsData));

    applyPolylineStyle(polyline, key);
    polyline->setVisible(false);

    // Points can't be dragged past the system's first/last segment
    const qreal minX = firstSeg ? (firstSeg->canvasX() - staffCanvasRect.x()) / staffCanvasRect.width() : 0.0;
    const qreal maxX = lastSeg ? (lastSeg->canvasX() + lastSeg->width() - staffCanvasRect.x()) / staffCanvasRect.width() : 1.0;

    QObject::connect(polyline, &muse::uicomponents::PolylinePlot::pointMoved,
                     [this, key, polyline, minX, maxX](int pointIdx, qreal x, qreal y, bool completed) {
        IF_ASSERT_FAILED(polylinePointIndexIsValid(polyline, pointIdx)) {
            return;
        }

        const auto pointsDataIt = m_pointsDataByStaff.find(key);
        IF_ASSERT_FAILED(pointsDataIt != m_pointsDataByStaff.end() && pointIdx < pointsDataIt->second.size()) {
            return;
        }

        const PointData& oldPointData = pointsDataIt->second[pointIdx];
        const mu::engraving::AutomationPoint* automationPoint = automationPointAt(key, oldPointData.tick);
        const bool editRestricted = !automationPoint || automationPoint->generated || automationPoint->itemId.has_value();
        const qreal clampedX = editRestricted ? oldPointData.qPointF.x() : std::clamp(x, minX, maxX);

        const auto setPreviewPoint = [this, polyline, pointIdx, key](const QPointF& point) {
            QVector<QPointF> points = polyline->points();
            points.replace(pointIdx, point);
            polyline->setPoints(points);

            //! 段的两端动了，而弯折手柄在段里的**相对**位置（ease 的 t 与幅度）没变 ——
            //! 所以手柄的绝对位置要跟着重算。不重算的话，拖动过程中曲线会经过一个陈旧的
            //! 弯折点：画面与松手后的结果不一致（"看到的 ≠ 听到的"的拖动版）。
            const auto pointsDataIt = m_pointsDataByStaff.find(key);
            if (pointsDataIt != m_pointsDataByStaff.end()) {
                const QVector<PointData>& pointsData = pointsDataIt->second;
                QVector<QPointF> bends = bendsForPolyline(pointsData);

                if (pointIdx >= 1 && pointIdx < pointsData.size() && pointsData.at(pointIdx).hasBend) {
                    const Staff* staff = score() ? score()->staff(key.staffIdx) : nullptr;
                    if (const std::optional<QPointF> bend = bendHandleFor(staff, pointsData.at(pointIdx).tick,
                                                                         pointsData.at(pointIdx - 1).qPointF, point)) {
                        bends[pointIdx - 1] = *bend;
                    }
                }
                if (pointIdx + 1 < pointsData.size() && pointsData.at(pointIdx + 1).hasBend) {
                    const Staff* staff = score() ? score()->staff(key.staffIdx) : nullptr;
                    if (const std::optional<QPointF> bend = bendHandleFor(staff, pointsData.at(pointIdx + 1).tick, point,
                                                                         pointsData.at(pointIdx + 1).qPointF)) {
                        bends[pointIdx] = *bend;
                    }
                }

                polyline->setBends(bends);
            }

            applyPolylineColorsUnderLine(polyline, key);
            polyline->update(); // TODO: pass update rect?
        };

        if (completed) {
            //! ⚠️ 按下时在**折线上**新加的那个点，模型里**还不存在** —— 这时它不是"移动"而是"新建"。
            //! 直接走 `requestEditPoint()` 会因为找不到点被拒绝（`IF_ASSERT_FAILED(existingIt != curve.end())`
            //! 就是它，日志里那 19 次断言也是这个），用户看到的就是"按住曲线拖动没反应"。
            //! 与 MIDI 页"新建+拖动会丢点"是同一个病根：**一次手势里点还不存在就被当成已存在的点操作**。
            //! 这里按"模型里有没有"来分流：没有 → 在**松手的位置**新建一个点（一个命令）。
            if (!automationPointAt(key, oldPointData.tick)) {
                requestAddPoint(key, clampedX, y);
                return;
            }

            if (!requestEditPoint(oldPointData, key, clampedX, y)) {
                // Edit was rejected - snap the point back to where it actually is instead of
                // leaving the live-drag preview stuck at the rejected position
                setPreviewPoint(oldPointData.qPointF);
            }
            return;
        }

        // Live drag preview
        setPreviewPoint({ clampedX, y });
    });

    QObject::connect(polyline, &muse::uicomponents::PolylinePlot::pointAdded,
                     [this, key, polyline, system, staffCanvasRect](qreal x, qreal y, bool completed) {
        if (completed) {
            requestAddPoint(key, x, y);
            return;
        }

        const std::optional<int> tick = tickFromCanvasX(system, staffCanvasRect, x);
        if (!tick) {
            return;
        }

        QVector<PointData>& pointsData = m_pointsDataByStaff[key];
        int insertIdx = 0;
        while (insertIdx < pointsData.size() && pointsData.at(insertIdx).tick < *tick) {
            ++insertIdx;
        }
        pointsData.insert(insertIdx, PointData(-1, *tick, { x, y }, PointData::PointType::BOTH));

        QVector<QPointF> points = polyline->points();
        points.insert(insertIdx, { x, y });
        polyline->setPoints(points);

        //! 按下落在**线上**，所以这个新顶点就在它切开的那一段上：原来属于该段的弯折（记在下一个
        //! 点上）现在描述的这一段两端变了，手柄位置要跟着重算。这是一次手势内的预览 ——
        //! 松手时的模型写入会按曲线把这一段整个重建。
        QVector<QPointF> bends = bendsForPolyline(pointsData);
        if (insertIdx + 1 < pointsData.size() && pointsData.at(insertIdx + 1).hasBend) {
            const Staff* staff = score() ? score()->staff(key.staffIdx) : nullptr;
            if (const std::optional<QPointF> bend = bendHandleFor(staff, pointsData.at(insertIdx + 1).tick,
                                                                 pointsData.at(insertIdx).qPointF,
                                                                 pointsData.at(insertIdx + 1).qPointF)) {
                bends[insertIdx] = *bend;
            }
        }
        //! 新顶点自己带不了弯折：它还没有进入模型，形状也未定 —— 这时给它一个抓手，
        //! 只会让人以为功能坏了。松手写入之后它就有了。
        polyline->setBends(bends);
        applyPolylineColorsUnderLine(polyline, key);
    });

    QObject::connect(polyline, &muse::uicomponents::PolylinePlot::bendMoved,
                     [this, key, polyline](int segmentIdx, qreal x, qreal y, bool completed) {
        const auto pointsDataIt = m_pointsDataByStaff.find(key);
        IF_ASSERT_FAILED(pointsDataIt != m_pointsDataByStaff.end()) {
            return;
        }
        QVector<PointData>& pointsData = pointsDataIt->second;
        if (segmentIdx < 0 || segmentIdx + 1 >= pointsData.size()) {
            return;
        }

        PointData& arrival = pointsData[segmentIdx + 1];
        if (!arrival.hasBend) {
            // A flat segment has nothing to bend, and a vertical jump is not a segment in time
            return;
        }

        const QPointF from = pointsData.at(segmentIdx).qPointF;
        const QPointF to = arrival.qPointF;

        //! 预览与写入用**同一套夹取**，否则松手时手柄会跳一下：
        //! 横向留在段内（两端各留 5%，理由见 BEND_T_MIN），纵向留在这段值的范围内
        //! （`Ease::value` 的含义就是"落在起止值之间的比例"）。
        const qreal minX = from.x() + (to.x() - from.x()) * BEND_T_MIN;
        const qreal maxX = from.x() + (to.x() - from.x()) * BEND_T_MAX;
        const qreal minY = std::min(from.y(), to.y());
        const qreal maxY = std::max(from.y(), to.y());

        const QPointF preview(std::clamp(x, minX, maxX), std::clamp(y, minY, maxY));

        //! 拖动中只把预览写进**显示数据**（跟手），模型一个字都不写 —— 松手才提交一次。
        arrival.bendPointF = preview;
        polyline->setBends(bendsForPolyline(pointsData));

        if (!completed) {
            return;
        }

        const int fromTick = pointsData.at(segmentIdx).tick;
        if (!requestEditPointEase(arrival, from, key, preview.x(), preview.y())) {
            // 什么都没写（手柄回到原地、或写入被拒）：立刻按模型重画这一段，
            // 免得画面停在一个曲线并不存在的位置上（§"松手后立刻按真实数据重画"）。
            updateStaffPointsInRange(key, fromTick, arrival.tick);
        }
    });

    QObject::connect(polyline, &muse::uicomponents::PolylinePlot::pointRemoved,
                     [this, key](int pointIdx, bool completed) {
        if (!completed) {
            return;
        }
        const auto pointsDataIt = m_pointsDataByStaff.find(key);
        IF_ASSERT_FAILED(pointsDataIt != m_pointsDataByStaff.end() && pointIdx >= 0 && pointIdx < pointsDataIt->second.size()) {
            return;
        }
        requestRemovePoint(pointsDataIt->second.at(pointIdx), key);
    });

    return polyline;
}

QVector<NotationAutomationController::PointData> NotationAutomationController::pointsDataInStaff(const mu::engraving::Staff* staff,
                                                                                                 const muse::RectF& sysStaffCanvasRect,
                                                                                                 int startTick, int endTick) const
{
    QVector<PointData> points;
    IF_ASSERT_FAILED(staff && score() && automationData()) {
        return points;
    }

    const AutomationType type = currentAutomationType();

    int currentPointIndex = 0;
    const mu::engraving::AutomationCurveKey key = curveKeyFor(type, staff);
    const mu::engraving::AutomationCurve& curve = automationData()->curve(key);

    // Start at the first point >= startTick rather than curve.begin() - resolvedInValue() only ever
    // looks backward via std::prev(it), which works on any valid iterator, not just one reached by
    // walking from the beginning
    for (auto it = curve.lower_bound(startTick); it != curve.end(); ++it) {
        const int tick = it->first;
        const bool isPastEnd = tick > endTick;

        const Fraction frac = Fraction::fromTicks(tick);
        const Segment* seg = score()->tick2leftSegmentMM(frac);
        if (!seg) { //! FIXME: fix automation curve on measure repeats
            continue;
        }

        // The point's tick may not exactly match that of a segment. For this reason we can only calculate the x position
        // of our points based on a "tickRatio". This ratio is based on the "tick difference" between the point tick and
        // the segment's tick, and the duration of the segment (in ticks)...
        const int tickDiff = tick - seg->tick().ticks();
        const double tickRatio = static_cast<double>(tickDiff) / seg->ticks().ticks();
        const double pointXInSeg = tickRatio * seg->width(); // The point's x relative to the segment

        const double segXInStaff = seg->canvasX() - sysStaffCanvasRect.x(); // The segment's x relative to the staff
        const double pointXInStaff = (segXInStaff + pointXInSeg) / sysStaffCanvasRect.width();

        // Point in/out values are rescaled to the display range - higher value == lower Y...
        const mu::engraving::AutomationPoint& autoPoint = it->second;
        const mu::engraving::real_t resolvedIn = mu::engraving::resolveInValue(curve, it);

        //! 曲率：到达本点的这一段能不能弯、弯折手柄在哪。判据与画法都在 `bendHandleFor()` 里 ——
        //! 与 MIDI 页、与这里的拖动预览共用同一份（三处算法不一致，就会出现"拖的时候一个样、
        //! 松手另一个样"这种最难查的偏差）。
        //! ⚠️ 折线上还没有上一个顶点时（本范围内的第一个点）算不出这一段，这就是"不能弯"；
        //! `updateStaffPointsInRange()` 会用范围外保留下来的那个顶点补上（见那里的说明）。
        const bool hasPrevVertex = !points.isEmpty();
        const QPointF prevVertex = hasPrevVertex ? points.last().qPointF : QPointF();

        const auto applyBend = [&](PointData& pointData, const QPointF& toPointF) {
            if (!hasPrevVertex) {
                return;
            }
            if (const std::optional<QPointF> bend = bendHandleFor(staff, tick, prevVertex, toPointF)) {
                pointData.hasBend = true;
                pointData.bendPointF = *bend;
            }
        };

        if (resolvedIn == autoPoint.value.outValue) {
            const QPointF qpf(pointXInStaff, 1.0 - automationValueToDisplay(type, resolvedIn));
            PointData pointData(currentPointIndex++, tick, qpf, PointData::PointType::BOTH);
            applyBend(pointData, qpf);
            points.emplace_back(pointData);
        } else {
            const QPointF qpfIn(pointXInStaff, 1.0 - automationValueToDisplay(type, resolvedIn));
            PointData inPointData(currentPointIndex++, tick, qpfIn, PointData::PointType::IN);
            applyBend(inPointData, qpfIn);
            points.emplace_back(inPointData);

            const QPointF qpfOut(pointXInStaff, 1.0 - automationValueToDisplay(type, autoPoint.value.outValue));
            points.emplace_back(PointData(currentPointIndex++, tick, qpfOut, PointData::PointType::OUT));
        }

        if (isPastEnd) {
            // Included one point past the range - its own resolved value may depend on the outValue
            // of the last in-range point, which could have just changed
            break;
        }
    }

    return points;
}

void NotationAutomationController::applyPolylineStyle(PolylinePlot* polyline, const SysStaffKey& key) const
{
    IF_ASSERT_FAILED(polyline) {
        return;
    }

    polyline->setLineWidth(POLYLINE_LINE_WIDTH);
    polyline->setDrawBackground(false);

    polyline->setBaselineN(defaultValueFor(currentAutomationType()));

    polyline->setGhostPointsEnabled(false);
    polyline->setSelectedPointsEnabled(true);

    //! 曲率：车道给出"有坡度的段"的弯折手柄，拖它调曲率（写上游的 `AutomationPoint::Ease`）。
    //! 与 MIDI 页同一套手势、同一份语义 —— 两页编辑的是同一条曲线，只是画在两处。
    polyline->setBendHandlesEnabled(true);

    PolylinePointStyle* standard = polyline->standardPointStyle();
    standard->setCenterRadius(POLYLINE_STANDARD_CENTER_RADIUS);
    standard->setOutlineWidth(POLYLINE_LINE_WIDTH);

    standard->setCenterRadiusHovered(POLYLINE_HOVERED_CENTER_RADIUS);
    standard->setOutlineWidthHovered(POLYLINE_LINE_WIDTH);

    PolylinePointStyle* selected = polyline->selectedPointStyle();
    selected->setCenterRadius(POLYLINE_SELECTED_CENTER_RADIUS);
    selected->setMiddleRingWidth(POLYLINE_SELECTED_MIDDLE_RING_WIDTH);
    selected->setOutlineWidth(POLYLINE_LINE_WIDTH);

    selected->setCenterRadiusHovered(POLYLINE_SELECTED_CENTER_RADIUS);
    selected->setMiddleRingWidthHovered(POLYLINE_SELECTED_MIDDLE_RING_WIDTH);
    selected->setOutlineWidthHovered(POLYLINE_LINE_WIDTH);

    applyPolylineColors(polyline, key);
}

void NotationAutomationController::applyPolylineColors(PolylinePlot* polyline, const SysStaffKey& key) const
{
    IF_ASSERT_FAILED(polyline) {
        return;
    }

    const QColor lineColor = inversionRelativeColor(muse::ui::FONT_PRIMARY_COLOR);
    polyline->setLineColor(lineColor);

    const QColor foregroundColor = notationConfiguration()->foregroundColor();

    PolylinePointStyle* standard = polyline->standardPointStyle();
    standard->setCenterColor(foregroundColor);
    standard->setOutlineColor(lineColor);

    standard->setCenterColorHovered(inversionRelativeColor(muse::ui::BUTTON_COLOR));
    standard->setOutlineColorHovered(lineColor);

    QColor selectionColor = engravingConfiguration()->selectionColor().toQColor();

    PolylinePointStyle* selected = polyline->selectedPointStyle();
    selected->setCenterColor(selectionColor);
    selected->setMiddleRingColor(foregroundColor);
    selected->setOutlineColor(lineColor);

    selectionColor.setAlpha(POLYLINE_SELECTED_HOVERED_ALPHA);
    selected->setCenterColorHovered(selectionColor);
    selected->setMiddleRingColorHovered(foregroundColor);
    selected->setOutlineColorHovered(lineColor);

    applyPolylineColorsUnderLine(polyline, key);
}

void NotationAutomationController::applyPolylineColorsUnderLine(PolylinePlot* polyline, const SysStaffKey& key) const
{
    IF_ASSERT_FAILED(polyline) {
        return;
    }

    const auto pointsDataIt = m_pointsDataByStaff.find(key);
    IF_ASSERT_FAILED(pointsDataIt != m_pointsDataByStaff.end()) {
        return;
    }

    // TODO: Cache these colors?
    const QColor allVoicesColor = engravingConfiguration()->selectionColor(mu::engraving::VOICES).toQColor();

    QColor generatedColor = allVoicesColor;
    generatedColor.setAlpha(POLYLINE_GENERATED_AREA_ALPHA);

    QColor editedColor = allVoicesColor;
    editedColor.setAlpha(POLYLINE_EDITED_AREA_ALPHA);

    const QVector<PointData>& pointsData = pointsDataIt->second;

    QVector<QColor> colorsUnderLine;
    colorsUnderLine.reserve(pointsData.size() + 1); // +1 for the "trailing color" (see below)

    bool prevPointGenerated = true;
    for (const PointData& pointData : pointsData) {
        //! NOTE: The following can be null for newly created (always non-generated) points because they're not in the model yet
        const mu::engraving::AutomationPoint* automationPoint = automationPointAt(key, pointData.tick);
        const bool currPointGenerated = automationPoint && automationPoint->generated;

        // Colors either side of an edited point should use the "edited color"...
        const bool useEditedColor = !prevPointGenerated || !currPointGenerated;
        colorsUnderLine.emplace_back(useEditedColor ? editedColor : generatedColor);

        prevPointGenerated = currPointGenerated;
    }

    // This is the trailing color (after the last point) - it always follows the color of the last point...
    colorsUnderLine.emplace_back(prevPointGenerated ? generatedColor : editedColor);

    polyline->setColorsUnderLine(colorsUnderLine);
}

QColor NotationAutomationController::inversionRelativeColor(const muse::ui::ThemeStyleKey& key) const
{
    // This method is necessary because automation colors are relative to the score inversion as opposed to the current UI theme. In an
    // inverted score we use dark theme colors, and in a non-inverted score we use light theme colors...

    // TODO: High contrast colors should actually be fully customizable (issue #34154)
    const bool isHighContrast = uiConfiguration()->isHighContrast();
    const muse::ui::ThemeCode lightTheme = isHighContrast ? muse::ui::HIGH_CONTRAST_WHITE_THEME_CODE : muse::ui::LIGHT_THEME_CODE;
    const muse::ui::ThemeCode darkTheme = isHighContrast ? muse::ui::HIGH_CONTRAST_BLACK_THEME_CODE : muse::ui::DARK_THEME_CODE;

    const bool inverted = notationConfiguration()->shouldInvertScore();

    const muse::ui::ThemeList& themes = uiConfiguration()->themes();
    for (const muse::ui::ThemeInfo& theme : themes) {
        // Set line colors based on score inversion as opposed to current UI themes...
        const bool foundLightTheme = !inverted && theme.codeKey == lightTheme;
        const bool foundDarkTheme = inverted && theme.codeKey == darkTheme;
        if (foundLightTheme || foundDarkTheme) {
            return theme.values[key].toString();
        }
    }

    ASSERT_X("Error scanning themes");

    return QColor();
}

void NotationAutomationController::updatePolylinesGeometry()
{
    const bool visible = automation() && automation()->isAutomationModeEnabled();

    for (const auto& [key, polylines] : m_stavesToLinesMap) {
        IF_ASSERT_FAILED(key.isValid() && !polylines.empty()) {
            continue;
        }

        // TODO: Staves can have multiple polylines due to horizontal frames, at the moment we're
        // providing a single polyline over the entire staff...
        PolylinePlot* polyline = *polylines.begin();
        polyline->setVisible(visible);
        if (!visible) {
            continue;
        }

        const SysStaff* sysStaff = key.system->staff(key.staffIdx);
        IF_ASSERT_FAILED(sysStaff) {
            continue;
        }

        //! NOTE: Here we should only update properties of the polyline that change relative to the view matrix. Polyline points are
        //! placed relative to the polylines themselves, and thus do not need to be modified in here...
        muse::RectF staffCanvasRect = sysStaff->bbox().translated(key.system->canvasPos());
        staffCanvasRect = m_viewMatrix.map(staffCanvasRect);

        polyline->setWidth(staffCanvasRect.width());
        polyline->setHeight(staffCanvasRect.height());
        polyline->setX(staffCanvasRect.x());
        polyline->setY(staffCanvasRect.y());

        applyPolylineColors(polyline, key);
    }
}

void NotationAutomationController::updatePolylinesColors()
{
    for (const auto& [key, polylines] : m_stavesToLinesMap) {
        IF_ASSERT_FAILED(key.isValid() && !polylines.empty()) {
            continue;
        }
        // TODO: Staves can have multiple polylines due to horizontal frames, at the moment we're
        // providing a single polyline over the entire staff...
        PolylinePlot* polyline = *polylines.begin();
        applyPolylineColors(polyline, key);
    }
}

void NotationAutomationController::setViewMatrix(const muse::draw::Transform& viewMatrix)
{
    if (viewMatrix == m_viewMatrix) {
        return;
    }
    m_viewMatrix = viewMatrix;

    if (automation() && automation()->isAutomationModeEnabled()) {
        updatePolylinesGeometry();
    }
}

void NotationAutomationController::onCurrentNotationChanged()
{
    m_pendingChanges.clear();
    m_pendingScoreState = PendingScoreState();
    rebuildAllPolylines();

    if (automationData()) {
        automationData()->changed().onReceive(this, [this](const mu::engraving::AutomationChanges& changes) {
            mergePendingChanges(changes);
            scheduleUpdate();
        }, Asyncable::Mode::SetReplace /* FIXME */);
    }

    if (score()) {
        score()->changesChannel().onReceive(this, [this](const mu::engraving::ScoreChanges& changes) {
            mergePendingScoreChanges(changes);
            scheduleUpdate();
        }, Asyncable::Mode::SetReplace /* FIXME */);
    }
}

void NotationAutomationController::mergePendingScoreChanges(const mu::engraving::ScoreChanges& changes)
{
    const bool firstChange = !m_pendingScoreState.hasChanges;
    m_pendingScoreState.hasChanges = true;
    m_pendingScoreState.structural = m_pendingScoreState.structural || isStructuralChange(changes);

    if (!changes.isValidBoundary()) {
        m_pendingScoreState.boundary = std::nullopt;
        return;
    }
    if (!firstChange && !m_pendingScoreState.boundary) {
        return;
    }

    const TickStaffRange changeRange { changes.tickFrom, changes.tickTo, changes.staffIdxFrom, changes.staffIdxTo };
    TickStaffRange range = m_pendingScoreState.boundary.value_or(changeRange);
    range.tickFrom = std::min(range.tickFrom, changeRange.tickFrom);
    range.tickTo = std::max(range.tickTo, changeRange.tickTo);
    range.staffIdxFrom = std::min(range.staffIdxFrom, changeRange.staffIdxFrom);
    range.staffIdxTo = std::max(range.staffIdxTo, changeRange.staffIdxTo);
    m_pendingScoreState.boundary = range;
}

void NotationAutomationController::scheduleUpdate()
{
    if (m_updateScheduled) {
        return;
    }
    m_updateScheduled = true;

    muse::async::Async::call(this, [this]() {
        m_updateScheduled = false;
        processPendingChanges();
    });
}

void NotationAutomationController::processPendingChanges()
{
    if (!m_pendingScoreState.hasChanges && m_pendingChanges.isEmpty()) {
        return;
    }
    const PendingScoreState scoreState = m_pendingScoreState;
    m_pendingScoreState = PendingScoreState();

    const bool automationVisible = automation() && automation()->isAutomationModeEnabled();

    if (scoreState.structural) {
        if (!automationVisible) {
            // Nothing visible right now; defer the rebuild until automation mode is enabled again
            m_pendingChanges.isFullReset = true;
            return;
        }
        rebuildAllPolylines();
        m_pendingChanges.clear();
        return;
    }

    if (!automationVisible) {
        // Nothing visible right now; m_pendingChanges keeps accumulating for next time
        return;
    }

    if (!m_pendingChanges.isEmpty()) {
        applyAutomationChanges(m_pendingChanges);
        m_pendingChanges.clear();
        return;
    }

    // No automation-data change and nothing structural - just layout drift
    // (e.g. measure widths shifted); refresh point positions using the batch's own range
    for (const auto& [key, polylines] : m_stavesToLinesMap) {
        IF_ASSERT_FAILED(key.isValid()) {
            continue;
        }
        const Staff* staff = score()->staff(key.staffIdx);
        if (!staff) {
            continue;
        }

        const int systemStartTick = key.system->first()->tick().ticks();
        const int systemEndTick = key.system->last()->endTick().ticks();
        if (scoreState.boundary) {
            const TickStaffRange& range = *scoreState.boundary;
            if (staff->idx() < range.staffIdxFrom || staff->idx() > range.staffIdxTo) {
                continue;
            }
            if (systemEndTick < range.tickFrom || systemStartTick > range.tickTo) {
                continue;
            }
        }

        updateStaffPointsInRange(key, systemStartTick, systemEndTick);
    }

    updatePolylinesGeometry();
}

void NotationAutomationController::rebuildAllPolylines()
{
    // TODO: More efficient if we don't clear/recreate the polylines every time...
    for (const auto& [staff, polylines] : m_stavesToLinesMap) {
        for (PolylinePlot* polyline : polylines) {
            delete polyline;
        }
    }
    m_stavesToLinesMap.clear();
    m_pointsDataByStaff.clear();

    if (!score()) {
        // Happens on close...
        return;
    }

    for (const System* system : score()->systems()) {
        m_stavesToLinesMap.merge(createPolylinesForSystem(system));
    }

    updatePolylinesGeometry();
}

void NotationAutomationController::updateStaffPointsInRange(const SysStaffKey& key, int tickFrom, int tickTo)
{
    auto mapIt = m_stavesToLinesMap.find(key);
    IF_ASSERT_FAILED(key.isValid() && mapIt != m_stavesToLinesMap.end() && !mapIt->second.empty()) {
        return;
    }
    PolylinePlot* polyline = *mapIt->second.begin();

    const Staff* staff = score() ? score()->staff(key.staffIdx) : nullptr;
    const SysStaff* sysStaff = key.system ? key.system->staff(key.staffIdx) : nullptr;
    IF_ASSERT_FAILED(staff && sysStaff) {
        return;
    }

    const muse::RectF staffCanvasRect = sysStaff->bbox().translated(key.system->canvasPos());
    const QVector<PointData> newRangeData = pointsDataInStaff(staff, staffCanvasRect, tickFrom, tickTo);

    QVector<PointData>& pointsData = m_pointsDataByStaff[key];

    int firstIdx = 0;
    while (firstIdx < pointsData.size() && pointsData.at(firstIdx).tick < tickFrom) {
        ++firstIdx;
    }
    int lastIdx = firstIdx;
    while (lastIdx < pointsData.size() && pointsData.at(lastIdx).tick <= tickTo) {
        ++lastIdx;
    }
    if (!newRangeData.isEmpty() && newRangeData.back().tick > tickTo) {
        const int trailingTick = newRangeData.back().tick;
        while (lastIdx < pointsData.size() && pointsData.at(lastIdx).tick == trailingTick) {
            ++lastIdx;
        }
    }

    QVector<PointData> updatedPointsData;
    updatedPointsData.reserve(pointsData.size() - (lastIdx - firstIdx) + newRangeData.size());
    for (int i = 0; i < firstIdx; ++i) {
        updatedPointsData.push_back(pointsData.at(i));
    }
    for (const PointData& pointData : newRangeData) {
        updatedPointsData.push_back(pointData);
    }
    for (int i = lastIdx; i < pointsData.size(); ++i) {
        updatedPointsData.push_back(pointsData.at(i));
    }
    pointsData = updatedPointsData;

    //! ⚠️ 局部刷新算不出**范围内第一个点**的到达段：那一段的起点在范围之外，是被保留下来的那个顶点。
    //! 不补这一步，范围内任何一次编辑都会让第一个点的弯折手柄消失（要等下一次整表重建才回来）。
    if (firstIdx > 0 && firstIdx < pointsData.size() && pointsData.at(firstIdx).tick != pointsData.at(firstIdx - 1).tick) {
        PointData& first = pointsData[firstIdx];
        const QPointF prevVertex = pointsData.at(firstIdx - 1).qPointF;
        if (const std::optional<QPointF> bend = bendHandleFor(staff, first.tick, prevVertex, first.qPointF)) {
            first.hasBend = true;
            first.bendPointF = *bend;
        } else {
            first.hasBend = false;
            first.bendPointF = QPointF();
        }
    }

    QVector<QPointF> points;
    points.reserve(pointsData.size());
    for (const PointData& pointData : pointsData) {
        points.push_back(pointData.qPointF);
    }
    polyline->setPoints(points);
    polyline->setBends(bendsForPolyline(pointsData));
    applyPolylineColorsUnderLine(polyline, key);
    polyline->update();
}

QVector<QPointF> NotationAutomationController::bendsForPolyline(const QVector<PointData>& pointsData)
{
    if (pointsData.size() < 2) {
        return {};
    }

    //! 一条折线有 n-1 段，而"这一段能不能弯"记在段的**到达点**上，所以弯折表比点表短一个。
    QVector<QPointF> bends;
    bends.reserve(pointsData.size() - 1);
    for (int i = 1; i < pointsData.size(); ++i) {
        const PointData& arrival = pointsData.at(i);
        bends.push_back(arrival.hasBend ? arrival.bendPointF : PolylinePlot::noBend());
    }

    return bends;
}

std::optional<QPointF> NotationAutomationController::bendHandleFor(const mu::engraving::Staff* staff, int tick,
                                                                   const QPointF& fromPointF, const QPointF& toPointF) const
{
    const AutomationDataConstPtr data = automationData();
    if (!staff || !data) {
        return std::nullopt;
    }

    const AutomationCurveKey curveKey = curveKeyFor(currentAutomationType(), staff);
    const AutomationCurve& curve = data->curve(curveKey);

    //! ⚠️ 前一个点要取**曲线上**的前一个点，不是折线上的前一个顶点：折线上的前一个顶点可能是同一个
    //! 点的 `OUT`（被拆成 IN/OUT 的点），那样算出来的是那个"竖跳"，不是这一段。
    const auto it = curve.find(tick);
    if (it == curve.end() || it == curve.begin()) {
        return std::nullopt;
    }

    const auto prevIt = std::prev(it);
    const muse::real_t arrivalValue = mu::engraving::resolveInValue(curve, it);
    if (muse::RealIsEqual(prevIt->second.value.outValue, arrivalValue)) {
        //! 平的段没有弯折可言：`Ease::value` 是"落在起止值之间的比例"，起止值相同就没有比例可言，
        //! 拖了也不会弯。给它一个抓手只会让人以为功能坏了。
        return std::nullopt;
    }

    return bendHandlePoint(fromPointF, toPointF, currentAutomationType(), prevIt->second.value.outValue,
                           arrivalValue, mu::engraving::ease(it->second));
}

void NotationAutomationController::mergePendingChanges(const mu::engraving::AutomationChanges& changes)
{
    if (changes.isFullReset) {
        m_pendingChanges.isFullReset = true;
        return;
    }
    for (const mu::engraving::AutomationCurveKey& key : changes.affectedKeys) {
        m_pendingChanges.extend(key, changes.tickFrom, changes.tickTo);
    }
}

void NotationAutomationController::applyAutomationChanges(const mu::engraving::AutomationChanges& changes)
{
    if (changes.isFullReset || !score()) {
        rebuildAllPolylines();
        return;
    }

    std::set<muse::ID> affectedStaffIds;
    std::set<mu::engraving::InstrumentTrackId> affectedTrackIds;
    bool globalAffected = false;
    for (const mu::engraving::AutomationCurveKey& key : changes.affectedKeys) {
        if (const std::optional<muse::ID> staffId = key.staffId()) {
            affectedStaffIds.insert(*staffId);
        } else if (const std::optional<mu::engraving::InstrumentTrackId> trackId = key.trackId()) {
            affectedTrackIds.insert(*trackId);
        } else if (key.isGlobal()) {
            globalAffected = true;
        }
    }

    // Only touch the staves that were actually affected and whose system overlaps the changed tick
    // range, and only recompute points within that range, rather than the whole score or even the
    // whole staff
    for (const auto& [key, polylines] : m_stavesToLinesMap) {
        IF_ASSERT_FAILED(key.isValid()) {
            continue;
        }
        const Staff* staff = score()->staff(key.staffIdx);
        if (!staff) {
            continue;
        }
        const bool staffAffected = affectedStaffIds.find(staff->id()) != affectedStaffIds.end();
        const mu::engraving::InstrumentTrackId staffTrackId { staff->part()->id(), staff->part()->instrumentId() };
        const bool trackAffected = affectedTrackIds.find(staffTrackId) != affectedTrackIds.end();
        const bool globalCurveAffected = globalAffected && curveKeyFor(currentAutomationType(), staff).isGlobal();
        if (!staffAffected && !trackAffected && !globalCurveAffected) {
            continue;
        }
        const System* system = key.system;
        const int systemStartTick = system->first()->tick().ticks();
        const int systemEndTick = system->last()->endTick().ticks();
        if (systemEndTick >= changes.tickFrom && systemStartTick <= changes.tickTo) {
            updateStaffPointsInRange(key, changes.tickFrom, changes.tickTo);
        }
    }

    updatePolylinesGeometry();
}

bool NotationAutomationController::requestEditPoint(const PointData& oldPointData, const SysStaffKey& key, qreal x, qreal y)
{
    // STEP 1 - Check that all of our parameters are valid...
    const PointData::PointType pointType = oldPointData.pointType;
    IF_ASSERT_FAILED(key.isValid() && pointType != PointData::PointType::UNKNOWN) {
        return false;
    }
    const System* system = key.system;
    const SysStaff* sysStaff = system ? system->staff(key.staffIdx) : nullptr;
    const Staff* staff = score() ? score()->staff(key.staffIdx) : nullptr;
    IF_ASSERT_FAILED(sysStaff && staff) {
        return false;
    }

    // STEP 2 - Determine the new tick value based on the x parameter...
    const muse::RectF staffCanvasRect = sysStaff->bbox().translated(system->canvasPos());
    const std::optional<int> newTickOpt = tickFromCanvasX(system, staffCanvasRect, x);
    const int newTick = newTickOpt.value_or(oldPointData.tick);
    const bool tickChanged = newTick != oldPointData.tick;

    // STEP 3 - Fetch the point being edited...
    const mu::engraving::AutomationCurveKey curveKey = curveKeyFor(currentAutomationType(), staff);

    const mu::engraving::AutomationCurve& curve = automationData()->curve(curveKey);
    const auto existingIt = curve.find(oldPointData.tick);
    IF_ASSERT_FAILED(existingIt != curve.end()) {
        return false;
    }
    const mu::engraving::AutomationPoint& existingPoint = existingIt->second;
    const mu::engraving::real_t existingInValue = mu::engraving::resolveInValue(curve, existingIt);

    //! NOTE: Point in/out values are rescaled to the display range - higher value == lower Y...
    const mu::engraving::real_t referenceValue = pointType == PointData::PointType::IN ? existingInValue : existingPoint.value.outValue;
    const mu::engraving::real_t newValue = automationValueFromDisplay(currentAutomationType(), 1.0 - y, referenceValue);

    // STEP 4 - Update the point's value, and move it to the new tick if necessary...

    //! NOTE: Moving a BOTH point is the simplest case - we can update its value then simply change the tick.
    //! Moving IN/OUT points is slightly more complex. In this case we need to set the in/out values to be
    //! equal at oldTick (effectively converting the original point to a BOTH point) and create a new point
    //! at newTick...

    if (!tickChanged || pointType == PointData::PointType::BOTH) {
        mu::engraving::AutomationPoint editedPoint = existingPoint;
        const mu::engraving::AutomationPoint::Ease preservedEase
            = mu::engraving::ease(editedPoint).value_or(mu::engraving::AutomationPoint::Ease::none());
        if (pointType == PointData::PointType::IN) {
            // The user explicitly chose this arrival value; it no longer follows whatever precedes it
            editedPoint.value.inValue = mu::engraving::AutomationPoint::ExplicitArrival { newValue, preservedEase };
        } else if (pointType == PointData::PointType::BOTH) {
            editedPoint.value.outValue = newValue;
            editedPoint.value.inValue = mu::engraving::AutomationPoint::ExplicitArrival { editedPoint.value.outValue, preservedEase };
        } else {
            editedPoint.value.outValue = newValue;
        }
        editedPoint.generated = false;

        mu::engraving::AutomationPointEdits edits {
            { newTick, MovePoint { editedPoint, oldPointData.tick } }
        };

        editAutomationPoints(curveKey, edits);

        return true;
    }

    // oldTick becomes a flat BOTH point, so its inValue is frozen to outValue at edit time (it no
    // longer live-tracks outValue if it's edited again later)
    const mu::engraving::AutomationPoint::Ease originalEase
        = mu::engraving::ease(existingPoint).value_or(mu::engraving::AutomationPoint::Ease::none());

    mu::engraving::AutomationPoint updatedOldPoint = existingPoint;
    if (pointType == PointData::PointType::OUT) {
        updatedOldPoint.value.outValue = existingInValue;
    }
    updatedOldPoint.value.inValue = mu::engraving::AutomationPoint::ExplicitArrival { updatedOldPoint.value.outValue, originalEase };
    updatedOldPoint.generated = false;

    mu::engraving::AutomationPoint newPoint;
    newPoint.value.outValue = newValue;
    newPoint.value.inValue = mu::engraving::AutomationPoint::ExplicitArrival { newPoint.value.outValue, originalEase };
    newPoint.itemId = existingPoint.itemId;

    mu::engraving::AutomationPointEdits edits {
        { oldPointData.tick, SetPoint { updatedOldPoint } },
        { newTick, SetPoint { newPoint } }
    };

    editAutomationPoints(curveKey, edits);

    return true;
}

bool NotationAutomationController::requestAddPoint(const SysStaffKey& key, qreal x, qreal y)
{
    IF_ASSERT_FAILED(key.isValid()) {
        return false;
    }

    const System* system = key.system;
    const SysStaff* sysStaff = system ? system->staff(key.staffIdx) : nullptr;
    const Staff* staff = score() ? score()->staff(key.staffIdx) : nullptr;
    IF_ASSERT_FAILED(sysStaff && staff) {
        return false;
    }

    const muse::RectF staffCanvasRect = sysStaff->bbox().translated(system->canvasPos());
    const std::optional<int> newTick = tickFromCanvasX(system, staffCanvasRect, x);
    if (!newTick) {
        return false;
    }

    mu::engraving::AutomationPoint newPoint;
    newPoint.value.outValue = automationValueFromDisplay(currentAutomationType(), 1.0 - y);
    newPoint.value.inValue = mu::engraving::AutomationPoint::ExplicitArrival { newPoint.value.outValue,
                                                                               mu::engraving::AutomationPoint::Ease::none() };
    newPoint.generated = false;

    const mu::engraving::AutomationCurveKey curveKey = curveKeyFor(currentAutomationType(), staff);

    mu::engraving::AutomationPointEdits edits {
        { *newTick, SetPoint { newPoint } }
    };

    editAutomationPoints(curveKey, edits);

    return true;
}

bool NotationAutomationController::requestEditPointEase(const PointData& arrivalPointData, const QPointF& fromPointF,
                                                        const SysStaffKey& key, qreal x, qreal y)
{
    IF_ASSERT_FAILED(key.isValid() && arrivalPointData.hasBend) {
        return false;
    }

    const Staff* staff = score() ? score()->staff(key.staffIdx) : nullptr;
    const AutomationDataConstPtr data = automationData();
    IF_ASSERT_FAILED(staff && data) {
        return false;
    }

    const AutomationCurveKey curveKey = curveKeyFor(currentAutomationType(), staff);
    const AutomationCurve& curve = data->curve(curveKey);

    const auto existingIt = curve.find(arrivalPointData.tick);
    IF_ASSERT_FAILED(existingIt != curve.end()) {
        return false;
    }

    //! 这一段的两端：本点（到达点）与它在上游曲线里的前一个点。
    //! ⚠️ 用曲线里的前一个点，而不是折线上的前一个顶点 —— 折线上的前一个顶点可能是**同一个点**
    //! 的 `OUT`（拆开的点），那样算出来的段是那个"竖跳"，不是这一段。
    const auto prevIt = (existingIt == curve.begin()) ? curve.end() : std::prev(existingIt);
    if (prevIt == curve.end()) {
        return false;
    }

    const muse::real_t prevOutValue = prevIt->second.value.outValue;
    const muse::real_t arrivalValue = mu::engraving::resolveInValue(curve, existingIt);
    if (muse::RealIsEqual(prevOutValue, arrivalValue)) {
        // 平的段没有弯折可言（range 是 0，怎么弯都是平的）
        return false;
    }

    //! 横向：手柄在段内的位置（同一套 5% 夹取，与拖动预览一致）
    const double spanX = arrivalPointData.qPointF.x() - fromPointF.x();
    if (spanX <= 0.0) {
        return false;
    }
    const double t = std::clamp((double(x) - fromPointF.x()) / spanX, BEND_T_MIN, BEND_T_MAX);

    //! 纵向：手柄高度 → 自动化值 → 落在"前一点的值 .. 到达值"之间的比例，这就是 `Ease::value`
    const double draggedValue = automationValueFromDisplay(currentAutomationType(), 1.0 - double(y));
    const double range = double(arrivalValue) - double(prevOutValue);
    const double fraction = std::clamp((draggedValue - double(prevOutValue)) / range, 0.0, 1.0);

    //! 写成什么，与 MIDI 页共用同一个函数 —— 两页拖出来的曲率语义必须一模一样。
    const std::optional<AutomationPoint> written = bentAutomationPoint(existingIt->second, t, fraction);
    if (!written.has_value()) {
        return false;
    }

    mu::engraving::AutomationPointEdits edits {
        { arrivalPointData.tick, SetPoint { *written } }
    };

    //! ⚠️ 走 `INotationAutomation::editPoints`（记谱页的撤销栈事务），**不要**在这里自己
    //! `startCmd`/`endCmd`：只有記谱页那条路会在提交后通知"栈变了"，撤销/重做命令状态、
    //! 主菜单与 Ctrl+Z 才会跟上（维护手册 §4.8）。
    editAutomationPoints(curveKey, edits);

    return true;
}

bool NotationAutomationController::requestRemovePoint(const PointData& pointData, const SysStaffKey& key)
{
    IF_ASSERT_FAILED(key.isValid()) {
        return false;
    }

    const mu::engraving::AutomationPoint* automationPoint = automationPointAt(key, pointData.tick);
    if (!automationPoint || automationPoint->generated || automationPoint->itemId.has_value()) {
        return false;
    }

    const Staff* staff = score() ? score()->staff(key.staffIdx) : nullptr;
    IF_ASSERT_FAILED(staff) {
        return false;
    }

    const mu::engraving::AutomationCurveKey curveKey = curveKeyFor(currentAutomationType(), staff);

    mu::engraving::AutomationPointEdits edits {
        { pointData.tick, ErasePoint {} }
    };

    editAutomationPoints(curveKey, edits);

    return true;
}

void NotationAutomationController::editAutomationPoints(const mu::engraving::AutomationCurveKey& key,
                                                        mu::engraving::AutomationPointEdits& edits)
{
    const INotationAutomationPtr notationAutomation = automation();
    IF_ASSERT_FAILED(notationAutomation) {
        return;
    }

    notationAutomation->editPoints(key, edits);
}

const mu::engraving::AutomationPoint* NotationAutomationController::automationPointAt(const SysStaffKey& key, int tick) const
{
    const Staff* staff = score() ? score()->staff(key.staffIdx) : nullptr;
    IF_ASSERT_FAILED(staff && automationData()) {
        return nullptr;
    }

    const mu::engraving::AutomationCurveKey curveKey = curveKeyFor(currentAutomationType(), staff);
    const mu::engraving::AutomationCurve& curve = automationData()->curve(curveKey);
    const auto it = curve.find(tick);
    if (it == curve.end()) {
        return nullptr;
    }

    return &it->second;
}

AutomationType NotationAutomationController::currentAutomationType() const
{
    return notationConfiguration()->currentAutomationType();
}

INotationAutomationPtr NotationAutomationController::automation() const
{
    const IMasterNotationPtr masterNotation = globalContext()->currentMasterNotation();
    return masterNotation ? masterNotation->automation() : nullptr;
}

INotationPtr NotationAutomationController::currentNotation() const
{
    return globalContext()->currentNotation();
}

mu::engraving::AutomationDataConstPtr NotationAutomationController::automationData() const
{
    const INotationAutomationPtr notationAutomation = automation();
    return notationAutomation ? notationAutomation->automationData() : nullptr;
}

mu::engraving::Score* NotationAutomationController::score() const
{
    return currentNotation() ? currentNotation()->elements()->msScore() : nullptr;
}
