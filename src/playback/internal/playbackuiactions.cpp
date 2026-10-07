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
#include "playbackuiactions.h"

#include "ui/view/iconcodes.h"
#include "context/uicontext.h"
#include "context/shortcutcontext.h"
#include "types/translatablestring.h"
#include "notation/inotationinteraction.h"
#include "notation/inotationselection.h"

using namespace mu::playback;
using namespace mu::notation;
using namespace muse;
using namespace muse::ui;
using namespace muse::actions;

static const ActionCode PLAY_FROM_SELECTION_CODE("play-from-selection");
static const ActionCode CLEAR_ONLINE_SOUNDS_CACHE_CODE("clear-online-sounds-cache");

const UiActionList PlaybackUiActions::s_mainActions = {
    //! ⚠️ 走带类（play / pause / rewind / loop）用的是 `CTX_PROJECT_PAGE_OPENED`，不是
    //! `CTX_NOTATION_FOCUSED` —— 因为 MIDI 编辑页（`musescore://midi`）显示/编辑的是**同一个工程**，
    //! 走带在这两页都该能用。原来的上下文只认"记谱页有焦点"，MIDI 页解析出的 UI 上下文是
    //! `UiCtxUnknown` ⇒ 那条 `Space` 快捷键在 MIDI 页被过滤掉，空格落到全局的
    //! `nav-trigger-control` 上（用户 2026-10-07 报「空格没反应」的根因）。
    //! 新上下文**只放宽这一层**，不把整页记谱类快捷键带过去（见 context/shortcutcontext.h）。
    UiAction("play",
             mu::context::UiCtxProjectOpened,
             mu::context::CTX_PROJECT_PAGE_OPENED,
             TranslatableString("action", "Play"),
             TranslatableString("action", "Play"),
             IconCode::Code::PLAY
             ),
    UiAction(PLAY_FROM_SELECTION_CODE,
             mu::context::UiCtxProjectOpened,
             mu::context::CTX_NOTATION_OPENED,
             TranslatableString("action", "Play from selection"),
             TranslatableString("action", "Play from selection"),
             IconCode::Code::PLAY
             ),
    UiAction("pause",
             mu::context::UiCtxProjectOpened,
             mu::context::CTX_PROJECT_PAGE_OPENED,
             TranslatableString("action", "Pause"),
             TranslatableString("action", "Pause playback"),
             IconCode::Code::PAUSE
             ),
    UiAction("pause-and-select",
             mu::context::UiCtxProjectOpened,
             mu::context::CTX_NOTATION_OPENED,
             TranslatableString("action", "Pause and select"),
             TranslatableString("action", "Pause and select playback position"),
             IconCode::Code::PAUSE
             ),
    UiAction("stop",
             mu::context::UiCtxProjectOpened,
             mu::context::CTX_NOTATION_OPENED,
             TranslatableString("action", "Stop"),
             TranslatableString("action", "Stop playback"),
             IconCode::Code::STOP
             ),
    UiAction("rewind",
             mu::context::UiCtxProjectOpened,
             mu::context::CTX_PROJECT_PAGE_OPENED,
             TranslatableString("action", "Rewind"),
             TranslatableString("action", "Rewind"),
             IconCode::Code::REWIND
             ),
    UiAction("loop",
             mu::context::UiCtxProjectOpened,
             mu::context::CTX_PROJECT_PAGE_OPENED,
             TranslatableString("action", "Loop playback"),
             TranslatableString("action", "Toggle ‘Loop playback’"),
             IconCode::Code::LOOP,
             Checkable::Yes
             ),
    UiAction("metronome",
             mu::context::UiCtxProjectOpened,
             mu::context::CTX_NOTATION_FOCUSED,
             TranslatableString("action", "Metronome"),
             TranslatableString("action", "Toggle metronome playback"),
             IconCode::Code::METRONOME,
             Checkable::Yes
             ),
    UiAction("playback-setup",
             mu::context::UiCtxProjectOpened,
             mu::context::CTX_NOTATION_FOCUSED,
             TranslatableString("action", "Playback setup"),
             TranslatableString("action", "Open playback setup dialog"),
             IconCode::Code::NONE
             ),
    //! Backing/reference audio track. Kept in the playback action list so it appears
    //! alongside the other playback commands in the menu/shortcut editor.
    UiAction("import-audio",
             mu::context::UiCtxProjectOpened,
             mu::context::CTX_NOTATION_OPENED,
             TranslatableString("action", "Import audio"),
             TranslatableString("action", "Add an audio file as a backing track that plays along with the score"),
             IconCode::Code::NONE
             ),
    //! The two offset commands were registered as commands only, so they never showed up in the
    //! shortcut editor and could not be bound at all. They are here so "line the backing track
    //! up by ear" can actually be done from the keyboard, one beat at a time.
    UiAction("audio-track-earlier",
             mu::context::UiCtxProjectOpened,
             mu::context::CTX_NOTATION_OPENED,
             TranslatableString("action", "Audio track: move earlier"),
             TranslatableString("action", "Start the backing track one beat earlier, to line it up with the score"),
             IconCode::Code::NONE
             ),
    UiAction("audio-track-later",
             mu::context::UiCtxProjectOpened,
             mu::context::CTX_NOTATION_OPENED,
             TranslatableString("action", "Audio track: move later"),
             TranslatableString("action", "Start the backing track one beat later, to line it up with the score"),
             IconCode::Code::NONE
             )
};

