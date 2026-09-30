/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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
#pragma once

#include <QtEvents>

#include "../../../internal/inotationviewcontroller.h"

#include "modularity/ioc.h"

#include "async/asyncable.h"

#include "context/iglobalcontext.h"

#include "notation/inotationinteraction.h"
#include "notation/inotationplayback.h"
#include "notation/inotationconfiguration.h"
#include "notation/inotationcontextconfiguration.h"
#include "notation/types/viewmode.h"

#include "notationscene/inotationcommandscontroller.h"

#include "playback/iplaybackcontroller.h"

#include "global/iglobalconfiguration.h"
#include "ui/idragcontroller.h"
#include "ui/iuiconfiguration.h"
#include "rcommand/icommanddispatcher.h"

#include "abstractelementpopupmodel.h"

class QQuickItem;

namespace mu::engraving {
class Score;
}

namespace mu::notation {
class IControlledView
{
public:
    virtual ~IControlledView() = default;

    virtual qreal width() const = 0;
    virtual qreal height() const = 0;

    virtual muse::PointF viewportTopLeft() const = 0;

    //! muse::Returns true if the canvas has been moved
    virtual bool moveCanvas(qreal dx, qreal dy) = 0;
    virtual void moveCanvasHorizontal(qreal dx) = 0;
    virtual void moveCanvasVertical(qreal dy) = 0;

    virtual muse::RectF notationContentRect() const = 0;
    virtual qreal currentScaling() const = 0;
    virtual void setScaling(qreal scaling, const muse::PointF& pos, bool overrideZoomType = true) = 0;

    virtual muse::PointF toLogical(const muse::PointF& p) const = 0;
    virtual muse::PointF toLogical(const QPointF& p) const = 0;
    virtual muse::PointF fromLogical(const muse::PointF& r) const = 0;
    virtual muse::RectF fromLogical(const muse::RectF& r) const = 0;

    virtual bool isNoteEnterMode() const = 0;

    virtual void showContextMenu(const ElementType& elementType, const QPointF& pos) = 0;
    virtual void hideContextMenu() = 0;

    virtual void showSearch() = 0;

    virtual void showElementPopup(const ElementType& elementType) = 0;
    virtual void hideElementPopup(const ElementType& elementType) = 0;
    virtual void hideElementPopup(PopupModelType modelType = PopupModelType::TYPE_UNDEFINED) = 0;
    virtual void toggleElementPopup(const ElementType& elementType) = 0;

    virtual bool elementPopupIsOpen(const ElementType& elementType) const = 0;

    virtual INotationInteractionPtr notationInteraction() const = 0;
    virtual INotationPlaybackPtr notationPlayback() const = 0;

    virtual QQuickItem* asItem() = 0;

    virtual void scheduleRedraw(const muse::RectF& rect = muse::RectF()) = 0;
};

class NotationViewInputController : public INotationViewController, public muse::Contextable, public muse::async::Asyncable
{
public:
    muse::GlobalInject<muse::IGlobalConfiguration> globalConfiguration;
    muse::GlobalInject<INotationConfiguration> configuration;
    muse::GlobalInject<muse::ui::IUiConfiguration> uiConfiguration;
    muse::ContextInject<INotationContextConfiguration> contextConfiguration = { this };
    muse::ContextInject<muse::rcommand::ICommandDispatcher> commandDispatcher = { this };
    muse::ContextInject<playback::IPlaybackController> playbackController = { this };
    muse::ContextInject<context::IGlobalContext> globalContext = { this };
    muse::ContextInject<muse::ui::IDragController> dragController = { this };
    muse::ContextInject<INotationCommandsController> commandsController = { this };

public:
    NotationViewInputController(IControlledView* view, const muse::modularity::ContextPtr& iocCtx);
    ~NotationViewInputController();

    void init();
    void deinit();

    // INotationViewController
    void zoomIn() override;
    void zoomOut() override;
    void zoomToPageWidth() override;
    void zoomToWholePage() override;
    void zoomToTwoPages() override;
    void setZoom(int zoomPercentage) override;

