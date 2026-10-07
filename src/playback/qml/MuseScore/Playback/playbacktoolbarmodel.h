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

#include "rcommand/commandtypes.h"
#include "uicomponents/qml/Muse/UiComponents/abstractmenumodel.h"

#include <QVariantMap>

#include "modularity/ioc.h"
#include "iplaybackcontroller.h"
#include "notation/inotationconfiguration.h"
#include "context/iglobalcontext.h"

namespace mu::playback {
class PlaybackToolBarModel : public muse::uicomponents::AbstractMenuModel
{
    Q_OBJECT

    Q_PROPERTY(bool isToolbarFloating READ isToolbarFloating WRITE setIsToolbarFloating NOTIFY isToolbarFloatingChanged)
    Q_PROPERTY(bool isPlayAllowed READ isPlayAllowed NOTIFY isPlayAllowedChanged)

    Q_PROPERTY(QTime maxPlayTime READ maxPlayTime NOTIFY maxPlayTimeChanged)

    Q_PROPERTY(QTime playTime READ playTime WRITE setPlayTime NOTIFY playPositionChanged)
    Q_PROPERTY(qreal playPosition READ playPosition WRITE setPlayPosition NOTIFY playPositionChanged)
    Q_PROPERTY(int measureNumber READ measureNumber WRITE setMeasureNumber NOTIFY playPositionChanged)
    Q_PROPERTY(int maxMeasureNumber READ maxMeasureNumber NOTIFY playPositionChanged)
    Q_PROPERTY(int beatNumber READ beatNumber WRITE setBeatNumber NOTIFY playPositionChanged)
    Q_PROPERTY(int maxBeatNumber READ maxBeatNumber NOTIFY playPositionChanged)

    Q_PROPERTY(QVariant tempo READ tempo NOTIFY tempoChanged)
    Q_PROPERTY(qreal tempoMultiplier READ tempoMultiplier WRITE setTempoMultiplier NOTIFY tempoChanged)

    //! 🆕 宿主页面插进按钮行的**一个**按钮（MIDI 页用它把"录制"放到走带里、节拍器左边）。
    //!
    //! 为什么需要这样一个插槽：这一行是"播放模块的命令清单"，而**录制不是播放模块的命令** ——
    //! 它的实现在 MIDI 页的模型里（那边才知道录到哪个谱表、量化多少、写回哪一次事务）。
    //! 宿主给一个 { title, description, icon, checked, enabled }，模型把它排在**节拍器之前**
    //! （位置是用户指定的），点它时发 `extraItemTriggered()`，由宿主自己去做事。
    //!
    //! 没设这个属性时按钮行与上游**完全一致** —— 记谱页那条走带就是这样（它不该多出一个
    //! 按了没用的录制键）。
    Q_PROPERTY(QVariantMap extraItem READ extraItem WRITE setExtraItem NOTIFY extraItemChanged)

    //! 插槽按钮的 id（空串 = 没插）。视图用它判断"这个按钮要不要听自己的 enabled" ——
    //! 上游那些按钮的可用性由整行（`isPlayAllowed`）管着，不能顺手改它们的样子。
    Q_PROPERTY(QString extraItemId READ extraItemId NOTIFY extraItemChanged)

    QML_ELEMENT

    muse::GlobalInject<notation::INotationConfiguration> notationConfiguration;
    muse::ContextInject<IPlaybackController> playbackController = { this };
    muse::ContextInject<context::IGlobalContext> globalContext = { this };

public:
    explicit PlaybackToolBarModel(QObject* parent = nullptr);

    bool isToolbarFloating() const;
    bool isPlayAllowed() const;

    QTime maxPlayTime() const;
    QTime playTime() const;
    qreal playPosition() const;

    int measureNumber() const;
    int maxMeasureNumber() const;
    int beatNumber() const;
    int maxBeatNumber() const;

    QVariant tempo() const;
    qreal tempoMultiplier() const;

    QVariantMap extraItem() const { return m_extraItem; }
    QString extraItemId() const;

    //! 插槽按钮被点：宿主自己去做事（模型不猜它是什么按钮）。
    Q_INVOKABLE void handleMenuItem(const QString& itemId) override;

    Q_INVOKABLE void load() override;

public slots:
    void setIsToolbarFloating(bool floating);
    void setPlayPosition(qreal position);
    void setPlayTime(const QTime& time);
    void setMeasureNumber(int measureNumber);
    void setBeatNumber(int beatNumber);
    void setTempoMultiplier(qreal multiplier);
    void setExtraItem(const QVariantMap& item);

signals:
    void isToolbarFloatingChanged(bool floating);
    void isPlayAllowedChanged();
    void maxPlayTimeChanged();
    void playPositionChanged();
    void tempoChanged();
    void extraItemChanged();
    void extraItemTriggered();

private:
    void setupConnections();
    muse::uicomponents::MenuItem* makeInputPitchMenu();

    void updateActions();

    bool isAdditionalCommand(const muse::rcommand::Command& command) const;

    muse::secs_t totalPlayTime() const;
    engraving::MeasureBeat measureBeat() const;

    muse::uicomponents::MenuItem* makePlayItem();
    void updatePlayItem();

    //! 🆕 由宿主给的 map 造出插槽按钮。
    muse::uicomponents::MenuItem* makeExtraItem();

    void updatePlayPosition(muse::audio::secs_t secs);

    void rewind(muse::audio::secs_t secs);
    void rewindToBeat(const engraving::MeasureBeat& beat);

    bool m_isToolbarFloating = false;
    muse::secs_t m_playbackPositionSecs = 0.0;
    QVariantMap m_extraItem;
};
}
