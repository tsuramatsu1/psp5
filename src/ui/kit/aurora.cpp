// psp5 - Design "Aurora Shelf": the home screen, showing the memory stick.
//
// Forked from PS5_VKHomebrewUI's src/concepts/aurora.cpp.
// Copyright (C) 2026 BlackBearReloaded, the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Why a fork rather than a patch. The kit's designs read app::Context::catalog,
// a demo::Catalog that fills itself in its own constructor and hands out its
// items read-only; there is no seam to push real content through. The kit itself
// is a pinned checkout re-fetched by tools/setup-kit.sh, so editing it in place
// would be erased by the next build. So psp5 keeps its own copy of the one
// design it ships, the kit's copy is dropped from the build, and the content
// comes from psp5::Library() - the games actually on the stick.
//
// Everything below the content is the kit's work, unchanged: the springs, the
// cross-fades, the frosted sheet. What changed is where the titles come from,
// which facts the panels state, and that the top bar is now a real control.
//
// The classic living-room launcher: a hero panel for the focused title above
// horizontal shelves of covers. What makes it feel finished:
//
//   - the backdrop takes the colours of the focused cover, eased, so the whole
//     screen breathes with the selection instead of sitting on a fixed theme;
//   - the hero text and artwork cross-fade with a small slide when the focus
//     changes, staggered so the title lands first;
//   - shelves scroll with springs, the focused card grows, and the focus ring
//     is a separate spring that glides between cards;
//   - Cross opens a frosted sheet over the screen with the details and
//     actions; the screen behind it dims, blurs and scales back slightly;
//   - every move has a sound placed in stereo where it happened, and the ends
//     of a shelf answer with a soft refusal and a nudge instead of silence.

#include "concepts/concepts.hpp"