const UiActionList PlaybackUiActions::s_midiInputActions = {
    UiAction("midi-on",
             mu::context::UiCtxAny,
             mu::context::CTX_ANY,
             TranslatableString("action", "Enable MIDI input"),
             TranslatableString("action", "Toggle MIDI input"),
             IconCode::Code::MIDI_INPUT,
             Checkable::Yes
             ),
};

const UiActionList PlaybackUiActions::s_midiInputPitchActions = {
    UiAction("midi-input-written-pitch",
             mu::context::UiCtxAny,
             mu::context::CTX_ANY,
             TranslatableString("action", "Written pitch"),
             TranslatableString("action", "Input written pitch"),
             IconCode::Code::NONE,
             Checkable::Yes
             ),
    UiAction("midi-input-sounding-pitch",
             mu::context::UiCtxAny,
             mu::context::CTX_ANY,
             TranslatableString("action", "Sounding pitch"),
             TranslatableString("action", "Input sounding pitch"),
             IconCode::Code::NONE,
             Checkable::Yes
             ),
};

const UiActionList PlaybackUiActions::s_settingsActions = {
    UiAction("repeat",
             mu::context::UiCtxAny,
             mu::context::CTX_NOTATION_FOCUSED,
             TranslatableString("action", "Play repeats"),
             TranslatableString("action", "Play repeats"),
             IconCode::Code::PLAY_REPEATS,
             Checkable::Yes
             ),
    UiAction("play-chord-symbols",
             mu::context::UiCtxAny,
             mu::context::CTX_NOTATION_FOCUSED,
             TranslatableString("action", "Play chord symbols"),
             TranslatableString("action", "Play chord symbols"),
             IconCode::Code::CHORD_SYMBOL,
             Checkable::Yes
             ),
    UiAction("toggle-hear-playback-when-editing",
             mu::context::UiCtxAny,
             mu::context::CTX_ANY,
             TranslatableString("action", "Hear playback when editing"),
             TranslatableString("action", "Toggle hear playback when editing"),
             IconCode::Code::AUDIO,
             Checkable::Yes
             ),
    UiAction("pan",
             mu::context::UiCtxAny,
             mu::context::CTX_ANY,
             TranslatableString("action", "Pan score automatically"),
             TranslatableString("action", "Pan score automatically during playback"),
             IconCode::Code::PAN_SCORE,
             Checkable::Yes
             ),
    UiAction("countin",
             mu::context::UiCtxAny,
             mu::context::CTX_ANY,
             TranslatableString("action", "Enable count-in when playing"),
             TranslatableString("action", "Enable count-in when playing"),
             IconCode::Code::COUNT_IN,
             Checkable::Yes
             ),
};

const UiActionList PlaybackUiActions::s_loopBoundaryActions = {
    UiAction("loop-in",
             mu::context::UiCtxAny,
             mu::context::CTX_NOTATION_FOCUSED,
             TranslatableString("action", "Set loop marker left"),
             TranslatableString("action", "Set loop marker left"),
             IconCode::Code::LOOP_IN
             ),
    UiAction("loop-out",
             mu::context::UiCtxAny,
             mu::context::CTX_NOTATION_FOCUSED,
             TranslatableString("action", "Set loop marker right"),
             TranslatableString("action", "Set loop marker right"),
             IconCode::Code::LOOP_OUT
             ),
};

const UiActionList PlaybackUiActions::s_diagnosticActions = {
    UiAction("playback-reload-cache",
             mu::context::UiCtxAny,
             mu::context::CTX_ANY,
             TranslatableString("action", "Reload playback cache")
             )
};

const UiActionList PlaybackUiActions::s_onlineSoundsActions = {
    UiAction(CLEAR_ONLINE_SOUNDS_CACHE_CODE,
             mu::context::UiCtxAny,
             mu::context::CTX_ANY,
             TranslatableString("action", "Clear online sounds cache for this score")
             ),
};