    void setViewMode(ViewMode viewMode) override;

    void nextScreen() override;
    void previousScreen() override;
    void nextPage() override;
    void previousPage() override;
    void startOfScore() override;
    void endOfScore() override;

    void openContextMenuOfSelection() override;

    void togglePopupForItemIfSupports(const EngravingItem* item) override;

    void showSearch() override;

    void redrawView() override;
    // -----------------------

    void initZoom();
    void updateZoomAfterSizeChange();

    void initCanvasPos();

    bool readonly() const;
    void setReadonly(bool readonly);

    void pinchToZoom(qreal scaleFactor, const QPointF& pos);
    void wheelEvent(QWheelEvent* event);
    void mousePressEvent(QMouseEvent* event);
    void mouseMoveEvent(QMouseEvent* event);
    void mouseReleaseEvent(QMouseEvent* event);
    void mouseDoubleClickEvent(QMouseEvent* event);
    void hoverMoveEvent(QHoverEvent* event);
    void hoverLeaveEvent(QHoverEvent* event);
    bool shortcutOverrideEvent(QKeyEvent* event);
    void keyPressEvent(QKeyEvent* event);
    void keyReleaseEvent(QKeyEvent* event);
    void inputMethodEvent(QInputMethodEvent* event);

    bool canHandleInputMethodQuery(Qt::InputMethodQuery query) const;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const;

    bool ignoreNextMouseContextMenuEvent() const { return m_ignoreNextMouseContextMenuEvent; }

    void dragEnterEvent(QDragEnterEvent* event);
    void dragLeaveEvent(QDragLeaveEvent* event);
    void dragMoveEvent(QDragMoveEvent* event);
    void dropEvent(QDropEvent* event);

    ElementType selectionType() const;
    muse::PointF selectionElementPos() const;

private:
    INotationPtr currentNotation() const;
    INotationStylePtr notationStyle() const;
    INotationInteractionPtr viewInteraction() const;
    const INotationInteraction::HitElementContext& hitElementContext() const;

    //! Everything a lane drag needs to keep in mind between press, move and release.
    struct AudioLaneDrag
    {
        bool active = false;
        //! Where the pointer went down, in score units. Deltas are measured from here rather
        //! than accumulated per move, so a drag that wanders and comes back lands where it
        //! started instead of drifting.
        muse::PointF beginPos;
        //! Score units per tick at the lane under the press. Taken from the measure the press
        //! landed on, because that is the only place where the local pixel-to-time scale is
        //! known -- it differs between measures and resets at every system break.
        double unitsPerTick = 0.0;
        //! Ticks this drag has decided on so far. Committed on release, not on every move --
        //! see updateAudioLaneDrag() for why that is not merely an optimisation.
        int pendingTicks = 0;
        //! The score whose lane is being dragged. It carries the transient preview shift the
        //! lane is drawn with, and it is what gets redrawn per move.
        engraving::Score* score = nullptr;
        //! The whole lane of the system the press landed on, in score units: the only strip
        //! that can change while the drag is in progress.
        muse::RectF laneBand;
        //! Furthest the lane has been moved in this drag. The repaint has to cover where
        //! earlier frames drew, or dragging out and back leaves a waveform behind.
        double previewReach = 0.0;
    };

    //! Starts dragging the audio lane if the press landed on one. Returns true when the event
    //! belongs to the drag and must not be treated as a normal click.
    //!
    //! `hitStaff` is the staff the press handler already resolved, and it is the right test:
    //! hitStaff() resolves through hitMeasure(), which asks whether the point is inside a
    //! StaffLines hit shape -- exactly "on the lane". The hit element cannot be used instead,
    //! because hitElement() is built from the segments (notes, rests, and so on) and never
    //! returns the StaffLines that draws the lane, so an element-based test never fires.
    bool beginAudioLaneDrag(const muse::PointF& logicPos, engraving::Staff* hitStaff);
    void updateAudioLaneDrag(const muse::PointF& logicPos);
    void endAudioLaneDrag();

