#include "ui/browser/action_shortcuts.h"

#include "config/keybinds.h"
#include "core/input/key_modifiers.h"

#include <array>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <xkbcommon/xkbcommon.h>

namespace {
  using Chords = std::array<std::optional<KeyChord>, luciole::kBrowserActionCount>;

  using A = luciole::BrowserAction;
  using K = luciole::KeybindCommand;

  std::optional<K> keybindFor(A action) {
    switch (action) {
    case A::Back:
      return K::Back;
    case A::Forward:
      return K::Forward;
    case A::Up:
      return K::Up;
    case A::Home:
      return K::HomeFolder;
    case A::Refresh:
      return K::Refresh;
    case A::EditLocation:
      return K::EditLocation;
    case A::Search:
      return K::Search;
    case A::Grid:
      return K::ViewGrid;
    case A::List:
      return K::ViewList;
    case A::Miller:
      return K::ViewMiller;
    case A::ShowHidden:
      return K::ToggleHidden;
    case A::ShowSidebar:
      return K::ToggleSidebar;
    case A::ShowPreview:
      return K::TogglePreview;
    case A::QuickPreview:
      return K::QuickPreview;
    case A::Open:
      return K::Open;
    case A::CutFiles:
      return K::Cut;
    case A::CopyFiles:
      return K::Copy;
    case A::PasteFiles:
      return K::Paste;
    case A::Rename:
      return K::Rename;
    case A::Trash:
      return K::Trash;
    case A::DeletePermanently:
      return K::DeletePermanently;
    case A::Properties:
      return K::Properties;
    case A::Terminal:
      return K::Terminal;
    case A::CreateFolder:
      return K::CreateFolder;
    case A::CreateFile:
      return K::CreateFile;
    case A::SelectAll:
      return K::SelectAll;
    case A::InvertSelection:
      return K::InvertSelection;
    case A::SelectPattern:
      return K::SelectPattern;
    case A::ClearSelection:
      return K::ClearSelection;
    case A::Undo:
      return K::Undo;
    case A::Redo:
      return K::Redo;
    case A::Settings:
      return K::Settings;
    case A::KeyboardShortcuts:
      return K::KeyboardShortcuts;
    default:
      return std::nullopt;
    }
  }

  std::optional<KeyChord> parseShortcut(std::string_view shortcut) {
    if (shortcut.empty()) {
      return std::nullopt;
    }
    std::string spec(shortcut);
    // The table shows the glyph; xkb names the key "comma".
    if (spec.ends_with("+,")) {
      spec.replace(spec.size() - 1, 1, "comma");
    }
    auto chord = parseKeyChordSpec(spec);
    if (chord) {
      chord->sym = xkb_keysym_to_lower(chord->sym);
    }
    return chord;
  }

  const Chords& chords() {
    static const Chords table = [] {
      Chords parsed;
      for (const auto& definition : luciole::ActionRegistry::definitions()) {
        parsed[std::to_underlying(definition.action)] = parseShortcut(definition.shortcut);
      }
      return parsed;
    }();
    return table;
  }
} // namespace

const std::optional<KeyChord>& luciole::actionChord(BrowserAction action) {
  return chords()[std::to_underlying(action)];
}

bool luciole::shortcutMatches(BrowserAction action, std::uint32_t sym, std::uint32_t modifiers) {
  if (const auto command = keybindFor(action)) {
    return keybindMatches(*command, sym, modifiers);
  }
  const auto& chord = actionChord(action);
  // Shift turns letter keysyms upper case, so shifted chords accept either case.
  return chord
      && chord->modifiers == modifiers
      && (chord->sym == sym || ((modifiers & KeyMod::Shift) != 0 && xkb_keysym_to_lower(sym) == chord->sym));
}

std::optional<luciole::BrowserAction>
luciole::shortcutAction(std::initializer_list<BrowserAction> actions, std::uint32_t sym, std::uint32_t modifiers) {
  for (const auto action : actions) {
    if (shortcutMatches(action, sym, modifiers)) {
      return action;
    }
  }
  return std::nullopt;
}

std::string luciole::actionShortcutLabel(BrowserAction action) {
  if (const auto command = keybindFor(action)) {
    return keybindShortcutLabel(*command);
  }
  return std::string(ActionRegistry::definition(action).shortcut);
}

std::string luciole::shortcutTooltip(std::string_view text, BrowserAction action) {
  const std::string shortcut = actionShortcutLabel(action);
  return shortcut.empty() ? std::string(text) : std::format("{} ({})", text, shortcut);
}

std::string luciole::shortcutTooltip(std::string_view text, KeybindCommand command) {
  const std::string shortcut = keybindShortcutLabel(command);
  return shortcut.empty() ? std::string(text) : std::format("{} ({})", text, shortcut);
}