PlaybackUiActions::PlaybackUiActions(std::shared_ptr<PlaybackController> controller, const muse::modularity::ContextPtr& iocCtx)
    : muse::Contextable(iocCtx), m_controller(controller)
{
}

void PlaybackUiActions::init()
{
    m_controller->isPlayAllowedChanged().onReceive(this, [this](bool) {
        const UiActionList& actions = actionsList();

        ActionCodeList codes;
        codes.reserve(actions.size());

        for (const UiAction& action : actions) {
            codes.push_back(action.code);
        }

        m_actionEnabledChanged.send(codes);
    });

    globalContext()->currentNotationChanged().onNotify(this, [this]() {
        INotationPtr currNotation = globalContext()->currentNotation();
        if (!currNotation) {
            return;
        }

        INotationInteractionPtr interaction = currNotation->interaction();
        interaction->selectionChanged().onNotify(this, [this]() {
            m_actionEnabledChanged.send({ PLAY_FROM_SELECTION_CODE });
        }, Asyncable::Mode::SetReplace /* FIXME */);

        interaction->isEditingElementChanged().onNotify(this, [this]() {
            m_actionEnabledChanged.send({ PLAY_FROM_SELECTION_CODE });
        }, Asyncable::Mode::SetReplace /* FIXME */);
    });

    m_controller->onlineSoundsChanged().onNotify(this, [this]() {
        m_actionEnabledChanged.send({ CLEAR_ONLINE_SOUNDS_CACHE_CODE });
    });
}

const UiActionList& PlaybackUiActions::actionsList() const
{
    static UiActionList alist;
    if (alist.empty()) {
        alist.insert(alist.end(), s_mainActions.cbegin(), s_mainActions.cend());
        alist.insert(alist.end(), s_midiInputActions.cbegin(), s_midiInputActions.cend());
        alist.insert(alist.end(), s_midiInputPitchActions.cbegin(), s_midiInputPitchActions.cend());
        alist.insert(alist.end(), s_settingsActions.cbegin(), s_settingsActions.cend());
        alist.insert(alist.end(), s_loopBoundaryActions.cbegin(), s_loopBoundaryActions.cend());
        alist.insert(alist.end(), s_diagnosticActions.cbegin(), s_diagnosticActions.cend());
        alist.insert(alist.end(), s_onlineSoundsActions.cbegin(), s_onlineSoundsActions.cend());
    }
    return alist;
}

bool PlaybackUiActions::actionEnabled(const UiAction& act) const
{
    if (!m_controller->isPlayAllowed()) {
        return false;
    }

    if (!m_controller->canReceiveAction(act.code)) {
        return false;
    }

    if (act.code == CLEAR_ONLINE_SOUNDS_CACHE_CODE) {
        return !m_controller->onlineSounds().empty();
    }

    if (act.code == PLAY_FROM_SELECTION_CODE) {
        const INotationPtr currNotation = globalContext()->currentNotation();
        const INotationInteractionPtr interaction = currNotation ? currNotation->interaction() : nullptr;
        const INotationSelectionPtr selection = interaction ? interaction->selection() : nullptr;
        if (!selection) {
            return false;
        }
        const bool selectionValid = !selection->isNone() || selection->lastElementHit();
        return selectionValid && !interaction->isEditingElement();
    }

    return true;
}

bool PlaybackUiActions::actionChecked(const UiAction&) const
{
    return false;
}

muse::async::Channel<ActionCodeList> PlaybackUiActions::actionEnabledChanged() const
{
    return m_actionEnabledChanged;
}

muse::async::Channel<ActionCodeList> PlaybackUiActions::actionCheckedChanged() const
{
    return m_actionCheckedChanged;
}

const UiActionList& PlaybackUiActions::midiInputActions()
{
    return s_midiInputActions;
}

const UiActionList& PlaybackUiActions::midiInputPitchActions()
{
    return s_midiInputPitchActions;
}

const UiActionList& PlaybackUiActions::settingsActions()
{
    return s_settingsActions;
}

const UiActionList& PlaybackUiActions::loopBoundaryActions()
{
    return s_loopBoundaryActions;
}

const muse::ui::ToolConfig& PlaybackUiActions::defaultPlaybackToolConfig()
{
    static ToolConfig config;
    if (!config.isValid()) {
        config.items = {
            { "rewind", true },
            { "play", true },
            { "loop", true },
            { "loop-in", true },
            { "loop-out", true },
            { "metronome", true },
        };
    }
    return config;
}