    //! Repaints the strip the dragged lane lives in -- the picture changes there and nowhere
    //! else, and a full viewport repaint per mouse move is not free on a large score.
    void redrawAudioLaneBand();

    AudioLaneDrag m_audioLaneDrag;

    void onNotationChanged();

    void doZoomToPageWidth();
    void doZoomToWholePage();
    void doZoomToTwoPages();
    int currentZoomIndex() const;
    int currentZoomPercentage() const;
    muse::PointF findZoomFocusPoint() const;
    void doSetZoom(int zoomPercentage, const muse::PointF& pos);
    qreal scalingFromZoomPercentage(int zoomPercentage) const;
    int zoomPercentageFromScaling(qreal scaling) const;

    void moveScreen(int direction);
    void movePage(int direction);

    void setScaling(qreal scaling, const muse::PointF& pos = muse::PointF(), bool overrideZoomType = true);

    void startDragElements(ElementType elementsType, const muse::PointF& elementsOffset);

    void updateShadowNotePopupVisibility(bool forceHide = false);

    float hitWidth() const;

    struct ClickContext {
        muse::PointF logicClickPos;
        const QMouseEvent* event = nullptr;
        mu::engraving::EngravingItem* hitElement = nullptr;
        mu::engraving::staff_idx_t hitStaff = muse::nidx;
        bool isHitGrip = false;
    };

    void handleClickInNoteInputMode(QMouseEvent* event);
    bool mousePress_considerGrip(const ClickContext& ctx); // returns true if event is consumed
    bool mousePress_considerDragOutgoingElement(const ClickContext& ctx);
    void mousePress_considerSelect(const ClickContext& ctx);
    void mousePress_seekSelection(const ClickContext& ctx);
    void cycleOverlappingHitElements(const std::vector<EngravingItem*>& hitElements, staff_idx_t hitStaffIndex);
    bool mousePress_considerDragOutgoingRange(const ClickContext& ctx);
    bool mousePress_considerStartPasteRangeOnRelease(const ClickContext& ctx);
    void handleLeftClick(const ClickContext& ctx);
    void handleRightClick(const ClickContext& ctx);
    void handleLeftClickRelease(const QPointF& releasePoint);

    bool startTextEditingAllowed() const;
    void updateTextCursorPosition();

    bool isAnchorEditingEvent(QKeyEvent* event) const;

    bool tryPercussionShortcut(QKeyEvent* event);

    IControlledView* m_view = nullptr;

    QList<int> m_possibleZoomPercentages;

    bool m_readonly = false;
    bool m_isCanvasDragged = false;
    bool m_tripleClickPending = false;

    struct MouseDownInfo {
        enum DragAction {
            DragOutgoingElement,
            DragOutgoingRange,
            PasteRangeOnRelease,
            Other,
            Nothing
        } dragAction = Other;

        QPointF physicalBeginPoint;
        muse::PointF logicalBeginPoint;
    } m_mouseDownInfo;

    struct DragMoveEvent {
        QPointF position;
        Qt::KeyboardModifiers modifiers;
        Qt::DropAction dropAction = Qt::DropAction::CopyAction;
        QObject* source = nullptr;
    };

    bool dropEvent(const DragMoveEvent& event, const QMimeData* mimeData = nullptr);

    std::vector<int> pitchesBeingDragged() const;

    DragMoveEvent m_lastDragMoveEvent;

    const mu::engraving::EngravingItem* m_prevSelectedElement = nullptr;
    std::vector<const mu::engraving::EngravingItem*> m_notesBeingDragged;

    bool m_hitElementWasAlreadySingleSelected = false;
    bool m_shouldSelectOnLeftClickRelease = false;
    bool m_shouldStartEditOnLeftClickRelease = false;
    bool m_ignoreNextMouseContextMenuEvent = false;
};
}