#include "core/tween.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include "PS5Paths.h"
#include "ui/PS5AuroraLauncher.h"
#include "ui/PS5GameLibrary.h"
#include "ui/PS5GameSound.h"
#include "ui/PS5Keyboard.h"
#include "ui/PS5Cheats.h"
#include "ui/PS5GameAchievements.h"
#include "ui/PS5Settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace hui::concepts
{

namespace
{

using gfx::Color;
using gfx::Rect;

const Color kWhite = Color::rgb(0xffffff);

constexpr float kMargin = 96.0f;
constexpr float kCard = 196.0f; // card size at rest
constexpr float kCardGap = 26.0f;
constexpr float kCardGrow = 1.2f;     // focused card scale
constexpr int kViews = static_cast<int>(psp5::GameView::count); // tabs along the top
// The last row of the settings panel, which is not a setting.
const std::string kQuitLabel = "Close PSP5";
const std::string kCheatsEnabledLabel = "Cheats enabled";
const std::string kImportLabel = "Import from cheat.db";
const std::string kOverridesLabel = "Settings for this game";
const std::string kAchievementsLabel = "RetroAchievements";
// The RetroAchievements dialogs - signing in, and signing out - are the same
// card at the same size. Their height is built from what is in them rather than
// written down twice: the body runs to two lines, and the buttons sit below it.
// Both were 280 and 300 tall, which put the second line of the body straight
// through the buttons.
constexpr float kDialogW = 780.0f;
constexpr float kDialogPad = 48.0f;
constexpr float kDialogBody = 178.0f;  // first baseline of the body text
constexpr float kDialogLine = 34.0f;   // and its line height
constexpr float kDialogText = 25.0f;   // and its size
constexpr int kDialogMaxLines = 3;
constexpr float kDialogButton = 62.0f;
constexpr float kDialogAir = 30.0f;    // between the last line and the buttons
constexpr float kDialogFoot = 36.0f;

// The top of the kit's button-hint row: its layout centres the glyphs on 1010
// and draws them 40 tall. The settings list measures itself against this rather
// than assuming a number of rows that happens to fit.
constexpr float kHintsTop = 1010.0f - 20.0f;

constexpr float kShelfY = 730.0f;     // top of the focused shelf's cards
constexpr float kShelfPitch = 304.0f; // distance between shelves
// Play, Resume, Cheats, Close. Resume is always on the list rather than
// appearing only when there is a state to load: a row that comes and goes
// moves the others under the player's thumb.
constexpr int kActions = 4;

constexpr const char *kTechniques[] = {
    "Backdrop colours eased toward the focused cover's palette (ui::SpringColor)",
    "Hero text and artwork cross-fade with a staggered slide on every focus change",
    "Spring-driven shelf scrolling, card growth and a gliding focus ring",
    "Frosted details sheet: Frame::glass blurs the screen behind the overlay",
    "Stereo-panned focus sounds, pitched row changes, refusal nudge at the ends",
};

constexpr app::TourStep kTour[] = {
    {0.5f, 0, Direction::right},
    {0.25f, 0, Direction::right},
    {0.25f, 0, Direction::right},
    {0.9f, 0, Direction::down, "shelf"},
    {0.3f, 0, Direction::right},
    {0.3f, 0, Direction::right},
    {0.9f, action_bit(Action::confirm), Direction::none, "library"},
    {0.5f, 0, Direction::down},
    {0.7f, action_bit(Action::confirm), Direction::none, "details"},
    {0.6f, action_bit(Action::back)},
};

struct Shelf
{
    const char *title;
    std::vector<int> items; // catalogue indices
    int column = 0;
    ui::Scroller scroll;
};

class Aurora final : public app::Concept
{
  public:
    explicit Aurora(app::Context &context) : context_(context)
    {
        rebuild_shelf(-1);
        if (!empty())
        {
            shown_ = previous_ = focused_item();
            apply_palette(true);
            // The opening selection, which is otherwise never announced: the
            // report below only fires when the focus changes.
            psp5::GameSoundPlayer().Focus(static_cast<std::size_t>(shown_));
        }
        ring_.snap(card_rect(0, 0, true));
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "aurora",
            "Aurora Shelf",
            "A console home screen: hero panel, cover shelves, frosted details",
            "src/concepts/aurora.cpp",
            audio::SoundSet::glass,
            Color::rgb(0x7ff0d8),
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        age_ = 0.0f;
        sheet_open_ = false;
        // Coming back from a game. Nothing should still be holding a game's
        // settings open, but saying so costs nothing and a stale scope is not
        // something to carry into a fresh shelf.
        cheats_open_ = false;
        achievements_open_ = false;
        psp5::GameAchievementList().Close();
        psp5::SaveCheatsEnabled();
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        age_ += dt;
        clock_ += dt;
        // The tab strip reaches past the shelf, so it is read before anything
        // that depends on there being a game to focus. Not while the details
        // sheet is up, though: there L1/R1 would move the screen out from under
        // an open panel.
        if (sign_out_asking_)
        {
            // Left picks Sign out, right picks Cancel - where each sits, rather
            // than a step from wherever the cursor is. With two buttons a step
            // is a toggle, and nav repeats while a direction is held: holding
            // the stick made the choice flicker between them, so the button let
            // go on was not reliably the one being pointed at. Signing out by
            // accident is the expensive way to be wrong, so this does not guess.
            if (input.nav == Direction::left || input.nav == Direction::right)
            {
                const int next = input.nav == Direction::left ? 0 : 1;
                if (next != sign_out_button_)
                {
                    sign_out_button_ = next;
                    feedback.play(audio::Cue::focus, 1.0f, 0.0f);
                }
            }
            // The Cross that opened this is most likely still down. Nothing is
            // answered until it has been let go, so the press that asked the
            // question cannot also answer it.
            if (!sign_out_armed_)
            {
                sign_out_armed_ = !input.is_held(Action::confirm);
                return;
            }
            if (input.is_pressed(Action::confirm))
            {
                if (sign_out_button_ == 0)
                {
                    psp5::AchievementsLogout();
                    feedback.play(audio::Cue::back);
                }
                sign_out_asking_ = false;
            }
            else if (input.is_pressed(Action::back))
            {
                sign_out_asking_ = false;
                feedback.play(audio::Cue::back);
            }
            return;
        }

        if (psp5::AchievementsSignIn() != psp5::SignIn::idle)
        {
            // The dialog has the pad while it is up, and it does not close
            // itself: a result worth showing is worth waiting to be read.
            const int count = sign_in_buttons();
            if (count == 0)
                return;  // still talking to the server

            if (input.nav == Direction::left || input.nav == Direction::right)
            {
                const int next =
                    std::clamp(sign_in_button_ + (input.nav == Direction::right ? 1 : -1), 0,
                               count - 1);
                if (next != sign_in_button_)
                {
                    sign_in_button_ = next;
                    feedback.play(audio::Cue::focus, 1.0f, 0.0f);
                }
            }
            const bool retry = sign_in_button_ == 0 &&
                               psp5::AchievementsSignIn() == psp5::SignIn::failed &&
                               psp5::CanRetryAchievementsSignIn();
            if (input.is_pressed(Action::confirm))
            {
                if (retry)
                {
                    psp5::RetryAchievementsSignIn();
                    feedback.play(audio::Cue::open);
                }
                else
                {
                    psp5::ClearAchievementsSignIn();
                    feedback.play(audio::Cue::back);
                }
                sign_in_button_ = 0;
            }
            else if (input.is_pressed(Action::back))
            {
                psp5::ClearAchievementsSignIn();
                feedback.play(audio::Cue::back);
                sign_in_button_ = 0;
            }
            return;
        }

        // The keyboard is over everything and has the pad to itself: a press
        // that typed a letter must not also move the shelf behind it.
        if (psp5::KeyboardPanel().open())
        {
            bool accepted = false;
            if (psp5::KeyboardPanel().Update(input, feedback, &accepted))
                finish_typing(accepted);
            return;
        }

        // OPTIONS is the settings, from anywhere on the home screen. It is a
        // console's button for this, and it leaves the face buttons to the shelf.
        if (input.is_pressed(Action::menu))
        {
            // OPTIONS is read before anything else and returns, so it is the
            // one way out of the cheat panel that skipped closing it.
            close_cheats(feedback, false);
            toggle_settings(feedback);
            return;
        }
        if (achievements_open_)
        {
            update_achievements(input, feedback);
            return;
        }
        if (!settings_open_ && !sheet_open_ && update_tabs(input, feedback))
            return;
        if (settings_open_)
        {
            update_settings(input, feedback);
            nudge_.update(dt, 9.0f);
            return;
        }
        // Nothing to focus, so nothing below this has a subject. The backdrop
        // still breathes; the screen says what to do about it.
        if (empty())
            return;
        if (sheet_open_)
            update_sheet(input, feedback);
        else
            update_shelves(input, feedback);

        // ---- animation state ----
        if (focused_item() != shown_)
        {
            previous_ = shown_;
            shown_ = focused_item();
            // The game under the cursor plays its own menu loop, as it would on
            // a PSP. The player waits for the cursor to settle before starting.
            psp5::GameSoundPlayer().Focus(static_cast<std::size_t>(shown_));
            hero_.start(context_.settings.reduced_motion ? 0.12f : 0.42f);
            apply_palette(false);
        }
        hero_.update(dt);
        for (ui::SpringColor &colour : palette_)
            colour.update(dt, 4.0f);
        row_position_.target = static_cast<float>(row_);
        row_position_.update(dt, 11.0f);
        for (int r = 0; r < static_cast<int>(shelves_.size()); ++r)
        {
            Shelf &shelf = shelves_[static_cast<std::size_t>(r)];
            const float start = static_cast<float>(shelf.column) * (kCard + kCardGap);
            shelf.scroll.reveal(start, start + kCard * kCardGrow, gfx::kVirtualWidth - kMargin,
                                kMargin * 2.2f);
            shelf.scroll.update(dt, 12.0f);
        }
        ring_.target(card_rect(row_, shelves_[static_cast<std::size_t>(row_)].column, true));
        ring_.update(dt, 20.0f);
        nudge_.update(dt, 9.0f);
        sheet_.target = sheet_open_ ? 1.0f : 0.0f;
        sheet_.update(dt, context_.settings.reduced_motion ? 40.0f : 13.0f);
        action_position_.target = static_cast<float>(action_);
        action_position_.update(dt, 22.0f);
        star_.update(dt, 5.0f);
    }

    void draw(app::Frame &frame) const override
    {
        frame.backdrop.mode = gfx::BackdropMode::aurora;
        frame.backdrop.colors[0] = palette_[0].value();
        frame.backdrop.colors[1] = palette_[1].value();
        frame.backdrop.colors[2] = palette_[2].value();
        frame.backdrop.colors[3] = palette_[3].value();
        frame.backdrop.time = clock_;

        gfx::DrawList &list = frame.scene;
        // The sheet pushes the screen back a little: the scene shrinks toward
        // its centre and dims while the overlay is up.
        const float back = sheet_.value;
        list.push_transform(1.0f - 0.035f * back, 960, 540, 0, 0);
        draw_top_bar(list);
        if (settings_open_)
            draw_settings(list);
        else if (empty())
            draw_empty(list);
        else
        {
            draw_hero(list);
            draw_shelves(list);
        }
        list.pop_transform();
        if (achievements_open_)
            draw_achievements(list);
        draw_sign_in(list);
        draw_sign_out(list);
        psp5::KeyboardPanel().Draw(list, context_.fonts, palette_[3].value());
        if (back > 0.01f)
            list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                              Color::rgb(0x05070f, 0.45f * back));

        if (back > 0.01f)
        {
            // No glass capture: the panel is opaque, and capturing the screen
            // for a blur nothing draws is a copy of the whole frame wasted.
            draw_sheet(frame.overlay, frame.glass_texture);
        }
        draw_hints(back > 0.5f ? frame.overlay : frame.scene);
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    // A console with nothing on its memory stick is an ordinary state, not a
    // failure, and every accessor below would read past the end of an empty
    // shelf. So the screen checks this once and draws a placeholder instead.
    bool empty() const
    {
        return shelves_.empty() || shelves_[0].items.empty();
    }

    // One shelf per view: the whole library in that view's order. The views are
    // the tabs along the top, so switching one re-sorts the shelf rather than
    // showing different games - there is only ever one library.
    //
    // `keep` is an item index to stay focused on across the change, or -1.
    void rebuild_shelf(int keep)
    {
        const std::span<const int> order = psp5::Library().order(view_);
        shelves_.assign(1, Shelf{});
        shelves_[0].title = psp5::GameViewName(view_);
        shelves_[0].items.assign(order.begin(), order.end());
        row_ = 0;
        shelves_[0].column = 0;
        if (keep < 0)
            return;
        // The same game, wherever the new order put it: a re-sort that moved the
        // selection to a different title would read as the tab changing content.
        for (int c = 0; c < static_cast<int>(shelves_[0].items.size()); ++c)
            if (shelves_[0].items[static_cast<std::size_t>(c)] == keep)
            {
                shelves_[0].column = c;
                break;
            }
    }

    int focused_item() const
    {
        const Shelf &shelf = shelves_[static_cast<std::size_t>(row_)];
        return shelf.items[static_cast<std::size_t>(shelf.column)];
    }

    const demo::Item &item(int index) const
    {
        return psp5::Library().item(static_cast<std::size_t>(index));
    }

    const psp5::GameEntry &entry(int index) const
    {
        return psp5::Library().entry(static_cast<std::size_t>(index));
    }

    // The backdrop's four colours come from the focused cover.
    void apply_palette(bool snap)
    {
        const demo::Item &focused = item(focused_item());
        const Color targets[4] = {
            gfx::mix(focused.dark, Color::rgb(0x05060c), 0.35f),
            gfx::mix(focused.dark, focused.mid, 0.35f),
            focused.mid,
            gfx::mix(focused.mid, focused.accent, 0.55f),
        };
        for (int i = 0; i < 4; ++i)
        {
            if (snap)
                palette_[i].snap(targets[i]);
            else
                palette_[i].target(targets[i]);
        }
    }

    // Where a card sits. `focused` gives the grown rectangle the ring uses.
    Rect card_rect(int row, int column, bool focused) const
    {
        const Shelf &shelf = shelves_[static_cast<std::size_t>(row)];
        const float x =
            kMargin + static_cast<float>(column) * (kCard + kCardGap) - shelf.scroll.offset();
        const float y = kShelfY + (static_cast<float>(row) - row_position_.value) * kShelfPitch;
        if (!focused)
            return {x, y, kCard, kCard};
        const float grown = kCard * kCardGrow;
        return {x, y - (grown - kCard), grown, grown};
    }

    // L1/R1. Handled here rather than inside a screen, because it is what moves
    // between them - and before anything takes a reference into shelves_, since
    // changing the ordering rebuilds the shelf and the reference would not
    // survive it. Returns whether the tab changed, which ends the frame's input.
    bool update_tabs(const InputFrame &input, app::Feedback &feedback)
    {
        const bool prev = input.is_pressed(Action::page_prev);
        const bool next = input.is_pressed(Action::page_next);
        if (prev == next)
            return false;

        // Wrapping, because the strip shows L1 and R1 at both ends: a tab row
        // that stopped dead at either end would be refusing a move it offers.
        const int moved = (static_cast<int>(view_) + (next ? 1 : kViews - 1)) % kViews;
        view_ = static_cast<psp5::GameView>(moved);
        rebuild_shelf(empty() ? -1 : focused_item());
        feedback.play(audio::Cue::tab, next ? 1.06f : 0.94f);
        return true;
    }

    void open_settings(app::Feedback &feedback)
    {
        settings_open_ = true;
        setting_ = 0;
        psp5::SettingsPanel().Reload();
        feedback.play(audio::Cue::open);
    }

    void close_settings(app::Feedback &feedback)
    {
        settings_open_ = false;
        // Written once, on the way out, rather than on every keypress: PPSSPP's
        // save rewrites the whole ini.
        const bool changed = psp5::SettingsPanel().dirty();
        if (changed)
            psp5::SettingsPanel().Save();
        // Always, even when nothing changed: this is what leaves PPSSPP's
        // game-specific mode, and a panel that left it set would make the next
        // global edit land in the game's file.
        psp5::SettingsPanel().EndGame();
        feedback.play(changed ? audio::Cue::saved : audio::Cue::back);
    }

    void toggle_settings(app::Feedback &feedback)
    {
        if (settings_open_)
        {
            close_settings(feedback);
            return;
        }
        // OPTIONS is the whole title's settings, so any game scope is dropped.
        psp5::SettingsPanel().EndGame();
        open_settings(feedback);
    }

    // Scoped to a game, the first row is the switch that says whether the game
    // has settings of its own. Otherwise the last row closes the title: OPTIONS
    // opens this panel, and the foot of a settings list is where someone looks
    // for a way out.
    bool game_scoped() const
    {
        return psp5::SettingsPanel().scopedToGame();
    }

    int settings_rows() const
    {
        // The settings, then RetroAchievements and Close psp5 - unless this is
        // one game's panel, which has neither and the override switch instead.
        return static_cast<int>(psp5::SettingsPanel().size()) + (game_scoped() ? 1 : 2);
    }

    bool override_row(int row) const
    {
        return game_scoped() && row == 0;
    }

    bool quit_row(int row) const
    {
        return !game_scoped() && row == settings_rows() - 1;
    }

    // One row above Close psp5, and only on the title's own settings: an
    // account belongs to the player, not to a game.
    bool achievements_row(int row) const
    {
        return !game_scoped() && row == settings_rows() - 2;
    }

    // Which setting a row shows, or -1 when the row is not one.
    int setting_at(int row) const
    {
        if (quit_row(row) || achievements_row(row) || override_row(row))
            return -1;
        const int index = game_scoped() ? row - 1 : row;
        return index >= 0 && index < static_cast<int>(psp5::SettingsPanel().size()) ? index : -1;
    }

    // Signing in takes two answers, so the keyboard is opened twice and this
    // says which one came back.
    enum class Typing
    {
        none,
        achievement_user,
        achievement_password,
    };

    // What the server is doing, while it is doing it. The sign-in happens on
    // another thread and can take a few seconds; with nothing on screen it
    // looked as though the password had simply been dropped.
    // How many buttons the dialog is showing, and what they do.
    int sign_in_buttons() const
    {
        const psp5::SignIn state = psp5::AchievementsSignIn();
        if (state == psp5::SignIn::failed)
            return psp5::CanRetryAchievementsSignIn() ? 2 : 1;
        return state == psp5::SignIn::succeeded ? 1 : 0;
    }

    // Where a dialog's card and buttons go, for a message of this length. The
    // text is wrapped first and the card built around the answer. Both dialogs
    // were given a height by hand and both were too short, so the last line of
    // the message ran through the buttons; and the sign-in message is one line
    // while it is working and three when it has failed, which no one height
    // holds.
    struct DialogBox
    {
        Rect card;
        float body;    // first baseline of the message
        float buttons; // top of the button row
    };

    DialogBox dialog_box(std::string_view message) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const std::size_t wrapped =
            fonts.regular.font->wrap(message, kDialogText, kDialogW - kDialogPad * 2.0f).size();
        const float lines =
            static_cast<float>(std::clamp<std::size_t>(wrapped, 1, kDialogMaxLines));
        const float body_bottom = kDialogBody + lines * kDialogLine;
        const float h = body_bottom + kDialogAir + kDialogButton + kDialogFoot;
        DialogBox box;
        box.card = Rect{(gfx::kVirtualWidth - kDialogW) * 0.5f,
                        (gfx::kVirtualHeight - h) * 0.5f, kDialogW, h};
        box.body = box.card.y + kDialogBody;
        box.buttons = box.card.y + body_bottom + kDialogAir;
        return box;
    }

    void draw_sign_out(gfx::DrawList &list) const
    {
        if (!sign_out_asking_)
            return;
        const ui::Fonts &fonts = context_.fonts;
        list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                          Color::rgb(0x05070f, 0.76f));
        const std::string message =
            "Signing out as " + psp5::AchievementsUser() +
            ". Games will stop awarding achievements until you sign in again.";
        const DialogBox box = dialog_box(message);
        const Rect card = box.card;
        list.shadow({card.x, card.y + 18, card.w, card.h}, 36, 48, Color::rgb(0x000000, 0.55f));
        list.rounded_rect(card, 28, Color::rgb(0x000000));
        list.bordered_rect(card, 28, Color::rgb(0x000000, 0.0f), 2, kWhite.with_alpha(0.32f));

        ui::text(list, fonts.semibold, "RETROACHIEVEMENTS", card.x + 48, card.y + 62, 20,
                 palette_[3].value(), gfx::Align::left, 4.0f);
        ui::text(list, fonts.display, "Sign out?", card.x + 46, card.y + 128, 46, kWhite);
        ui::paragraph(list, fonts.regular, message, card.x + kDialogPad, box.body, kDialogText,
                      card.w - kDialogPad * 2.0f, kDialogLine, kWhite.with_alpha(0.78f),
                      kDialogMaxLines);

        const char *labels[2] = {"Sign out", "Cancel"};
        const float width = 200.0f;
        const float gap = 16.0f;
        float x = card.x + card.w - kDialogPad - 2.0f * width - gap;
        for (int i = 0; i < 2; ++i)
        {
            const bool focused = i == sign_out_button_;
            const Rect button{x, box.buttons, width, kDialogButton};
            if (focused)
            {
                list.glow(button, 31, 18, palette_[3].value().with_alpha(0.3f));
                list.rounded_rect(button, 31, kWhite);
            }
            else
            {
                list.bordered_rect(button, 31, kWhite.with_alpha(0.06f), 2,
                                   kWhite.with_alpha(0.3f));
            }
            ui::text(list, fonts.semibold, labels[i], button.cx(), button.cy() + 9, 26,
                     focused ? Color::rgb(0x0b0d16) : kWhite.with_alpha(0.9f),
                     gfx::Align::center);
            x += width + gap;
        }
    }

    void draw_sign_in(gfx::DrawList &list) const
    {
        const psp5::SignIn state = psp5::AchievementsSignIn();
        if (state == psp5::SignIn::idle)
            return;

        const ui::Fonts &fonts = context_.fonts;
        const bool working = state == psp5::SignIn::working;
        const bool good = state == psp5::SignIn::succeeded;
        std::string detail;
        if (working)
            detail = "Talking to the server.";
        else if (good)
            detail = "Signed in as " + psp5::AchievementsUser() + ". Games will award "
                                                                  "achievements as you play.";
        else
            detail = "The server did not accept it. Check the username and password, and that "
                     "the console is online.";

        list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                          Color::rgb(0x05070f, 0.76f));
        const DialogBox box = dialog_box(detail);
        const Rect card = box.card;
        list.shadow({card.x, card.y + 18, card.w, card.h}, 36, 48, Color::rgb(0x000000, 0.55f));
        list.rounded_rect(card, 28, Color::rgb(0x000000));
        list.bordered_rect(card, 28, Color::rgb(0x000000, 0.0f), 2,
                           kWhite.with_alpha(working ? 0.18f : 0.32f));

        ui::text(list, fonts.semibold, "RETROACHIEVEMENTS", card.x + 48, card.y + 62, 20,
                 palette_[3].value(), gfx::Align::left, 4.0f);

        const char *heading = working  ? "Signing in"
                              : good   ? "Signed in"
                                       : "Could not sign in";
        ui::text(list, fonts.display, heading, card.x + 46, card.y + 128, 46, kWhite);

        ui::paragraph(list, fonts.regular, detail, card.x + kDialogPad, box.body, kDialogText,
                      card.w - kDialogPad * 2.0f, kDialogLine, kWhite.with_alpha(0.78f),
                      kDialogMaxLines);

        if (working)
        {
            // A bar that travels rather than fills: how long this takes is the
            // server's business, and a progress bar would be inventing one.
            const float width = 170.0f;
            const float travel = card.w - kDialogPad * 2.0f - width;
            const float at = (std::sin(clock_ * 1.9f) * 0.5f + 0.5f) * travel;
            const float bar = box.buttons + kDialogButton * 0.5f - 2.0f;
            list.rounded_rect({card.x + kDialogPad, bar, card.w - kDialogPad * 2.0f, 4}, 2,
                              kWhite.with_alpha(0.14f));
            list.rounded_rect({card.x + kDialogPad + at, bar, width, 4}, 2,
                              palette_[3].value());
            return;
        }

        // The dialog waits: it says what happened and stays until it is answered.
        const int count = sign_in_buttons();
        const bool retry = state == psp5::SignIn::failed && psp5::CanRetryAchievementsSignIn();
        const char *labels[2] = {retry ? "Retry" : "Close", "Close"};
        const float width = 200.0f;
        const float gap = 16.0f;
        float x = card.x + card.w - kDialogPad - (float)count * width
                  - (float)(count - 1) * gap;
        for (int i = 0; i < count; ++i)
        {
            const bool focused = i == sign_in_button_;
            const Rect button{x, box.buttons, width, kDialogButton};
            if (focused)
            {
                list.glow(button, 31, 18, palette_[3].value().with_alpha(0.3f));
                list.rounded_rect(button, 31, kWhite);
            }
            else
            {
                list.bordered_rect(button, 31, kWhite.with_alpha(0.06f), 2,
                                   kWhite.with_alpha(0.3f));
            }
            ui::text(list, fonts.semibold, labels[i], button.cx(), button.cy() + 9, 26,
                     focused ? Color::rgb(0x0b0d16) : kWhite.with_alpha(0.9f),
                     gfx::Align::center);
            x += width + gap;
        }
    }

    void finish_typing(bool accepted)
    {
        const Typing was = typing_;
        typing_ = Typing::none;
        if (!accepted)
        {
            typed_user_.clear();
            return;
        }
        if (was == Typing::achievement_user)
        {
            typed_user_ = psp5::KeyboardPanel().text();
            if (typed_user_.empty())
                return;
            typing_ = Typing::achievement_password;
            psp5::KeyboardPanel().Open("RETROACHIEVEMENTS", "Password", "", true);
            return;
        }
        if (was == Typing::achievement_password)
        {
            psp5::AchievementsLogin(typed_user_, psp5::KeyboardPanel().text());
            typed_user_.clear();
        }
    }

    void update_settings(const InputFrame &input, app::Feedback &feedback)
    {
        psp5::Settings &settings = psp5::SettingsPanel();
        const int count = settings_rows();
        if (input.is_pressed(Action::back))
        {
            toggle_settings(feedback);
            return;
        }

        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            // Wrapping, not stopping. This is a short closed list and the row
            // most often wanted from the top of it - Close psp5 - is the last
            // one, so refusing the step up put it furthest from where the
            // cursor starts.
            const int step = input.nav == Direction::down ? 1 : count - 1;
            setting_ = (setting_ + step) % count;
            feedback.play(audio::Cue::focus, 1.0f, 0.0f);
        }

        if (quit_row(setting_))
        {
            if (input.is_pressed(Action::confirm))
            {
                feedback.play(audio::Cue::launch);
                if (psp5::SettingsPanel().dirty())
                    psp5::SettingsPanel().Save();
                psp5::RequestQuit();
            }
            return;
        }

        if (achievements_row(setting_))
        {
            if (input.is_pressed(Action::confirm))
            {
                if (!psp5::AchievementsAvailable())
                {
                    feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
                }
                else if (psp5::AchievementsLoggedIn())
                {
                    // Asked, not done: the row shows the account, so pressing it
                    // reads as opening it rather than as leaving it.
                    sign_out_asking_ = true;
                    sign_out_armed_ = false;
                    sign_out_button_ = 1;  // Cancel, so a second press changes nothing
                    feedback.play(audio::Cue::open);
                }
                else
                {
                    typing_ = Typing::achievement_user;
                    psp5::KeyboardPanel().Open("RETROACHIEVEMENTS", "Username",
                                               psp5::AchievementsUser(), false);
                    feedback.play(audio::Cue::open);
                }
            }
            return;
        }

        if (override_row(setting_))
        {
            if (input.nav == Direction::left || input.nav == Direction::right ||
                input.is_pressed(Action::confirm))
            {
                settings.SetOverrides(!settings.overrides());
                feedback.play(audio::Cue::toggle, 1.0f, 0.0f);
            }
            return;
        }

        // With no settings of its own, the rows below show the global ones and
        // are not this game's to change.
        if (game_scoped() && !settings.overrides())
        {
            if (input.nav == Direction::left || input.nav == Direction::right ||
                input.is_pressed(Action::confirm))
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            return;
        }

        // Left and right change the focused setting; Cross does the same as
        // right, because a row that is only on or off reads as something to
        // press rather than something to scroll.
        int delta = 0;
        if (input.nav == Direction::right || input.is_pressed(Action::confirm))
            delta = 1;
        else if (input.nav == Direction::left)
            delta = -1;
        if (delta != 0)
        {
            if (settings.Adjust(static_cast<std::size_t>(setting_at(setting_)), delta))
                feedback.play(audio::Cue::toggle, 1.0f, 0.0f);
            else
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        }
    }

    void update_shelves(const InputFrame &input, app::Feedback &feedback)
    {
        Shelf &shelf = shelves_[static_cast<std::size_t>(row_)];
        const int count = static_cast<int>(shelf.items.size());
        const int rows = static_cast<int>(shelves_.size());
        bool refused = false;
        switch (input.nav)
        {
        case Direction::left:
        case Direction::right:
        {
            const int next = shelf.column + (input.nav == Direction::right ? 1 : -1);
            if (next >= 0 && next < count)
            {
                shelf.column = next;
                feedback.play(audio::Cue::focus, 1.0f,
                              ui::pan_for_x(card_rect(row_, next, false).cx()));
            }
            else
            {
                refused = !input.nav_repeat;
                nudge_direction_ = input.nav == Direction::right ? 1.0f : -1.0f;
            }
            break;
        }
        case Direction::up:
        case Direction::down:
        {
            const int next = row_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < rows)
            {
                row_ = next;
                // Rows sound like steps of a scale: lower shelves, lower pitch.
                feedback.play(audio::Cue::tab, 1.12f - 0.08f * static_cast<float>(row_));
            }
            else
            {
                refused = !input.nav_repeat;
                nudge_direction_ = 0.0f;
            }
            break;
        }
        case Direction::none:
            break;
        }
        if (refused)
        {
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            feedback.rumble(0.25f, 0.05f);
            nudge_.trigger();
        }
        if (input.is_pressed(Action::confirm))
        {
            sheet_open_ = true;
            cheats_open_ = false;
            action_ = 0;
            action_position_.snap(0.0f);
            feedback.play(audio::Cue::open);
        }
        if (input.is_pressed(Action::north))
            toggle_favorite(feedback);
        if (input.is_pressed(Action::jump_next) && !empty())
        {
            // R2: the face buttons are all spoken for on the shelf - Cross is
            // the details, Circle goes back, Triangle favourites and Square is
            // this game's settings - so the achievements get a trigger.
            if (!psp5::AchievementsLoggedIn())
            {
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            }
            else
            {
                feedback.play(audio::Cue::open);
                achievements_open_ = true;
                achievement_ = 0;
                psp5::setAchievementFilter(psp5::AchievementFilter::all);
                psp5::GameAchievementList().Open(entry(focused_item()).path,
                                                 entry(focused_item()).disc_id);
            }
        }
        if (input.is_pressed(Action::west) && !empty())
        {
            // The same panel, scoped to this game. OPTIONS opens it for the
            // whole title; Square opens it for what is under the cursor.
            psp5::SettingsPanel().BeginGame(entry(focused_item()).disc_id,
                                            item(focused_item()).title);
            open_settings(feedback);
        }
    }

    // Leaving the cheat panel, by whatever route. While it is up, PPSSPP is
    // held in the game's own settings so the master switch reads and writes
    // that game's value - and that has to be given back on the way out. It was
    // only given back by Circle, so leaving any other way left the next game's
    // panel editing this game's file.
    void close_cheats(app::Feedback &feedback, bool announce)
    {
        if (!cheats_open_)
            return;
        psp5::Cheats &cheats = psp5::CheatList();
        const bool changed = cheats.dirty();
        if (changed)
            cheats.Save();
        psp5::SaveCheatsEnabled();
        if (announce)
            feedback.play(changed ? audio::Cue::saved : audio::Cue::back);
        cheats_open_ = false;
        cheat_notice_.clear();
    }

    void update_cheats(const InputFrame &input, app::Feedback &feedback)
    {
        psp5::Cheats &cheats = psp5::CheatList();
        // The master switch is the first row, and it is there whether or not the
        // game has a cheat file: without it on, the codes below are read and
        // ignored, so this is where it belongs.
        const int count = cheat_rows();

        if (input.is_pressed(Action::back))
        {
            close_cheats(feedback, true);
            return;
        }

        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next = std::clamp(cheat_ + (input.nav == Direction::down ? 1 : -1), 0,
                                        count - 1);
            if (next != cheat_)
            {
                cheat_ = next;
                feedback.play(audio::Cue::focus, 1.0f, 0.35f);
            }
        }
        if (input.is_pressed(Action::confirm))
        {
            if (cheat_ == 0)
            {
                psp5::SetCheatsEnabled(!psp5::CheatsEnabled());
                cheat_ = 0;
                cheat_notice_.clear();
                feedback.play(audio::Cue::toggle);
            }
            else if (cheat_import_row(cheat_))
            {
                import_cheats(feedback);
            }
            else
            {
                const int code = cheat_code_at(cheat_);
                if (code >= 0 && code < static_cast<int>(cheats.size()))
                    cheats.Toggle(static_cast<std::size_t>(code));
                feedback.play(audio::Cue::toggle);
            }
        }
    }

    void import_cheats(app::Feedback &feedback)
    {
        // Anything switched on and not yet written goes first: the import
        // appends to the same file, and saving afterwards would write the list
        // as it was read and drop what was just added.
        psp5::Cheats &cheats = psp5::CheatList();
        if (cheats.dirty())
            cheats.Save();

        int added = 0;
        const psp5::Cheats::Import result = cheats.ImportFromDatabase(&added);
        char text[96];
        switch (result)
        {
        case psp5::Cheats::Import::added:
            std::snprintf(text, sizeof(text), "Imported %d line%s from cheat.db", added,
                          added == 1 ? "" : "s");
            feedback.play(audio::Cue::saved);
            break;
        case psp5::Cheats::Import::none:
            std::snprintf(text, sizeof(text), "cheat.db has nothing new for this game");
            feedback.play(audio::Cue::back);
            break;
        case psp5::Cheats::Import::noFile:
            std::snprintf(text, sizeof(text), "No cheat.db in PSP/Cheats");
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            break;
        case psp5::Cheats::Import::noGame:
            std::snprintf(text, sizeof(text), "This game has no disc id to look up");
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            break;
        default:
            std::snprintf(text, sizeof(text), "Could not write the cheat file");
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            break;
        }
        cheat_notice_ = text;
        cheat_ = std::clamp(cheat_, 0, std::max(0, cheat_rows() - 1));
    }

    // The first row that is an achievement rather than a group's label.
    int first_achievement() const
    {
        const psp5::GameAchievements &list = psp5::GameAchievementList();
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            if (!list.row(i).header)
                return static_cast<int>(i);
        }
        return 0;
    }

    void update_achievements(const InputFrame &input, app::Feedback &feedback)
    {
        psp5::GameAchievements &list = psp5::GameAchievementList();
        list.Update();

        if (input.is_pressed(Action::back))
        {
            feedback.play(audio::Cue::back);
            list.Close();
            achievements_open_ = false;
            return;
        }
        const int count = static_cast<int>(list.size());
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            psp5::stepAchievementFilter(input.nav == Direction::right ? 1 : -1);
            achievement_ = first_achievement();
            feedback.play(audio::Cue::tab);
            return;
        }
        if (count > 0 && (input.nav == Direction::up || input.nav == Direction::down))
        {
            // A group's label is drawn, but there is nothing to select on it -
            // landing there looked like the first achievement had vanished.
            const int step = input.nav == Direction::down ? 1 : -1;
            int at = achievement_;
            for (int guard = 0; guard < count; ++guard)
            {
                at += step;
                if (at < 0 || at >= count)
                    return;  // the ends refuse rather than wrap
                if (!list.row(static_cast<std::size_t>(at)).header)
                    break;
            }
            if (at != achievement_ && at >= 0 && at < count)
            {
                achievement_ = at;
                feedback.play(audio::Cue::focus, 1.0f, 0.35f);
            }
        }
    }

    void update_sheet(const InputFrame &input, app::Feedback &feedback)
    {
        if (cheats_open_)
        {
            update_cheats(input, feedback);
            return;
        }
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next =
                std::clamp(action_ + (input.nav == Direction::down ? 1 : -1), 0, kActions - 1);
            if (next != action_)
            {
                action_ = next;
                feedback.play(audio::Cue::focus, 1.0f, 0.35f);
            }
        }
        if (input.is_pressed(Action::confirm))
        {
            if (action_ == 0)
            {
                feedback.play(audio::Cue::launch);
                feedback.rumble(0.7f, 0.18f);
                sheet_open_ = false;
                // In the kit this closed the sheet and nothing more. Here it
                // names the file; the launcher's loop sees it and gives the
                // device back so PPSSPP can boot it.
                psp5::RequestLaunch(entry(focused_item()).path,
                                    entry(focused_item()).disc_id);
            }
            else if (action_ == 1)
            {
                if (!psp5::HasSaveState(entry(focused_item()).disc_id))
                {
                    feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
                    return;
                }
                // The same boot, with PPSSPP told to load the newest state as
                // it starts: it has that already, for its own auto-load
                // setting, so psp5 does not have to time a load itself.
                feedback.play(audio::Cue::launch);
                feedback.rumble(0.7f, 0.18f);
                sheet_open_ = false;
                psp5::RequestLaunch(entry(focused_item()).path,
                                    entry(focused_item()).disc_id, true);
            }
            else if (action_ == 2)
            {
                // The cheat file is read now rather than when the shelf was
                // built: it is one small file, and reading it on the way in
                // means a file copied since the title started is still found.
                psp5::CheatList().Load(entry(focused_item()).disc_id);
                cheat_notice_.clear();
                cheat_ = 0;  // the master switch
                cheats_open_ = true;
                feedback.play(audio::Cue::open);
            }
            else
            {
                feedback.play(audio::Cue::modal_close);
                sheet_open_ = false;
            }
        }
        if (input.is_pressed(Action::back))
        {
            feedback.play(audio::Cue::back);
            close_cheats(feedback, false);
            psp5::GameAchievementList().Close();
            achievements_open_ = false;
            sheet_open_ = false;
        }
    }

    void toggle_favorite(app::Feedback &feedback)
    {
        const std::size_t index = static_cast<std::size_t>(focused_item());
        psp5::Library().ToggleFavorite(index);
        const bool starred = psp5::Library().IsFavorite(index);
        feedback.play(starred ? audio::Cue::favorite_on : audio::Cue::favorite_off);
        if (starred)
            star_.trigger();
        // The Favorites view is the list of marks, so marking one changes what
        // that shelf holds - including, when the last is unmarked, emptying it.
        if (view_ == psp5::GameView::favorites)
            rebuild_shelf(starred ? static_cast<int>(index) : -1);
    }

    // The tabs, and the shoulder buttons that move between them. In the kit this
    // strip was decoration with "Home" fixed as the active one; here each tab is
    // a way of ordering the library and L1/R1 select it, so the underline has to
    // follow the real selection.
    void draw_top_bar(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ui::GlyphStyle style = ui::GlyphStyle::dark();
        const float in = tween::stagger(age_, 0, 0.05f, 0.5f);
        const float y = 92 - 10 * (1.0f - in);
        list.push_opacity(in);

        // A shoulder glyph is far wider than it is tall, so the gap after it is
        // measured rather than guessed - at 30 high the L1 cap ran under the
        // first tab's text.
        constexpr float kGlyph = 30.0f;
        constexpr float kGlyphGap = 18.0f;
        float x = kMargin;
        ui::draw_button(list, fonts, style, ui::Button::l1, x, y - 9, kGlyph);
        x += ui::button_width(ui::Button::l1, kGlyph) + kGlyphGap;
        for (int i = 0; i < kViews; ++i)
        {
            const bool active = i == static_cast<int>(view_);
            const char *label = psp5::GameViewName(static_cast<psp5::GameView>(i));
            const float w = ui::text(list, active ? fonts.semibold : fonts.regular, label, x, y, 26,
                                     kWhite.with_alpha(active ? 1.0f : 0.55f));
            if (active)
                list.rounded_rect({x, 104, w, 4}, 2, palette_[3].value());
            x += w + 44;
        }
        ui::draw_button(list, fonts, style, ui::Button::r1, x - 44 + kGlyphGap, y - 9, kGlyph);

        // Where the clock and the account avatar were. A count of what is on the
        // stick is the one fact this screen can actually state.
        char text[48];
        const std::size_t count = psp5::Library().size();
        std::snprintf(text, sizeof(text), count == 1 ? "%u game" : "%u games",
                      static_cast<unsigned>(count));
        ui::text(list, fonts.regular, text, 1824, y, 26, kWhite.with_alpha(0.8f),
                 gfx::Align::right);
        list.pop_opacity();
    }

    // The settings, as a list: the name on the left, the value on the right, and
    // one line under the focused row saying what it does. No controls to hit -
    // the focused row is the control, and left and right move its value.
    void draw_settings(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const std::span<const psp5::SettingItem> rows = psp5::SettingsPanel().items();

        const float in = tween::stagger(age_, 1, 0.08f, 0.5f);
        list.push_opacity(in);
        ui::text(list, fonts.display,
                 game_scoped() ? psp5::SettingsPanel().gameTitle() : std::string("Settings"),
                 kMargin, 190 - 20 * (1.0f - in), 64, kWhite);
        // The focused row's explanation, in the one fixed place: under the title
        // rather than under the list, which would be past the foot of the screen
        // once every row is on it.
        const int at = setting_at(setting_);
        const char *hint =
            quit_row(setting_)           ? "Closes PSP5 and returns to the console."
            : achievements_row(setting_)
                ? (psp5::AchievementsAvailable()
                       ? "Signs in so games award achievements as you play."
                       : "This build has no HTTPS transport, so it cannot reach the server.")
            : override_row(setting_)     ? "Keeps a separate set of settings for this game."
            : at >= 0                    ? rows[static_cast<std::size_t>(at)].hint.c_str()
                                         : "";
        ui::text(list, fonts.regular, hint, kMargin, 243, 25, kWhite.with_alpha(0.7f));
        list.pop_opacity();

        // The list scrolls. It used to draw every row from a fixed top, which
        // fitted while there were eleven of them and put the last one through
        // the button hints the moment a twelfth was added. How many fit is
        // worked out from the space there is, so adding a setting cannot
        // overrun the screen again.
        constexpr float kRow = 56.0f;
        const float top = 300.0f;
        const float width = gfx::kVirtualWidth - kMargin * 2;
        const float shake = ui::shake(nudge_.value, clock_, 16.0f, 8.0f);
        const int count = settings_rows();
        const int visible =
            std::max(1, static_cast<int>((kHintsTop - 16.0f - top) / kRow));
        const int first = std::clamp(setting_ - visible / 2, 0, std::max(0, count - visible));
        const int last = std::min(count, first + visible);

        // Which part of the list this is, when there is more of it than fits.
        if (count > visible)
        {
            char counter[32];
            std::snprintf(counter, sizeof(counter), "%d of %d", setting_ + 1, count);
            ui::text(list, fonts.regular, counter, gfx::kVirtualWidth - kMargin, 243, 24,
                     kWhite.with_alpha(0.5f), gfx::Align::right);
        }

        for (int i = first; i < last; ++i)
        {
            const bool quit = quit_row(i);
            const bool focused = i == setting_;
            const float appear = tween::stagger(age_, 2 + i - first, 0.05f, 0.5f);
            list.push_opacity(appear);
            const Rect rect{kMargin,
                            top + static_cast<float>(i - first) * kRow + 24 * (1.0f - appear),
                            width, kRow - 8};
            if (focused)
            {
                list.rounded_rect({rect.x + shake, rect.y, rect.w, rect.h}, 18,
                                  kWhite.with_alpha(0.12f));
                list.rounded_rect({rect.x + shake, rect.y + 10, 5, rect.h - 20}, 3,
                                  palette_[3].value());
            }
            const int row_setting = setting_at(i);
            const bool overrides = override_row(i);
            // Dimmed where the row shows a global value this panel will not
            // change, so the panel never looks like it took an edit it refused.
            const bool live = !game_scoped() || overrides || psp5::SettingsPanel().overrides();
            const bool achievements = achievements_row(i);
            const std::string &label = quit           ? kQuitLabel
                                       : achievements ? kAchievementsLabel
                                       : overrides    ? kOverridesLabel
                                                      : rows[static_cast<std::size_t>(row_setting)].label;
            ui::text(list, focused ? fonts.semibold : fonts.regular, label, rect.x + 30 + shake,
                     rect.y + 34, 26,
                     kWhite.with_alpha(!live ? 0.4f : (focused ? 1.0f : 0.72f)));
            if (overrides)
                draw_toggle(list, rect.x + rect.w - 86 + shake, rect.y + 7,
                            psp5::SettingsPanel().overrides());
            else if (achievements)
                ui::text(list, fonts.semibold,
                         !psp5::AchievementsAvailable() ? std::string("Needs HTTPS")
                         : psp5::AchievementsLoggedIn() ? psp5::AchievementsUser()
                                                        : std::string("Not signed in"),
                         rect.x + rect.w - 30 + shake, rect.y + 34, 26,
                         focused ? palette_[3].value() : kWhite.with_alpha(0.6f),
                         gfx::Align::right);
            else if (!quit)
                ui::text(list, fonts.semibold, rows[static_cast<std::size_t>(row_setting)].value,
                         rect.x + rect.w - 30 + shake, rect.y + 34, 26,
                         !live ? kWhite.with_alpha(0.35f)
                               : (focused ? palette_[3].value() : kWhite.with_alpha(0.6f)),
                         gfx::Align::right);
            list.pop_opacity();
        }
    }

    // Shown when the memory stick holds no game psp5 can boot. Says where it
    // looked, because the usual cause is a file in the wrong folder.
    void draw_empty(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        const Rect panel{kMargin, 360, gfx::kVirtualWidth - kMargin * 2, 420};
        list.bordered_rect(panel, 36, kWhite.with_alpha(0.04f), 2, kWhite.with_alpha(0.16f));
        const bool no_favorites =
            view_ == psp5::GameView::favorites && psp5::Library().size() > 0;
        ui::text(list, fonts.display, no_favorites ? "No favorites yet" : "No games yet",
                 panel.x + 64, panel.y + 150, 72, kWhite);
        ui::text(list, fonts.regular,
                 no_favorites
                     ? "Press Triangle on a game to keep it here."
                     : "Copy a PSP ISO, CSO, CHD or EBOOT.PBP onto the memory stick and relaunch.",
                 panel.x + 64, panel.y + 220, 30, kWhite.with_alpha(0.82f));
        if (!no_favorites)
            ui::text(list, fonts.regular, PS5Paths::Memstick() + "/PSP/GAME", panel.x + 64,
                     panel.y + 290, 26, palette_[3].value());
        list.pop_opacity();
    }

    // One title's hero block, drawn at an opacity and a horizontal offset so
    // two of them can cross-fade.
    void draw_hero_item(gfx::DrawList &list, int index, float alpha, float slide) const
    {
        if (alpha <= 0.01f)
            return;
        const ui::Fonts &fonts = context_.fonts;
        const demo::Item &it = item(index);
        char text[96];
        list.push_opacity(alpha);

        // Artwork, floating gently, with a glow in its own accent colour.
        const float bob = context_.settings.reduced_motion ? 0.0f : std::sin(clock_ * 0.8f) * 6.0f;
        const Rect art{1316 + slide * 1.6f, 132 + bob, 440, 440};
        list.glow(art.inset(30), 60, 90, it.accent.with_alpha(0.3f));
        list.shadow({art.x, art.y + 26, art.w, art.h}, 36, 46, Color::rgb(0x000000, 0.55f));
        list.image(it.cover, art, gfx::kCanvasUv, kWhite, 36);
        list.bordered_rect(art, 36, Color::rgb(0x000000, 0.0f), 2, kWhite.with_alpha(0.16f));

        const float x = kMargin + slide;
        ui::text(list, fonts.semibold, ui::upper(shelves_[static_cast<std::size_t>(row_)].title), x,
                 212, 20, it.accent, gfx::Align::left, 4.0f);
        // The artwork starts at 1316, so the title has to stop before it. Set at
        // whatever size fits that width, down to a floor, and shortened only when
        // even the floor is not enough - a shrunk title still reads, a title
        // running under the cover does not.
        {
            // The artwork starts at 1316, so the title has to stop before it.
            constexpr float kTitleRight = 1316.0f - 48.0f;
            std::string shown = it.title;
            const float size = fit_title(&shown, 88.0f, 52.0f, kTitleRight - x);
            // Bigger text sits lower, so the baseline follows the size and the
            // block keeps its distance from the line under it.
            ui::text(list, fonts.display, shown, x - 4, 304 - (88.0f - size) * 0.35f, size,
                     kWhite);
        }
        // Where the rating and the play-time were. psp5 knows none of that about a
        // file on a memory stick, and three facts it does know read better than
        // three it would have to invent.
        const psp5::GameEntry &file = entry(index);
        std::snprintf(text, sizeof(text), "%s  \xC2\xB7  %s  \xC2\xB7  Added %s",
                      file.format.c_str(), file.size_text.c_str(), file.date_text.c_str());
        // The disc id, under the title. Set in the mono face because that is
        // what it is - an identifier, not prose - and it is what RetroAchievements
        // and the cheat files are keyed on, so it is worth being able to read off
        // the screen.
        if (!file.disc_id.empty())
            ui::text(list, fonts.mono, file.disc_id, x, 334, 22, kWhite.with_alpha(0.5f));
        ui::text(list, fonts.regular, text, x, 368, 26, kWhite.with_alpha(0.78f));
        // The path was only ever useful for finding a file. How long it has been
        // played is what a shelf is usually asked.
        ui::text(list, fonts.regular, psp5::PlayedLabel(file.disc_id), x, 420, 26,
                 kWhite.with_alpha(0.7f));

        // Triangle is already favorites and Square is already this game's
        // settings, so the achievements button is shown rather than bound to a
        // face button: the shelf's Cross moves to it like any other control.
        const Rect play{x, 540, 220, 64};
        list.glow(play, 32, 18, it.accent.with_alpha(0.35f));
        list.rounded_rect(play, 32, kWhite);
        ui::draw_button(list, fonts, ui::GlyphStyle::light(), ui::Button::cross, play.x + 22,
                        play.cy(), 34);
        // "Select", not "Play": Cross opens the game's details, where Play and
        // Resume are. A button that said Play and then showed a panel was
        // promising the wrong thing.
        ui::text(list, fonts.semibold, "Select", play.x + 72, play.cy() + 10, 28,
                 Color::rgb(0x0b0d16));
        const Rect more{x + 240, 540, 64, 64};
        list.bordered_rect(more, 32, kWhite.with_alpha(0.1f), 2, kWhite.with_alpha(0.3f));
        const bool starred = psp5::Library().IsFavorite(static_cast<std::size_t>(index));
        list.star(more.cx(), more.cy(), 16 + 10 * star_.value,
                  starred ? Color::rgb(0xffd166) : kWhite.with_alpha(0.85f), starred ? 0.0f : 2.5f);

        list.pop_opacity();
    }

    // A title set at whatever size fits the room it has, down to a floor, and
    // shortened only when even the floor is not enough: a shrunk title still
    // reads, a title running under the artwork or the buttons does not.
    // Rewrites `text` and returns the size to set it at.
    float fit_title(std::string *text, float largest, float smallest, float room) const
    {
        const ui::Fonts &fonts = context_.fonts;
        float size = largest;
        while (size > smallest && fonts.display.font->measure(*text, size) > room)
            size -= 4.0f;
        if (fonts.display.font->measure(*text, size) > room)
        {
            while (text->size() > 4 &&
                   fonts.display.font->measure(*text + "...", size) > room)
                text->pop_back();
            *text += "...";
        }
        return size;
    }

    void draw_hero(gfx::DrawList &list) const
    {
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        if (hero_.running)
        {
            // The old title leaves quickly; the new one arrives a beat later.
            const float t = hero_.progress();
            draw_hero_item(list, previous_, 1.0f - tween::smoothstep(t * 2.2f),
                           -36.0f * tween::cubic_in(tween::clamp01(t * 2.2f)));
            const float arrive = tween::clamp01((t - 0.25f) / 0.75f);
            draw_hero_item(list, shown_, tween::smoothstep(arrive),
                           44.0f * (1.0f - tween::quint_out(arrive)));
        }
        else
        {
            draw_hero_item(list, shown_, 1.0f, 30.0f * (1.0f - in));
        }
        list.pop_opacity();
    }

    void draw_shelves(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float nudge = ui::shake(nudge_.value, clock_, 16.0f, 8.0f) * nudge_direction_;
        for (int r = 0; r < static_cast<int>(shelves_.size()); ++r)
        {
            const Shelf &shelf = shelves_[static_cast<std::size_t>(r)];
            // Shelves above the focused one are gone; the one below peeks in.
            const float distance = static_cast<float>(r) - row_position_.value;
            const float visible =
                tween::clamp01(1.0f + distance * 2.5f) *
                (distance > 0.0f ? 1.0f - 0.45f * tween::clamp01(distance) : 1.0f);
            if (visible <= 0.01f)
                continue;
            const float in = tween::stagger(age_, 3 + r, 0.09f, 0.6f);
            list.push_opacity(visible * in);
            const float y = kShelfY + distance * kShelfPitch + 40.0f * (1.0f - in);
            ui::text(list, fonts.semibold, shelf.title, kMargin, y - 62, 24,
                     kWhite.with_alpha(r == row_ ? 0.95f : 0.6f));
            for (int c = 0; c < static_cast<int>(shelf.items.size()); ++c)
            {
                Rect rect = card_rect(r, c, false);
                rect.y = y;
                if (rect.x > gfx::kVirtualWidth || rect.x + rect.w < -80.0f)
                    continue;
                const bool focused = r == row_ && c == shelf.column;
                if (focused)
                    continue; // drawn last, on top
                const demo::Item &it = item(shelf.items[static_cast<std::size_t>(c)]);
                // Cards to the right of a grown card make room for it.
                if (r == row_ && c > shelf.column)
                    rect.x += kCard * (kCardGrow - 1.0f);
                list.image(it.cover, rect, gfx::kCanvasUv, kWhite.with_alpha(0.82f), 22);
            }
            list.pop_opacity();
        }

        // The focused card: grown, lifted, ringed.
        const Shelf &shelf = shelves_[static_cast<std::size_t>(row_)];
        const demo::Item &it = item(shelf.items[static_cast<std::size_t>(shelf.column)]);
        Rect ring = ring_.value();
        ring.x += nudge;
        const float in = tween::stagger(age_, 3 + row_, 0.09f, 0.6f);
        list.push_opacity(in);
        list.shadow({ring.x, ring.y + 18, ring.w, ring.h}, 26, 34, Color::rgb(0x000000, 0.6f));
        list.glow(ring, 26, 26, it.accent.with_alpha(0.4f + 0.15f * ui::breathe(clock_)));
        list.image(it.cover, ring, gfx::kCanvasUv, kWhite, 26);
        list.bordered_rect(ring.inset(-5), 30, Color::rgb(0x000000, 0.0f), 4, kWhite);
        if (psp5::Library().IsFavorite(static_cast<std::size_t>(focused_item())))
            list.star(ring.x + ring.w - 26, ring.y + 26, 13, Color::rgb(0xffd166));
        list.pop_opacity();
    }

    void draw_sheet(gfx::DrawList &list, std::uint32_t glass) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const demo::Item &it = item(focused_item());
        const float t = sheet_.value;
        constexpr float kHeight = 500.0f;
        const Rect sheet{120, gfx::kVirtualHeight - kHeight * t - 40.0f * t + 60.0f * (1.0f - t),
                         1680, kHeight};
        list.push_opacity(tween::clamp01(t * 1.4f));
        list.shadow({sheet.x, sheet.y + 20, sheet.w, sheet.h}, 44, 60, Color::rgb(0x000000, 0.5f));
        // Solid, not frosted. The kit's glass shows the shelf through the panel,
        // and over a cover's own artwork the text on it had to compete with the
        // picture it was describing.
        (void)glass;
        list.rounded_rect(sheet, 44, Color::rgb(0x000000));
        list.bordered_rect(sheet, 44, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.22f));

        if (cheats_open_)
        {
            draw_cheats(list, sheet);
            list.pop_opacity();
            return;
        }

        const Rect art{sheet.x + 56, sheet.y + 56, 300, 300};
        list.image(it.cover, art, gfx::kCanvasUv, kWhite, 28);
        const float x = art.x + art.w + 56;
        ui::text(list, fonts.semibold, ui::upper(it.genre), x, sheet.y + 92, 20, it.accent,
                 gfx::Align::left, 4.0f);
        {
            // The actions start here, so the title stops before them - it used
            // to be set at 60 whatever its length and ran straight under Play.
            const float actions_left = sheet.x + sheet.w - 56 - 420;
            std::string shown = it.title;
            const float size = fit_title(&shown, 60.0f, 34.0f, actions_left - 32.0f - x);
            ui::text(list, fonts.display, shown, x - 2, sheet.y + 156 - (60.0f - size) * 0.35f,
                     size, kWhite);
        }
        const psp5::GameEntry &file = entry(focused_item());
        ui::text(list, fonts.regular, psp5::PlayedLabel(file.disc_id), x, sheet.y + 226, 26,
                 kWhite.with_alpha(0.82f));

        // Three stat tiles. The kit gauged a rating here; these state what the
        // file is, which is what psp5 actually knows about it.
        const float tiles_y = sheet.y + 300;
        const char *labels[] = {"FORMAT", "SIZE", "ADDED"};
        const std::string values[] = {file.format, file.size_text, file.date_text};
        for (int i = 0; i < 3; ++i)
        {
            const float appear = tween::stagger(t, i, 0.12f, 0.6f);
            const Rect tile{x + static_cast<float>(i) * 236, tiles_y + 24 * (1.0f - appear), 216,
                            120};
            list.push_opacity(appear);
            list.rounded_rect(tile, 22, kWhite.with_alpha(0.08f));
            ui::text(list, fonts.semibold, labels[i], tile.x + 22, tile.y + 36, 15,
                     kWhite.with_alpha(0.55f), gfx::Align::left, 3.0f);
            // A date is twice the length of a size, so it is set smaller rather
            // than allowed to run out of its tile.
            ui::text(list, fonts.semibold, values[i], tile.x + 22, tile.y + 92, i == 2 ? 26 : 40,
                     kWhite);
            list.pop_opacity();
        }

        // Actions: one highlight that springs between the rows.
        const bool resumable = psp5::HasSaveState(entry(focused_item()).disc_id);
        const char *actions[kActions] = {"Play", "Resume", "Cheats", "Close"};
        const float ax = sheet.x + sheet.w - 56 - 420;
        const float ay = sheet.y + 76;
        list.rounded_rect({ax, ay + action_position_.value * 84, 420, 72}, 36, kWhite);
        for (int i = 0; i < kActions; ++i)
        {
            const bool focused = i == action_;
            const float y = ay + static_cast<float>(i) * 84;
            if (!focused)
                list.bordered_rect({ax, y, 420, 72}, 36, kWhite.with_alpha(0.06f), 1.5f,
                                   kWhite.with_alpha(0.18f));
            const bool idle = i == 1 && !resumable;
            ui::text(list, fonts.semibold, actions[i], ax + 36, y + 46, 26,
                     focused      ? Color::rgb(0x0b0d16)
                     : idle       ? kWhite.with_alpha(0.35f)
                                  : kWhite.with_alpha(0.9f));
        }
        list.pop_opacity();
    }

    // The codes in the game's cheat file, inside the sheet that was showing its
    // details. Each row is one `_C` heading; Cross switches it on or off and
    // Circle writes the file.
    // The master switch, then one row per code - but only while the switch is
    // on. With it off the codes do nothing, and a list of switches that cannot
    // take effect invites turning them on and wondering why nothing happened.
    // Row 0 is the master switch. Row 1 imports this game's codes out of
    // PSP/Cheats/cheat.db, when there is one to read - an action rather than a
    // switch, so it sits above the codes instead of among them. The codes
    // follow, and are hidden entirely while cheats are off.
    bool cheat_import_row(int row) const
    {
        return row == 1 && psp5::CheatsEnabled() && psp5::Cheats::DatabaseExists();
    }

    // Which code a row shows, or -1 where the row is not a code.
    int cheat_code_at(int row) const
    {
        const int first = 1 + (cheat_import_row(1) ? 1 : 0);
        return row >= first ? row - first : -1;
    }

    int cheat_rows() const
    {
        if (!psp5::CheatsEnabled())
            return 1;
        return 1 + (cheat_import_row(1) ? 1 : 0) +
               static_cast<int>(psp5::CheatList().size());
    }

    // A switch, not a tick box: a track with the knob at one end, filled in the
    // accent colour when on. The same control the in-game menu uses.
    void draw_toggle(gfx::DrawList &list, float x, float y, bool on) const
    {
        constexpr float kWidth = 56.0f;
        constexpr float kHeight = 28.0f;
        const float radius = kHeight * 0.5f;
        list.rounded_rect({x, y, kWidth, kHeight}, radius,
                          on ? palette_[3].value() : kWhite.with_alpha(0.22f));
        const float knob = on ? x + kWidth - radius : x + radius;
        list.circle(knob, y + radius, radius - 4.0f,
                    on ? Color::rgb(0x0b0d16) : kWhite.with_alpha(0.85f));
    }

    // This game's achievements, as a bar down the right edge - the shape the
    // console uses for a list that belongs beside what is on screen rather than
    // instead of it. The same shape as the in-game bar on R1 + R3.
    // The kit's d-pad glyph has all four arms, which says "any direction". In
    // this bar up and down move the cursor and left and right move the filter,
    // so each hint shows only its own axis.
    void draw_dpad_axis(gfx::DrawList &list, const ui::GlyphStyle &style, float cx, float cy,
                        float size, bool vertical) const
    {
        const float half = size * 0.5f;
        list.circle(cx, cy, half, style.body);
        list.ring(cx, cy, half - 1.0f, 1.5f, style.edge);
        const float arm = size * 0.3f;
        const float thick = size * 0.2f;
        if (vertical)
            list.rounded_rect({cx - thick * 0.5f, cy - arm, thick, arm * 2}, thick * 0.3f,
                              style.ink);
        else
            list.rounded_rect({cx - arm, cy - thick * 0.5f, arm * 2, thick}, thick * 0.3f,
                              style.ink);
    }

    // The bar's own hint row, laid out from the right like the kit's.
    void draw_achievement_hints(gfx::DrawList &list, const ui::GlyphStyle &style) const
    {
        const ui::Fonts &fonts = context_.fonts;
        constexpr float kSize = 40.0f;
        constexpr float kCy = 1010.0f;
        constexpr float kIconGap = 12.0f;
        constexpr float kItemGap = 44.0f;
        constexpr float kText = 26.0f;
        const char *labels[3] = {"Move", "Filter", "Back"};

        // Each glyph is as wide as its own shape, and draw_button takes its left
        // edge rather than its centre.
        const auto glyph_width = [&](int i) {
            return ui::button_width(i == 2 ? ui::Button::circle : ui::Button::dpad, kSize);
        };

        float total = 0.0f;
        for (int i = 0; i < 3; ++i)
            total += glyph_width(i) + kIconGap + fonts.regular.font->measure(labels[i], kText) +
                     (i < 2 ? kItemGap : 0.0f);

        float x = 1824.0f - total;
        for (int i = 0; i < 3; ++i)
        {
            const float gw = glyph_width(i);
            if (i == 2)
                ui::draw_button(list, fonts, style, ui::Button::circle, x, kCy, kSize);
            else
                draw_dpad_axis(list, style, x + gw * 0.5f, kCy, kSize, i == 0);
            x += gw + kIconGap;
            ui::text(list, fonts.regular, labels[i], x, kCy + kText * 0.35f, kText, style.label);
            x += fonts.regular.font->measure(labels[i], kText) + kItemGap;
        }
    }

    void draw_achievements(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const psp5::GameAchievements &view = psp5::GameAchievementList();
        constexpr float kWidth = 620.0f;
        const float x = gfx::kVirtualWidth - kWidth;

        list.rounded_rect({0, 0, x, gfx::kVirtualHeight}, 0, Color::rgb(0x05070f, 0.72f));
        list.rounded_rect({x, 0, kWidth, gfx::kVirtualHeight}, 0, Color::rgb(0x000000));
        list.rounded_rect({x, 0, 1.5f, gfx::kVirtualHeight}, 0, kWhite.with_alpha(0.18f));

        const float left = x + 44;
        const float inner = kWidth - 88;
        ui::text(list, fonts.semibold, "ACHIEVEMENTS", left, 92, 20, palette_[3].value(),
                 gfx::Align::left, 4.0f);
        // Left and right move it; the hint row says so.
        ui::text(list, fonts.semibold,
                 psp5::achievementFilterName(psp5::achievementFilter()), x + kWidth - 44, 92, 20,
                 kWhite.with_alpha(0.75f), gfx::Align::right, 4.0f);
        ui::text(list, fonts.display,
                 fonts.display.font->fit(item(focused_item()).title, 38, inner), left, 146, 38,
                 kWhite);

        using State = psp5::GameAchievements::State;
        const State state = view.state();
        if (state != State::ready)
        {
            const char *said =
                state == State::working       ? "Looking this game up..."
                : state == State::notSignedIn ? "Sign in to RetroAchievements in settings."
                : state == State::unsupported ? "This file cannot be identified."
                                              : "RetroAchievements has none for this game.";
            ui::paragraph(list, fonts.regular, said, left, 216, 25, inner, 34,
                          kWhite.with_alpha(0.75f), 3);
            if (state == State::working)
            {
                const float width = 150.0f;
                const float travel = inner - width;
                const float at = (std::sin(clock_ * 1.9f) * 0.5f + 0.5f) * travel;
                list.rounded_rect({left, 290, inner, 4}, 2, kWhite.with_alpha(0.14f));
                list.rounded_rect({left + at, 290, width, 4}, 2, palette_[3].value());
            }
            return;
        }

        // How far along the game is.
        ui::text(list, fonts.semibold, view.summary(), left, 206, 30, kWhite);
        ui::text(list, fonts.regular, view.points(), x + kWidth - 44, 206, 22,
                 kWhite.with_alpha(0.6f), gfx::Align::right);
        list.rounded_rect({left, 226, inner, 6}, 3, kWhite.with_alpha(0.14f));
        list.rounded_rect({left, 226, inner * view.fraction(), 6}, 3, palette_[3].value());

        const int count = static_cast<int>(view.size());
        constexpr float kDetail = 19.0f;  // the description's size
        constexpr float kLine = 22.0f;    // ... and its line height
        const float top = 284.0f;
        const float textW = inner - 72 - 54;

        // What an achievement asks for is a sentence, and a sentence is as long
        // as it is - but giving every row room for its whole sentence left four
        // of them on a screen that holds ten. So the row being read is the one
        // that opens: it shows the description in full, and the rest show the
        // name and what they are worth.
        const auto row_height = [&](int index) {
            const psp5::GameAchievement &row = view.row(static_cast<std::size_t>(index));
            if (row.header)
                return 44.0f;
            if (index != achievement_ || row.detail.empty())
                return 72.0f;
            const std::size_t lines =
                fonts.regular.font->wrap(row.detail, kDetail, textW + 40).size();
            return 58.0f + static_cast<float>(lines) * kLine + 16.0f;
        };

        // Enough of the list is skipped to keep the focused row on screen.
        // Heights vary, so the first visible row is found by walking back from
        // the cursor until the rows below it fill the bar.
        const float bottom = kHintsTop - 16.0f;
        const float space = bottom - top;
        int first = std::clamp(achievement_, 0, std::max(0, count - 1));
        float used = count > 0 ? row_height(first) : 0.0f;
        while (first > 0)
        {
            const float next = row_height(first - 1);
            if (used + next > space)
                break;
            used += next;
            --first;
        }

        float y = top;
        for (int i = first; i < count; ++i)
        {
            const psp5::GameAchievement &row = view.row(static_cast<std::size_t>(i));
            const bool focused = i == achievement_;
            const float height = row_height(i);
            // Whole rows only. A row that would cross the hints is left for the
            // next screenful instead of being drawn over them.
            if (y + height > bottom)
                break;

            if (row.header)
            {
                ui::text(list, fonts.semibold, ui::upper(row.title), left, y + 30, 17,
                         kWhite.with_alpha(0.45f), gfx::Align::left, 3.0f);
                y += height;
                continue;
            }

            if (focused)
            {
                list.rounded_rect({x, y, kWidth, height - 8}, 0, kWhite.with_alpha(0.1f));
                list.rounded_rect({x, y, 4, height - 8}, 0, palette_[3].value());
            }

            // The badge RetroAchievements shows, once it has arrived. Until
            // then a plain mark, rather than an empty square.
            const Rect mark{left, y + 12, 56, 56};
            if (row.badge)
            {
                list.image(row.badge, mark, gfx::kFullUv,
                           row.unlocked ? kWhite : kWhite.with_alpha(0.55f), 10);
            }
            else if (row.unlocked)
            {
                list.rounded_rect(mark, 12, palette_[3].value());
            }
            else
            {
                list.bordered_rect(mark, 12, kWhite.with_alpha(0.04f), 2,
                                   kWhite.with_alpha(0.26f));
            }

            const float textX = left + 72;
            ui::text(list, focused ? fonts.semibold : fonts.regular,
                     fonts.regular.font->fit(row.title, 24, textW), textX, y + 34, 24,
                     kWhite.with_alpha(row.unlocked ? 1.0f : 0.7f));
            if (focused && !row.detail.empty())
                ui::paragraph(list, fonts.regular, row.detail, textX, y + 58, kDetail, textW + 40,
                              kLine, kWhite.with_alpha(0.5f));
            if (!row.points.empty())
                ui::text(list, fonts.semibold, row.points, x + kWidth - 44, y + 34, 21,
                         kWhite.with_alpha(0.5f), gfx::Align::right);
            if (row.progress > 0.0f && !row.unlocked)
            {
                list.rounded_rect({textX, y + height - 18, textW, 3}, 2, kWhite.with_alpha(0.14f));
                list.rounded_rect({textX, y + height - 18, textW * row.progress, 3}, 2,
                                  palette_[3].value());
            }
            y += height;
        }

        // Counted in achievements, not in rows: the group labels are rows too,
        // so the first achievement sits at index 1 and called itself the second.
        int total = 0;
        int at = 0;
        for (int i = 0; i < count; ++i)
        {
            if (view.row(static_cast<std::size_t>(i)).header)
                continue;
            ++total;
            if (i <= achievement_)
                at = total;
        }
        char counter[48];
        std::snprintf(counter, sizeof(counter), "%d of %d", at, total);
        ui::text(list, fonts.regular, counter, left, 258, 20, kWhite.with_alpha(0.55f));
    }

    void draw_cheats(gfx::DrawList &list, const Rect &sheet) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const psp5::Cheats &cheats = psp5::CheatList();
        const std::span<const psp5::CheatEntry> codes = cheats.items();
        const float x = sheet.x + 56;

        ui::text(list, fonts.semibold, "CHEATS", x, sheet.y + 76, 20, palette_[3].value(),
                 gfx::Align::left, 4.0f);
        ui::text(list, fonts.display, item(focused_item()).title, x - 2, sheet.y + 140, 44, kWhite);

        constexpr int kVisible = 7;
        constexpr float kRow = 46.0f;
        const int count = cheat_rows();
        const int first = std::clamp(cheat_ - kVisible / 2, 0, std::max(0, count - kVisible));
        const float top = sheet.y + 186;

        for (int i = first; i < std::min(count, first + kVisible); ++i)
        {
            const bool master = i == 0;
            const bool importer = cheat_import_row(i);
            const int code = cheat_code_at(i);
            const bool focused = i == cheat_;
            const float y = top + static_cast<float>(i - first) * kRow;
            // The codes are indented under the switch and the import that
            // govern them, rather than reading as three of the same thing.
            const float indent = code >= 0 ? 34.0f : 0.0f;
            if (focused)
                list.rounded_rect({x - 18 + indent, y - 30, sheet.w - 76 - indent, kRow - 6}, 14,
                                  kWhite.with_alpha(0.12f));

            if (importer)
            {
                // No switch: this one does something rather than holding a
                // state, so it is a line of text on its own.
                ui::text(list, focused ? fonts.semibold : fonts.regular, kImportLabel, x + 76, y,
                         26, kWhite.with_alpha(focused ? 1.0f : 0.75f));
                continue;
            }

            const bool on = master ? psp5::CheatsEnabled()
                                   : codes[static_cast<std::size_t>(code)].enabled;
            const std::string &label =
                master ? kCheatsEnabledLabel : codes[static_cast<std::size_t>(code)].name;
            draw_toggle(list, x + indent, y - 23, on);
            ui::text(list, focused || master ? fonts.semibold : fonts.regular, label,
                     x + indent + 76, y, 26, kWhite.with_alpha(focused ? 1.0f : 0.75f));
        }

        // Under the last row that was drawn, whichever rows those were. Written
        // as top + kRow when the master switch was the only one above it, this
        // then sat on top of the import row.
        const float y = top + static_cast<float>(std::min(count, kVisible)) * kRow + 16.0f;

        if (!psp5::CheatsEnabled())
        {
            char note[96];
            std::snprintf(note, sizeof(note), "%u code%s in this game's file.",
                          (unsigned)codes.size(), codes.size() == 1 ? "" : "s");
            ui::text(list, fonts.regular, codes.empty() ? "Turn this on to use cheats." : note, x,
                     y, 26, kWhite.with_alpha(0.75f));
            if (!codes.empty())
                ui::text(list, fonts.regular, "Turn the switch on to choose between them.", x,
                         y + 38, 24, kWhite.with_alpha(0.55f));
        }
        else if (!cheat_notice_.empty())
        {
            ui::text(list, fonts.regular, cheat_notice_, x, y, 24, palette_[3].value());
        }
        else if (codes.empty())
        {
            // Worth showing on its own: the master switch is what decides
            // whether a file copied here later takes effect.
            ui::text(list, fonts.regular, "No cheat file for this game. Copy a CWCheat .ini to:", x,
                     y, 24, kWhite.with_alpha(0.7f));
            ui::text(list, fonts.regular, cheats.path(), x, y + 38, 24, palette_[3].value());
        }
        else if (count > kVisible)
        {
            char text[48];
            std::snprintf(text, sizeof(text), "%d of %d", cheat_ + 1, count);
            ui::text(list, fonts.regular, text, sheet.x + sheet.w - 56, sheet.y + 76, 22,
                     kWhite.with_alpha(0.6f), gfx::Align::right);
        }
    }

    void draw_hints(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ui::GlyphStyle style = ui::GlyphStyle::dark();
        if (settings_open_)
        {
            const ui::Hint hints[] = {{ui::Button::dpad, "Change"},
                                      {ui::Button::options, "Close"}};
            ui::draw_hints(list, fonts, style, hints, 2, 1824, true);
            return;
        }
        // With no games, every hint below names a control that does nothing.
        if (empty())
            return;
        if (achievements_open_)
        {
            draw_achievement_hints(list, style);
        }
        else if (cheats_open_)
        {
            const ui::Hint hints[] = {{ui::Button::cross, "Toggle"},
                                      {ui::Button::circle, "Save and close"}};
            ui::draw_hints(list, fonts, style, hints, 2, 1824, true);
        }
        else if (sheet_open_)
        {
            const ui::Hint hints[] = {{ui::Button::cross, "Choose"}, {ui::Button::circle, "Back"}};
            ui::draw_hints(list, fonts, style, hints, 2, 1824, true);
        }
        else
        {
            list.push_opacity(tween::stagger(age_, 8, 0.08f, 0.5f) * (1.0f - sheet_.value));
            const ui::Hint hints[] = {{ui::Button::r2, "Achievements"},
                                      {ui::Button::cross, "Details"},
                                      {ui::Button::square, "Game settings"},
                                      {ui::Button::triangle, "Favorite"},
                                      {ui::Button::options, "Settings"}};
            ui::draw_hints(list, fonts, style, hints, 4, 1824, true);
            list.pop_opacity();
        }
    }

    app::Context &context_;
    std::vector<Shelf> shelves_;
    psp5::GameView view_ = psp5::GameView::recent; // how the library is ordered
    bool settings_open_ = false;                   // OPTIONS opened the settings panel
    int setting_ = 0;                              // its focused row
    Typing typing_ = Typing::none;                 // which answer the keyboard is taking
    int sign_in_button_ = 0;                       // the focused button of the sign-in dialog
    bool sign_out_asking_ = false;                 // the sign-out dialog is up
    bool sign_out_armed_ = false;                  // ... and the pad has been let go since
    int sign_out_button_ = 1;
    std::string typed_user_;
    int row_ = 0;
    float age_ = 0.0f;   // seconds since enter(): drives the entrance
    float clock_ = 0.0f; // free-running time for idle motion
    int shown_ = 0;      // the title the hero shows
    int previous_ = 0;   // ... and the one it is fading out
    tween::Timer hero_;
    ui::SpringColor palette_[4];
    tween::Spring row_position_;
    ui::SpringRect ring_;
    ui::Pulse nudge_;
    float nudge_direction_ = 0.0f;
    ui::Pulse star_;
    bool sheet_open_ = false;
    bool cheats_open_ = false; // the sheet is showing the cheat list, not the details
    bool achievements_open_ = false; // ... or this game's achievements
    int achievement_ = 0;
    std::string cheat_notice_; // what the last import did, shown under the list
    int cheat_ = 0;            // the focused code
    tween::Spring sheet_;
    int action_ = 0;
    tween::Spring action_position_;
};

} // namespace

std::unique_ptr<app::Concept> make_aurora(app::Context &context)
{
    return std::make_unique<Aurora>(context);
}

} // namespace hui::concepts
