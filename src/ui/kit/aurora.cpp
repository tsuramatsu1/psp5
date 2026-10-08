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
#include "ui/PS5Cheats.h"
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
const std::string kQuitLabel = "Close psp5";
constexpr float kShelfY = 730.0f;     // top of the focused shelf's cards
constexpr float kShelfPitch = 304.0f; // distance between shelves
constexpr int kActions = 3;

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
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        age_ += dt;
        clock_ += dt;
        // The tab strip reaches past the shelf, so it is read before anything
        // that depends on there being a game to focus. Not while the details
        // sheet is up, though: there L1/R1 would move the screen out from under
        // an open panel.
        // OPTIONS is the settings, from anywhere on the home screen. It is a
        // console's button for this, and it leaves the face buttons to the shelf.
        if (input.is_pressed(Action::menu))
        {
            toggle_settings(feedback);
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
        if (back > 0.01f)
            list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                              Color::rgb(0x05070f, 0.45f * back));

        if (back > 0.01f)
        {
            frame.glass = true;
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

    void toggle_settings(app::Feedback &feedback)
    {
        settings_open_ = !settings_open_;
        if (settings_open_)
        {
            setting_ = 0;
            psp5::SettingsPanel().Reload();
            feedback.play(audio::Cue::open);
            return;
        }
        // Written once, on the way out, rather than on every keypress: PPSSPP's
        // save rewrites the whole ini.
        if (psp5::SettingsPanel().dirty())
        {
            psp5::SettingsPanel().Save();
            feedback.play(audio::Cue::saved);
        }
        else
        {
            feedback.play(audio::Cue::back);
        }
    }

    // The settings, and one row past them: closing the title. That row is here
    // rather than on a gesture of its own because OPTIONS now opens this panel,
    // and the foot of a settings list is where someone looks for a way out.
    int settings_rows() const
    {
        return static_cast<int>(psp5::SettingsPanel().size()) + 1;
    }

    bool quit_row(int row) const
    {
        return row == settings_rows() - 1;
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
            const int next = std::clamp(setting_ + (input.nav == Direction::down ? 1 : -1), 0,
                                        count - 1);
            if (next != setting_)
            {
                setting_ = next;
                feedback.play(audio::Cue::focus, 1.0f, 0.0f);
            }
            else
            {
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
                nudge_direction_ = 0.0f;
                nudge_.trigger();
            }
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
            if (settings.Adjust(static_cast<std::size_t>(setting_), delta))
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
    }

    void update_cheats(const InputFrame &input, app::Feedback &feedback)
    {
        psp5::Cheats &cheats = psp5::CheatList();
        const int count = static_cast<int>(cheats.size());
        if (input.is_pressed(Action::back) || count == 0)
        {
            if (input.is_pressed(Action::back))
            {
                if (cheats.dirty())
                {
                    cheats.Save();
                    feedback.play(audio::Cue::saved);
                }
                else
                {
                    feedback.play(audio::Cue::back);
                }
                cheats_open_ = false;
            }
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
            cheats.Toggle(static_cast<std::size_t>(cheat_));
            feedback.play(audio::Cue::toggle);
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
                // The cheat file is read now rather than when the shelf was
                // built: it is one small file, and reading it on the way in
                // means a file copied since the title started is still found.
                psp5::CheatList().Load(entry(focused_item()).disc_id);
                cheat_ = 0;
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
            sheet_open_ = false;
            cheats_open_ = false;
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

        ui::draw_button(list, fonts, style, ui::Button::l1, kMargin + 16, y - 9, 30);
        float x = kMargin + 56;
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
        ui::draw_button(list, fonts, style, ui::Button::r1, x - 10, y - 9, 30);

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
        ui::text(list, fonts.display, "Settings", kMargin, 190 - 20 * (1.0f - in), 64, kWhite);
        // The focused row's explanation, in the one fixed place: under the title
        // rather than under the list, which would be past the foot of the screen
        // once every row is on it.
        const char *hint = quit_row(setting_)
                               ? "Closes psp5 and returns to the console."
                               : rows[static_cast<std::size_t>(setting_)].hint.c_str();
        ui::text(list, fonts.regular, hint, kMargin, 243, 25, kWhite.with_alpha(0.7f));
        list.pop_opacity();

        // Eleven rows and a title have to fit inside 1080, above the hint bar.
        constexpr float kRow = 56.0f;
        const float top = 300.0f;
        const float width = gfx::kVirtualWidth - kMargin * 2;
        const float shake = ui::shake(nudge_.value, clock_, 16.0f, 8.0f);
        for (int i = 0; i < settings_rows(); ++i)
        {
            const bool quit = quit_row(i);
            const bool focused = i == setting_;
            const float appear = tween::stagger(age_, 2 + i, 0.05f, 0.5f);
            list.push_opacity(appear);
            const Rect rect{kMargin, top + static_cast<float>(i) * kRow + 24 * (1.0f - appear),
                            width, kRow - 8};
            if (focused)
            {
                list.rounded_rect({rect.x + shake, rect.y, rect.w, rect.h}, 18,
                                  kWhite.with_alpha(0.12f));
                list.rounded_rect({rect.x + shake, rect.y + 10, 5, rect.h - 20}, 3,
                                  palette_[3].value());
            }
            const std::string &label =
                quit ? kQuitLabel : rows[static_cast<std::size_t>(i)].label;
            ui::text(list, focused ? fonts.semibold : fonts.regular, label, rect.x + 30 + shake,
                     rect.y + 34, 26, kWhite.with_alpha(focused ? 1.0f : 0.72f));
            if (!quit)
                ui::text(list, fonts.semibold, rows[static_cast<std::size_t>(i)].value,
                         rect.x + rect.w - 30 + shake, rect.y + 34, 26,
                         focused ? palette_[3].value() : kWhite.with_alpha(0.6f),
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
            constexpr float kTitleRight = 1316.0f - 48.0f;
            const float room = kTitleRight - x;
            float size = 88.0f;
            while (size > 52.0f && fonts.display.font->measure(it.title, size) > room)
                size -= 4.0f;
            std::string shown = it.title;
            if (fonts.display.font->measure(shown, size) > room)
            {
                while (shown.size() > 4 &&
                       fonts.display.font->measure(shown + "...", size) > room)
                    shown.pop_back();
                shown += "...";
            }
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
        ui::text(list, fonts.regular, text, x, 358, 26, kWhite.with_alpha(0.78f));
        ui::paragraph(list, fonts.regular, it.blurb, x, 414, 26, 820, 36, kWhite.with_alpha(0.7f),
                      2);

        const Rect play{x, 540, 220, 64};
        list.glow(play, 32, 18, it.accent.with_alpha(0.35f));
        list.rounded_rect(play, 32, kWhite);
        ui::draw_button(list, fonts, ui::GlyphStyle::light(), ui::Button::cross, play.x + 22,
                        play.cy(), 34);
        ui::text(list, fonts.semibold, "Play", play.x + 72, play.cy() + 10, 28,
                 Color::rgb(0x0b0d16));
        const Rect more{x + 240, 540, 64, 64};
        list.bordered_rect(more, 32, kWhite.with_alpha(0.1f), 2, kWhite.with_alpha(0.3f));
        const bool starred = psp5::Library().IsFavorite(static_cast<std::size_t>(index));
        list.star(more.cx(), more.cy(), 16 + 10 * star_.value,
                  starred ? Color::rgb(0xffd166) : kWhite.with_alpha(0.85f), starred ? 0.0f : 2.5f);
        list.pop_opacity();
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
        // Frosted panel: the blurred screen, a tint, then a hairline of light.
        list.glass(glass, sheet, 44, kWhite);
        list.rounded_rect(sheet, 44,
                          gfx::mix(it.dark, Color::rgb(0x0b0d16), 0.5f).with_alpha(0.62f));
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
        ui::text(list, fonts.display, it.title, x - 2, sheet.y + 156, 60, kWhite);
        ui::paragraph(list, fonts.regular, it.blurb, x, sheet.y + 208, 25, 700, 36,
                      kWhite.with_alpha(0.82f), 2);

        // Three stat tiles. The kit gauged a rating here; these state what the
        // file is, which is what psp5 actually knows about it.
        const psp5::GameEntry &file = entry(focused_item());
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
        const char *actions[kActions] = {"Play", "Cheats", "Close"};
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
            ui::text(list, fonts.semibold, actions[i], ax + 36, y + 46, 26,
                     focused ? Color::rgb(0x0b0d16) : kWhite.with_alpha(0.9f));
        }
        list.pop_opacity();
    }

    // The codes in the game's cheat file, inside the sheet that was showing its
    // details. Each row is one `_C` heading; Cross switches it on or off and
    // Circle writes the file.
    void draw_cheats(gfx::DrawList &list, const Rect &sheet) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const psp5::Cheats &cheats = psp5::CheatList();
        const std::span<const psp5::CheatEntry> rows = cheats.items();
        const float x = sheet.x + 56;

        ui::text(list, fonts.semibold, "CHEATS", x, sheet.y + 76, 20, palette_[3].value(),
                 gfx::Align::left, 4.0f);
        ui::text(list, fonts.display, item(focused_item()).title, x - 2, sheet.y + 140, 44, kWhite);

        if (rows.empty())
        {
            ui::text(list, fonts.regular, "No cheat file for this game.", x, sheet.y + 220, 30,
                     kWhite.with_alpha(0.85f));
            ui::text(list, fonts.regular, "Copy a CWCheat .ini here and open this again:", x,
                     sheet.y + 272, 24, kWhite.with_alpha(0.6f));
            ui::text(list, fonts.regular, cheats.path(), x, sheet.y + 312, 24,
                     palette_[3].value());
            return;
        }

        // Only as many rows as the sheet holds, scrolled to keep the focused one
        // on screen. The sheet is a fixed height, so this cannot grow with the
        // file the way a page of its own could.
        constexpr int kVisible = 7;
        constexpr float kRow = 46.0f;
        const int count = static_cast<int>(rows.size());
        int first = cheat_ - kVisible / 2;
        first = std::clamp(first, 0, std::max(0, count - kVisible));
        const float top = sheet.y + 186;

        for (int i = first; i < std::min(count, first + kVisible); ++i)
        {
            const psp5::CheatEntry &row = rows[static_cast<std::size_t>(i)];
            const bool focused = i == cheat_;
            const float y = top + static_cast<float>(i - first) * kRow;
            if (focused)
                list.rounded_rect({x - 18, y - 30, sheet.w - 76, kRow - 6}, 14,
                                  kWhite.with_alpha(0.12f));
            // A filled mark reads as on at a glance; the outline is off.
            const Rect box{x, y - 23, 24, 24};
            if (row.enabled)
            {
                list.rounded_rect(box, 7, palette_[3].value());
                list.line(box.x + 6, box.cy(), box.cx() - 1, box.y + box.h - 7, 3,
                          Color::rgb(0x0b0d16));
                list.line(box.cx() - 1, box.y + box.h - 7, box.x + box.w - 5, box.y + 6, 3,
                          Color::rgb(0x0b0d16));
            }
            else
            {
                list.bordered_rect(box, 7, Color::rgb(0x000000, 0.0f), 2,
                                   kWhite.with_alpha(0.45f));
            }
            ui::text(list, focused ? fonts.semibold : fonts.regular, row.name, x + 44, y, 26,
                     kWhite.with_alpha(focused ? 1.0f : 0.75f));
        }

        if (count > kVisible)
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
        if (cheats_open_)
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
            const ui::Hint hints[] = {{ui::Button::cross, "Details"},
                                      {ui::Button::triangle, "Favorite"},
                                      {ui::Button::options, "Settings"}};
            ui::draw_hints(list, fonts, style, hints, 3, 1824, true);
            list.pop_opacity();
        }
    }

    app::Context &context_;
    std::vector<Shelf> shelves_;
    psp5::GameView view_ = psp5::GameView::recent; // how the library is ordered
    bool settings_open_ = false;                   // OPTIONS opened the settings panel
    int setting_ = 0;                              // its focused row
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
