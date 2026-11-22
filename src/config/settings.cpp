#include "config/settings.h"

#include "config/keybinds.h"
#include "config/toml_decoder.h"
#include "fs/xdg_dirs.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <format>
#include <iterator>
#include <optional>
#include <print>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

namespace {
  using luciole::BrowserSettings;
  constexpr std::size_t kMaxConfigBytes = 256 * 1024;

  template <class T> struct Named {
    T value;
    std::string_view name;
  };
  constexpr auto kActivationModes = std::to_array<Named<luciole::ActivationMode>>(
      {{luciole::ActivationMode::SingleClick, "single_click"}, {luciole::ActivationMode::DoubleClick, "double_click"}}
  );
  constexpr auto kTypingModes = std::to_array<Named<luciole::TypingMode>>(
      {{luciole::TypingMode::Search, "search"},
       {luciole::TypingMode::Typeahead, "typeahead"},
       {luciole::TypingMode::Filter, "filter"}}
  );
  constexpr auto kDropActions = std::to_array<Named<luciole::DropAction>>(
      {{luciole::DropAction::Menu, "menu"}, {luciole::DropAction::Copy, "copy"}, {luciole::DropAction::Move, "move"}}
  );
  constexpr auto kSortFields = std::to_array<Named<luciole::SortField>>(
      {{luciole::SortField::Name, "name"},
       {luciole::SortField::Size, "size"},
       {luciole::SortField::Modified, "modified"},
       {luciole::SortField::Type, "type"},
       {luciole::SortField::Accessed, "accessed"},
       {luciole::SortField::Created, "created"},
       {luciole::SortField::Owner, "owner"},
       {luciole::SortField::Group, "group"},
       {luciole::SortField::Permissions, "permissions"},
       {luciole::SortField::Location, "location"}}
  );
  constexpr auto kViewModes = std::to_array<Named<luciole::ViewMode>>(
      {{luciole::ViewMode::List, "list"}, {luciole::ViewMode::Grid, "grid"}, {luciole::ViewMode::Miller, "miller"}}
  );
  // File pickers have no Columns view: it enters folders on selection, which choosing a folder needs.
  constexpr auto kChooserViewModes =
      std::to_array<Named<luciole::ViewMode>>({{luciole::ViewMode::List, "list"}, {luciole::ViewMode::Grid, "grid"}});
  constexpr auto kChooserStartFolders = std::to_array<Named<luciole::ChooserStartFolder>>(
      {{luciole::ChooserStartFolder::Home, "home"}, {luciole::ChooserStartFolder::Last, "last"}}
  );
  constexpr auto kDensities = std::to_array<Named<luciole::ViewDensity>>(
      {{luciole::ViewDensity::Compact, "compact"}, {luciole::ViewDensity::Comfortable, "comfortable"}}
  );
  constexpr auto kFilenameTruncations = std::to_array<Named<luciole::FilenameTruncation>>(
      {{luciole::FilenameTruncation::Middle, "middle"}, {luciole::FilenameTruncation::End, "end"}}
  );
  constexpr auto kThumbnailPolicies = std::to_array<Named<luciole::ThumbnailPolicy>>(
      {{luciole::ThumbnailPolicy::Never, "never"},
       {luciole::ThumbnailPolicy::Local, "local"},
       {luciole::ThumbnailPolicy::All, "all"}}
  );

  using luciole::KeybindChord;
  using luciole::KeybindCommand;
  using luciole::KeybindScope;
  namespace KeybindMod = luciole::KeybindMod;

  std::string asciiLower(std::string_view value) {
    std::string result(value);
    for (char& character : result) {
      if (character >= 'A' && character <= 'Z') {
        character = static_cast<char>(character - 'A' + 'a');
      }
    }
    return result;
  }

  std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
      value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
      value.remove_suffix(1);
    }
    return value;
  }

  std::string canonicalKeyName(std::string raw) {
    const std::string lower = asciiLower(raw);
    if (lower == "esc") {
      return "Escape";
    }
    if (lower == "del") {
      return "Delete";
    }
    if (lower == "enter") {
      return "Return";
    }
    if (lower == "kp_enter") {
      return "KP_Enter";
    }
    if (lower == "space" || lower == "spacebar") {
      return "space";
    }
    if (lower == "left") {
      return "Left";
    }
    if (lower == "right") {
      return "Right";
    }
    if (lower == "up") {
      return "Up";
    }
    if (lower == "down") {
      return "Down";
    }
    if (lower == "tab") {
      return "Tab";
    }
    if (lower == "iso_left_tab" || lower == "shift_tab") {
      return "ISO_Left_Tab";
    }
    return raw;
  }

  std::string keysymName(std::uint32_t sym) {
    if (sym == 0) {
      return {};
    }
    std::array<char, 64> buffer{};
    const int length = xkb_keysym_get_name(static_cast<xkb_keysym_t>(sym), buffer.data(), buffer.size());
    if (length <= 0) {
      return {};
    }
    return std::string(buffer.data(), static_cast<std::size_t>(length));
  }

  bool printableConfigKey(std::uint32_t sym) {
    const auto keysym = static_cast<xkb_keysym_t>(sym);
    if ((keysym >= XKB_KEY_KP_Space && keysym <= XKB_KEY_KP_Delete)
        || (keysym >= XKB_KEY_KP_0 && keysym <= XKB_KEY_KP_Equal)) {
      return false;
    }
    const auto codepoint = xkb_keysym_to_utf32(keysym);
    return codepoint >= 0x20U && codepoint != 0x7FU;
  }

  // Raw chord parser; compact shorthands are handled by parseKeybindChord.
  std::optional<KeybindChord> parseChordSpec(std::string_view rawSpec) {
    const std::string spec(trim(rawSpec));
    if (spec.empty()) {
      return std::nullopt;
    }
    std::vector<std::string> tokens;
    std::size_t start = 0;
    while (start <= spec.size()) {
      const std::size_t plus = spec.find('+', start);
      const std::size_t length = plus == std::string::npos ? spec.size() - start : plus - start;
      const std::string token(trim(std::string_view(spec).substr(start, length)));
      if (token.empty()) {
        return std::nullopt;
      }
      tokens.push_back(token);
      if (plus == std::string::npos) {
        break;
      }
      start = plus + 1;
    }

    std::uint32_t modifiers = 0;
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
      const std::string mod = asciiLower(tokens[index]);
      if (mod == "ctrl" || mod == "control" || mod == "ctl") {
        modifiers |= KeybindMod::Ctrl;
      } else if (mod == "shift") {
        modifiers |= KeybindMod::Shift;
      } else if (mod == "alt" || mod == "option") {
        modifiers |= KeybindMod::Alt;
      } else if (mod == "super" || mod == "meta" || mod == "logo" || mod == "win" || mod == "mod4") {
        // Compositors own Super shortcuts.
        return std::nullopt;
      } else {
        return std::nullopt;
      }
    }

    const std::string keyName = canonicalKeyName(tokens.back());
    const xkb_keysym_t sym = xkb_keysym_from_name(keyName.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym == XKB_KEY_NoSymbol || sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R) {
      return std::nullopt;
    }
    return KeybindChord{static_cast<std::uint32_t>(sym), modifiers};
  }

  std::string chordLabel(const KeybindChord& chord) {
    if (chord.sym == 0) {
      return {};
    }
    std::string keyName;
    const std::uint32_t codepoint = xkb_keysym_to_utf32(static_cast<xkb_keysym_t>(chord.sym));
    if (codepoint > 0x20 && codepoint != 0x7F) {
      std::array<char, 8> utf8{};
      const int length = xkb_keysym_to_utf8(static_cast<xkb_keysym_t>(chord.sym), utf8.data(), utf8.size());
      if (length > 1) {
        keyName.assign(utf8.data(), static_cast<std::size_t>(length - 1));
        if (keyName.size() == 1 && keyName[0] >= 'a' && keyName[0] <= 'z') {
          keyName[0] = static_cast<char>(keyName[0] - 'a' + 'A');
        }
      }
    }
    if (keyName.empty()) {
      keyName = keysymName(chord.sym);
    }
    if (const std::string_view label = luciole::keybindKeyNameLabel(keyName); !label.empty()) {
      keyName = label;
    }
    if (keyName.empty()) {
      return {};
    }
    std::string out;
    const auto append = [&](std::string_view part) {
      if (!out.empty()) {
        out += '+';
      }
      out += part;
    };
    if ((chord.modifiers & KeybindMod::Ctrl) != 0) {
      append("Ctrl");
    }
    if ((chord.modifiers & KeybindMod::Alt) != 0) {
      append("Alt");
    }
    if ((chord.modifiers & KeybindMod::Shift) != 0) {
      append("Shift");
    }
    append(keyName);
    return out;
  }

  struct KeybindSpec {
    KeybindCommand command;
    KeybindScope scope;
    std::string_view key;
    std::string_view label;
    std::array<std::string_view, luciole::kMaxKeybindChords> defaults;
  };

  // Keybind commands in enum order; defaults use display spelling.
  constexpr auto kKeybindSpecs = std::to_array<KeybindSpec>({
      {KeybindCommand::Validate, KeybindScope::Controls, "validate", "Validate", {"Return", "space"}},
      {KeybindCommand::Cancel, KeybindScope::Controls, "cancel", "Cancel", {"Escape"}},
      {KeybindCommand::FocusNext, KeybindScope::Controls, "focus_next", "Focus next", {"Tab"}},
      {KeybindCommand::FocusPrevious, KeybindScope::Controls, "focus_previous", "Focus previous", {"Shift+Tab"}},
      {KeybindCommand::MoveLeft, KeybindScope::Controls, "move_left", "Move left", {"Left"}},
      {KeybindCommand::MoveRight, KeybindScope::Controls, "move_right", "Move right", {"Right"}},
      {KeybindCommand::MoveUp, KeybindScope::Controls, "move_up", "Move up", {"Up"}},
      {KeybindCommand::MoveDown, KeybindScope::Controls, "move_down", "Move down", {"Down"}},
      {KeybindCommand::MoveFirst, KeybindScope::Controls, "move_first", "Move to first", {"Home"}},
      {KeybindCommand::MoveLast, KeybindScope::Controls, "move_last", "Move to last", {"End"}},
      {KeybindCommand::MovePageUp, KeybindScope::Controls, "move_page_up", "Move up one page", {"Page_Up"}},
      {KeybindCommand::MovePageDown, KeybindScope::Controls, "move_page_down", "Move down one page", {"Page_Down"}},

      {KeybindCommand::Back, KeybindScope::FileBrowser, "back", "Back", {"Alt+Left"}},
      {KeybindCommand::Forward, KeybindScope::FileBrowser, "forward", "Forward", {"Alt+Right"}},
      {KeybindCommand::Up, KeybindScope::FileBrowser, "up", "Up", {"Alt+Up"}},
      {KeybindCommand::HomeFolder, KeybindScope::FileBrowser, "home_folder", "Home folder", {"Alt+Home"}},
      {KeybindCommand::Refresh, KeybindScope::FileBrowser, "refresh", "Refresh", {"F5"}},
      {KeybindCommand::EditLocation, KeybindScope::FileBrowser, "edit_location", "Edit location", {"Ctrl+L"}},
      {KeybindCommand::Search, KeybindScope::FileBrowser, "search", "Search", {"Ctrl+F", "Ctrl+K"}},
      {KeybindCommand::ViewGrid, KeybindScope::FileBrowser, "view_grid", "Grid view", {"Ctrl+1"}},
      {KeybindCommand::ViewList, KeybindScope::FileBrowser, "view_list", "List view", {"Ctrl+2"}},
      {KeybindCommand::ViewMiller, KeybindScope::FileBrowser, "view_miller", "Columns view", {"Ctrl+3"}},
      {KeybindCommand::ToggleHidden, KeybindScope::FileBrowser, "toggle_hidden", "Show hidden", {"Ctrl+H"}},
      {KeybindCommand::ToggleSidebar, KeybindScope::FileBrowser, "toggle_sidebar", "Toggle sidebar", {"F9"}},
      {KeybindCommand::TogglePreview, KeybindScope::FileBrowser, "toggle_preview", "Toggle preview", {"F8"}},
      {KeybindCommand::QuickPreview, KeybindScope::FileBrowser, "quick_preview", "Quick preview", {"space"}},
      {KeybindCommand::Open, KeybindScope::FileBrowser, "open", "Open", {"Return"}},
      {KeybindCommand::OpenInNewTab, KeybindScope::FileBrowser, "open_in_new_tab", "Open in new tab", {"Ctrl+Return"}},
      {KeybindCommand::OpenInNewTabBackground,
       KeybindScope::FileBrowser,
       "open_in_new_tab_background",
       "Open in new tab in background",
       {"Ctrl+Shift+Return"}},
      {KeybindCommand::ToggleSelection,
       KeybindScope::FileBrowser,
       "toggle_selection",
       "Toggle selection",
       {"Ctrl+space"}},
      {KeybindCommand::Rename, KeybindScope::FileBrowser, "rename", "Rename", {"F2"}},
      {KeybindCommand::CreateFolder, KeybindScope::FileBrowser, "create_folder", "Create folder", {"Ctrl+Shift+N"}},
      {KeybindCommand::CreateFile, KeybindScope::FileBrowser, "create_file", "Create file", {"Ctrl+Alt+N"}},
      {KeybindCommand::Cut, KeybindScope::FileBrowser, "cut", "Cut", {"Ctrl+X"}},
      {KeybindCommand::Copy, KeybindScope::FileBrowser, "copy", "Copy", {"Ctrl+C"}},
      {KeybindCommand::Paste, KeybindScope::FileBrowser, "paste", "Paste", {"Ctrl+V"}},
      {KeybindCommand::Trash, KeybindScope::FileBrowser, "trash", "Move to trash", {"Delete"}},
      {KeybindCommand::DeletePermanently,
       KeybindScope::FileBrowser,
       "delete_permanently",
       "Delete permanently",
       {"Shift+Delete"}},
      {KeybindCommand::Properties, KeybindScope::FileBrowser, "properties", "Properties", {"Alt+Return"}},
      {KeybindCommand::Terminal, KeybindScope::FileBrowser, "terminal", "Open terminal", {"F4"}},
      {KeybindCommand::ParentFolder, KeybindScope::FileBrowser, "parent_folder", "Parent folder", {"BackSpace"}},
      {KeybindCommand::ContextMenu, KeybindScope::FileBrowser, "context_menu", "Context menu", {"Menu", "Shift+F10"}},
      {KeybindCommand::SelectAll, KeybindScope::FileBrowser, "select_all", "Select all", {"Ctrl+A"}},
      {KeybindCommand::InvertSelection,
       KeybindScope::FileBrowser,
       "invert_selection",
       "Invert selection",
       {"Ctrl+Shift+I"}},
      {KeybindCommand::SelectPattern, KeybindScope::FileBrowser, "select_pattern", "Select pattern", {"Ctrl+S"}},
      {KeybindCommand::ClearSelection, KeybindScope::FileBrowser, "clear_selection", "Clear selection", {"Escape"}},
      {KeybindCommand::Undo, KeybindScope::FileBrowser, "undo", "Undo", {"Ctrl+Z"}},
      {KeybindCommand::Redo, KeybindScope::FileBrowser, "redo", "Redo", {"Ctrl+Shift+Z"}},
      {KeybindCommand::Settings, KeybindScope::FileBrowser, "settings", "Settings", {"Ctrl+,"}},
      {KeybindCommand::KeyboardShortcuts,
       KeybindScope::FileBrowser,
       "keyboard_shortcuts",
       "Keyboard shortcuts",
       {"F1"}},
      {KeybindCommand::ZoomIn,
       KeybindScope::FileBrowser,
       "zoom_in",
       "Zoom in",
       {"Ctrl+equal", "Ctrl+plus", "Ctrl+Shift+plus", "Ctrl+KP_Add"}},
      {KeybindCommand::ZoomOut, KeybindScope::FileBrowser, "zoom_out", "Zoom out", {"Ctrl+minus", "Ctrl+KP_Subtract"}},
      {KeybindCommand::ZoomReset, KeybindScope::FileBrowser, "zoom_reset", "Reset zoom", {"Ctrl+0", "Ctrl+KP_0"}},

      {KeybindCommand::NewTab, KeybindScope::TabsAndWindows, "new_tab", "New tab", {"Ctrl+T"}},
      {KeybindCommand::CloseTab, KeybindScope::TabsAndWindows, "close_tab", "Close tab", {"Ctrl+W"}},
      {KeybindCommand::DuplicateTab, KeybindScope::TabsAndWindows, "duplicate_tab", "Duplicate tab", {"Ctrl+Shift+D"}},
      {KeybindCommand::ReopenTab, KeybindScope::TabsAndWindows, "reopen_tab", "Reopen closed tab", {"Ctrl+Shift+T"}},
      {KeybindCommand::NextTab,
       KeybindScope::TabsAndWindows,
       "next_tab",
       "Next tab",
       {"Ctrl+Tab", "Ctrl+Page_Down", "Ctrl+Shift+Page_Down"}},
      {KeybindCommand::PreviousTab,
       KeybindScope::TabsAndWindows,
       "previous_tab",
       "Previous tab",
       {"Ctrl+Shift+Tab", "Ctrl+Page_Up", "Ctrl+Shift+Page_Up"}},
      {KeybindCommand::SplitView, KeybindScope::TabsAndWindows, "split_view", "Split view", {"F3"}},
      {KeybindCommand::SwitchArea, KeybindScope::TabsAndWindows, "switch_area", "Switch area", {"F6"}},
      {KeybindCommand::ClosePane, KeybindScope::TabsAndWindows, "close_pane", "Close pane", {"Ctrl+Alt+W"}},
      {KeybindCommand::NewWindow, KeybindScope::TabsAndWindows, "new_window", "New window", {"Ctrl+N"}},
      {KeybindCommand::CloseWindow, KeybindScope::TabsAndWindows, "close_window", "Close window", {"Ctrl+Shift+W"}},
      {KeybindCommand::Fullscreen, KeybindScope::TabsAndWindows, "fullscreen", "Fullscreen", {"F11"}},
      {KeybindCommand::Quit, KeybindScope::TabsAndWindows, "quit", "Quit", {"Ctrl+Q"}},
      {KeybindCommand::SelectTab1, KeybindScope::TabsAndWindows, "select_tab_1", "Select tab 1", {"Alt+1"}},
      {KeybindCommand::SelectTab2, KeybindScope::TabsAndWindows, "select_tab_2", "Select tab 2", {"Alt+2"}},
      {KeybindCommand::SelectTab3, KeybindScope::TabsAndWindows, "select_tab_3", "Select tab 3", {"Alt+3"}},
      {KeybindCommand::SelectTab4, KeybindScope::TabsAndWindows, "select_tab_4", "Select tab 4", {"Alt+4"}},
      {KeybindCommand::SelectTab5, KeybindScope::TabsAndWindows, "select_tab_5", "Select tab 5", {"Alt+5"}},
      {KeybindCommand::SelectTab6, KeybindScope::TabsAndWindows, "select_tab_6", "Select tab 6", {"Alt+6"}},
      {KeybindCommand::SelectTab7, KeybindScope::TabsAndWindows, "select_tab_7", "Select tab 7", {"Alt+7"}},
      {KeybindCommand::SelectTab8, KeybindScope::TabsAndWindows, "select_tab_8", "Select tab 8", {"Alt+8"}},
      {KeybindCommand::SelectTab9, KeybindScope::TabsAndWindows, "select_tab_9", "Select tab 9 (last)", {"Alt+9"}},
  });
  static_assert(kKeybindSpecs.size() == luciole::kKeybindCommandCount);
  static_assert(
      []<std::size_t... I>(std::index_sequence<I...>) {
        return ((kKeybindSpecs[I].command == static_cast<KeybindCommand>(I)) && ...);
      }(std::make_index_sequence<kKeybindSpecs.size()>{}),
      "kKeybindSpecs must follow KeybindCommand order"
  );

  constexpr std::size_t kControlKeybindCount = 12;
  constexpr std::size_t kFileBrowserKeybindCount = 41;
  constexpr std::size_t kTabsAndWindowsKeybindCount = 22;
  static_assert(kControlKeybindCount + kFileBrowserKeybindCount + kTabsAndWindowsKeybindCount == kKeybindSpecs.size());

  // How a preference is spelled in TOML and which values it accepts. Enums
  // are strings from `names`.
  template <class T> struct Rule {
    std::span<const Named<T>> names;
  };
  template <> struct Rule<bool> {};
  template <> struct Rule<std::string> {};
  template <> struct Rule<int> {
    bool (*accepts)(int);
    std::string_view expected;
  };
  template <> struct Rule<double> {
    double minimum;
    double maximum;
    std::string_view expected;
  };
  template <> struct Rule<luciole::DetailsColumnOrder> {};
  template <> struct Rule<luciole::KeybindConfig> {};

  bool accepts(const Rule<std::string>& /*rule*/, const std::string& value) {
    return value.size() <= 4096 && !value.contains('\0') && value.find_first_of("\r\n") == std::string::npos;
  }
  bool accepts(const Rule<bool>& /*rule*/, bool /*value*/) { return true; }
  bool accepts(const Rule<int>& rule, int value) { return rule.accepts(value); }
  bool accepts(const Rule<double>& rule, double value) { return value >= rule.minimum && value <= rule.maximum; }
  bool accepts(const Rule<luciole::DetailsColumnOrder>& /*rule*/, const luciole::DetailsColumnOrder& order) {
    return luciole::validDetailsColumnOrder(order);
  }
  bool accepts(const Rule<luciole::KeybindConfig>& /*rule*/, const luciole::KeybindConfig& config) {
    return luciole::keybindConfigValid(config);
  }
  template <class T>
    requires std::is_enum_v<T>
  bool accepts(const Rule<T>& rule, T value) {
    return std::ranges::contains(rule.names, value, &Named<T>::value);
  }

  std::optional<std::string> decode(const Rule<std::string>& /*rule*/, const toml::node& node) {
    return node.value_exact<std::string>();
  }
  std::string expected(const Rule<std::string>& /*rule*/) {
    return "expected a single-line command of at most 4096 bytes";
  }
  void encode(toml::table& table, std::string_view key, const Rule<std::string>& /*rule*/, const std::string& value) {
    table.insert(key, value);
  }

  // Shape only; callers check accepts() as well.
  std::optional<bool> decode(const Rule<bool>& /*rule*/, const toml::node& node) { return node.value_exact<bool>(); }
  std::optional<int> decode(const Rule<int>& /*rule*/, const toml::node& node) {
    const auto number = node.value_exact<std::int64_t>();
    return number && std::in_range<int>(*number) ? std::optional<int>(static_cast<int>(*number)) : std::nullopt;
  }
  // Accepts integers too, so `1` reads as 1.0.
  std::optional<double> decode(const Rule<double>& /*rule*/, const toml::node& node) {
    return node.is_number() ? node.value<double>() : std::nullopt;
  }
  std::optional<luciole::DetailsColumnOrder>
  decode(const Rule<luciole::DetailsColumnOrder>& /*rule*/, const toml::node& node) {
    const auto* columns = node.as_array();
    luciole::DetailsColumnOrder order{};
    if (columns == nullptr || columns->size() < 3 || columns->size() > order.size()) {
      return std::nullopt;
    }
    std::array<bool, luciole::kDetailsColumnCount> seen{};
    std::size_t index = 0;
    for (const auto& item : *columns) {
      const auto* name = item.as_string();
      const auto column = name ? luciole::detailsColumnFromName(name->get()) : std::nullopt;
      if (!column || seen[static_cast<std::size_t>(*column)]) {
        return std::nullopt;
      }
      seen[static_cast<std::size_t>(*column)] = true;
      order[index++] = *column;
    }
    if (!seen[0] || !seen[1] || !seen[2]) {
      return std::nullopt;
    }
    for (const auto column : luciole::kDefaultDetailsColumnOrder) {
      if (!seen[static_cast<std::size_t>(column)]) {
        order[index++] = column;
      }
    }
    return order;
  }
  std::optional<luciole::KeybindConfig>
  decode(const Rule<luciole::KeybindConfig>& /*rule*/, const toml::node& node, const luciole::KeybindConfig& base) {
    const auto* table = node.as_table();
    if (table == nullptr) {
      return std::nullopt;
    }
    luciole::KeybindConfig config = base;
    for (const auto& [key, value] : *table) {
      const auto command = luciole::keybindCommandFromKey(key.str());
      const auto* chords = value.as_array();
      if (!command || chords == nullptr || chords->size() > luciole::kMaxKeybindChords) {
        return std::nullopt;
      }
      luciole::KeybindChords parsed{};
      std::size_t index = 0;
      for (const auto& item : *chords) {
        const auto* spec = item.as_string();
        const auto chord = spec != nullptr ? luciole::parseKeybindChord(spec->get()) : std::nullopt;
        if (!chord) {
          return std::nullopt;
        }
        parsed[index++] = *chord;
      }
      config.chords(*command) = parsed;
    }
    return luciole::keybindConfigValid(config) ? std::optional(config) : std::nullopt;
  }
  template <class T>
    requires std::is_enum_v<T>
  std::optional<T> decode(const Rule<T>& rule, const toml::node& node) {
    const auto* text = node.as_string();
    if (text == nullptr) {
      return std::nullopt;
    }
    const auto found = std::ranges::find(rule.names, std::string_view(text->get()), &Named<T>::name);
    return found != rule.names.end() ? std::optional<T>(found->value) : std::nullopt;
  }

  std::string expected(const Rule<bool>& /*rule*/) { return "expected a boolean"; }
  std::string expected(const Rule<int>& rule) { return std::string(rule.expected); }
  std::string expected(const Rule<double>& rule) { return std::string(rule.expected); }
  std::string expected(const Rule<luciole::DetailsColumnOrder>& /*rule*/) {
    return "expected unique column names including 'type', 'size', and 'modified'; optional columns may be omitted";
  }
  std::string expected(const Rule<luciole::KeybindConfig>& /*rule*/) {
    return "expected a table of command names with arrays of at most four shortcut strings";
  }
  template <class T>
    requires std::is_enum_v<T>
  std::string expected(const Rule<T>& rule) {
    std::string text = "expected ";
    for (std::size_t index = 0; index < rule.names.size(); ++index) {
      if (index > 0) {
        text += rule.names.size() == 2 ? " or " : index + 1 == rule.names.size() ? ", or " : ", ";
      }
      text += std::format("'{}'", rule.names[index].name);
    }
    return text;
  }

  void encode(toml::table& table, std::string_view key, const Rule<bool>& /*rule*/, bool value) {
    table.insert(key, value);
  }
  void encode(toml::table& table, std::string_view key, const Rule<int>& /*rule*/, int value) {
    table.insert(key, value);
  }
  void encode(toml::table& table, std::string_view key, const Rule<double>& /*rule*/, double value) {
    table.insert(key, value);
  }
  void encode(
      toml::table& table, std::string_view key, const Rule<luciole::DetailsColumnOrder>& /*rule*/,
      const luciole::DetailsColumnOrder& order
  ) {
    toml::array columns;
    for (const auto column : order) {
      columns.push_back(luciole::detailsColumnName(column));
    }
    table.insert(key, std::move(columns));
  }
  void encode(
      toml::table& table, std::string_view key, const luciole::KeybindConfig& value,
      const luciole::KeybindConfig& inherited
  ) {
    toml::table commands;
    for (const auto& spec : kKeybindSpecs) {
      if (value.chords(spec.command) == inherited.chords(spec.command)) {
        continue;
      }
      toml::array chords;
      for (const auto& chord : value.chords(spec.command)) {
        if (chord) {
          chords.push_back(luciole::keybindChordSpec(*chord));
        }
      }
      commands.insert(spec.key, std::move(chords));
    }
    if (!commands.empty()) {
      table.insert(key, std::move(commands));
    }
  }
  template <class T>
    requires std::is_enum_v<T>
  void encode(toml::table& table, std::string_view key, const Rule<T>& rule, T value) {
    table.insert(key, std::ranges::find(rule.names, value, &Named<T>::value)->name);
  }

  template <class T> struct Descriptor {
    using Value = T;
    luciole::PreferenceKey<T> preference;
    std::string_view key; // Field name inside [browser].
    T& (*field)(BrowserSettings&);
    Rule<T> rule;
  };
  template <class T>
  constexpr Descriptor<T> describe(
      luciole::PreferenceKey<T> preference, std::string_view key, std::type_identity_t<T>& (*field)(BrowserSettings&),
      Rule<T> rule = {}
  ) {
    return {.preference = preference, .key = key, .field = field, .rule = rule};
  }
  template <int Minimum, int Maximum> bool between(int value) { return value >= Minimum && value <= Maximum; }
  template <std::size_t Index> bool columnWidth(int value) { return luciole::validDetailsColumnWidth(Index, value); }
  // List and Columns icon sizes; 0 keeps the view's default size.
  bool rowIconSize(int value) { return value == 0 || std::ranges::contains(luciole::kRowIconSizes, value); }
  constexpr Rule<int> kRowIconSize{rowIconSize, "expected 0 or integer icon size 16, 20, 24, 28, 32, 40, 48, or 64"};
  constexpr Rule<int> kGridIconSize{
      [](int value) { return std::ranges::contains(luciole::kGridIconSizes, value); },
      "expected integer icon size 32, 48, 64, 80, 96, 128, 160, 192, or 256"
  };

  using P = luciole::BrowserPreference;
  using S = BrowserSettings;
  // Every browser.* setting, in BrowserPreferenceId order. Defaults are BrowserSettings' initializers.
  constexpr std::tuple kPreferences{
      describe(
          P::ActivationMode, "activation_mode", [](S& s) -> auto& { return s.activationMode; }, {kActivationModes}
      ),
      describe(P::TypingMode, "typing_mode", [](S& s) -> auto& { return s.typingMode; }, {kTypingModes}),
      describe(P::DropAction, "drop_action", [](S& s) -> auto& { return s.dropAction; }, {kDropActions}),
      describe(
          P::SingleClickLocationEdit, "single_click_location_edit",
          [](S& s) -> auto& { return s.singleClickLocationEdit; }
      ),
      describe(
          P::ConfirmPermanentDeletion, "confirm_permanent_deletion",
          [](S& s) -> auto& { return s.confirmPermanentDeletion; }
      ),
      describe(
          P::ShowPermanentDeletion, "show_permanent_deletion", [](S& s) -> auto& { return s.showPermanentDeletion; }
      ),
      describe(P::SwitchToNewTab, "switch_to_new_tab", [](S& s) -> auto& { return s.switchToNewTab; }),
      describe(P::RestoreLastWindow, "restore_last_window", [](S& s) -> auto& { return s.restoreLastWindow; }),
      describe(P::SingleWindow, "single_window", [](S& s) -> auto& { return s.singleWindow; }),
      describe(P::ShowHidden, "show_hidden", [](S& s) -> auto& { return s.showHidden; }),
      describe(P::ShowBackups, "show_backups", [](S& s) -> auto& { return s.showBackups; }),
      describe(P::AlwaysShowTabBar, "always_show_tab_bar", [](S& s) -> auto& { return s.alwaysShowTabBar; }),
      describe(P::AlwaysShowStatusBar, "always_show_status_bar", [](S& s) -> auto& { return s.alwaysShowStatusBar; }),
      describe(P::CompactMenus, "compact_menus", [](S& s) -> auto& { return s.compactMenus; }),
      describe(P::ShowCloseButtons, "show_close_buttons", [](S& s) -> auto& { return s.showCloseButtons; }),
      describe(P::ReduceAnimations, "reduce_animations", [](S& s) -> auto& { return s.reduceAnimations; }),
      describe(P::UseSSD, "use_ssd", [](S& s) -> auto& { return s.useSSD; }),
      describe(
          P::BackgroundOpacity, "background_opacity", [](S& s) -> auto& { return s.backgroundOpacity; },
          {0.0, 1.0, "expected a number from 0 to 1"}
      ),
      describe(
          P::CornerRadiusScale, "corner_radius_scale", [](S& s) -> auto& { return s.cornerRadiusScale; },
          {0.0, 2.0, "expected a number from 0 to 2"}
      ),
      describe(P::SortBy, "sort_by", [](S& s) -> auto& { return s.sort.field; }, {kSortFields}),
      describe(P::SortDescending, "sort_descending", [](S& s) -> auto& { return s.sort.descending; }),
      describe(P::DirectoriesFirst, "directories_first", [](S& s) -> auto& { return s.sort.directoriesFirst; }),
      describe(P::HiddenLast, "hidden_last", [](S& s) -> auto& { return s.sort.hiddenLast; }),
      describe(P::ViewMode, "view_mode", [](S& s) -> auto& { return s.viewMode; }, {kViewModes}),
      describe(P::RememberFolderViews, "remember_folder_views", [](S& s) -> auto& { return s.rememberFolderViews; }),
      describe(
          P::GridIconSize, "grid_icon_size", [](S& s) -> auto& { return s.gridIconSize; }, kGridIconSize
      ),
      describe(
          P::ListIconSize, "list_icon_size", [](S& s) -> auto& { return s.listIconSize; }, kRowIconSize
      ),
      describe(
          P::MillerIconSize, "miller_icon_size", [](S& s) -> auto& { return s.millerIconSize; }, kRowIconSize
      ),
      describe(P::Density, "density", [](S& s) -> auto& { return s.presentation.density; }, {kDensities}),
      describe(
          P::FilenameTruncation, "filename_truncation", [](S& s) -> auto& { return s.presentation.filenameTruncation; },
          {kFilenameTruncations}
      ),
      describe(P::ShowSizeCaption, "show_size_caption", [](S& s) -> auto& { return s.presentation.captions.size; }),
      describe(P::ShowTypeCaption, "show_type_caption", [](S& s) -> auto& { return s.presentation.captions.type; }),
      describe(
          P::ShowModifiedCaption, "show_modified_caption",
          [](S& s) -> auto& { return s.presentation.captions.modified; }
      ),
      describe(P::ShowEmblems, "show_emblems", [](S& s) -> auto& { return s.presentation.showEmblems; }),
      describe(
          P::SidebarWidth, "sidebar_width", [](S& s) -> auto& { return s.sidebarWidth; },
          {between<luciole::kMinSidebarWidth, luciole::kMaxSidebarWidth>,
           "expected integer sidebar width from 160 to 400"}
      ),
      describe(P::ShowSidebar, "show_sidebar", [](S& s) -> auto& { return s.showSidebar; }),
      describe(P::SidebarTree, "sidebar_tree", [](S& s) -> auto& { return s.sidebarTree; }),
      describe(P::ShowDrives, "show_drives", [](S& s) -> auto& { return s.showDrives; }),
      describe(P::ShowNetwork, "show_network", [](S& s) -> auto& { return s.showNetwork; }),
      describe(P::ExpandableFolders, "expandable_folders", [](S& s) -> auto& { return s.expandableFolders; }),
      describe(P::ShowPreview, "show_preview", [](S& s) -> auto& { return s.showPreview; }),
      describe(P::ChooserShowPreview, "chooser_show_preview", [](S& s) -> auto& { return s.chooserShowPreview; }),
      describe(
          P::ChooserStartFolder, "chooser_start_folder", [](S& s) -> auto& { return s.chooserStartFolder; },
          {kChooserStartFolders}
      ),
      describe(P::ChooserSortBy, "chooser_sort_by", [](S& s) -> auto& { return s.chooserSort.field; }, {kSortFields}),
      describe(
          P::ChooserSortDescending, "chooser_sort_descending", [](S& s) -> auto& { return s.chooserSort.descending; }
      ),
      describe(
          P::ChooserDirectoriesFirst, "chooser_directories_first",
          [](S& s) -> auto& { return s.chooserSort.directoriesFirst; }
      ),
      describe(P::ChooserHiddenLast, "chooser_hidden_last", [](S& s) -> auto& { return s.chooserSort.hiddenLast; }),
      describe(
          P::ChooserViewMode, "chooser_view_mode", [](S& s) -> auto& { return s.chooserViewMode; }, {kChooserViewModes}
      ),
      describe(P::ChooserShowHidden, "chooser_show_hidden", [](S& s) -> auto& { return s.chooserShowHidden; }),
      describe(
          P::ChooserGridIconSize, "chooser_grid_icon_size", [](S& s) -> auto& { return s.chooserGridIconSize; },
          kGridIconSize
      ),
      describe(
          P::ChooserListIconSize, "chooser_list_icon_size", [](S& s) -> auto& { return s.chooserListIconSize; },
          kRowIconSize
      ),
      describe(
          P::PreviewWidth, "preview_width", [](S& s) -> auto& { return s.previewWidth; },
          {between<208, 800>, "expected integer preview width from 208 to 800"}
      ),
      describe(
          P::MillerColumnWidth, "miller_column_width", [](S& s) -> auto& { return s.millerColumnWidth; },
          {between<180, 800>, "expected integer column width from 180 to 800"}
      ),
      describe(P::MillerShowParents, "miller_show_parents", [](S& s) -> auto& { return s.millerShowParents; }),
      describe(P::ShowTypeColumn, "show_type_column", [](S& s) -> auto& { return s.columns.type; }),
      describe(P::ShowSizeColumn, "show_size_column", [](S& s) -> auto& { return s.columns.size; }),
      describe(P::ShowModifiedColumn, "show_modified_column", [](S& s) -> auto& { return s.columns.modified; }),
      describe(P::ShowAccessedColumn, "show_accessed_column", [](S& s) -> auto& { return s.columns.accessed; }),
      describe(P::ShowCreatedColumn, "show_created_column", [](S& s) -> auto& { return s.columns.created; }),
      describe(P::ShowOwnerColumn, "show_owner_column", [](S& s) -> auto& { return s.columns.owner; }),
      describe(P::ShowGroupColumn, "show_group_column", [](S& s) -> auto& { return s.columns.group; }),
      describe(
          P::ShowPermissionsColumn, "show_permissions_column", [](S& s) -> auto& { return s.columns.permissions; }
      ),
      describe(P::ShowLocationColumn, "show_location_column", [](S& s) -> auto& { return s.columns.location; }),
      describe(P::ColumnOrder, "column_order", [](S& s) -> auto& { return s.columnOrder; }),
      describe(
          P::NameColumnWidth, luciole::kDetailsColumnWidthKeys[0], [](S& s) -> auto& { return s.columnWidths[0]; },
          {columnWidth<0>, "expected 0 (automatic) or an integer width from 160 to 16384"}
      ),
      describe(
          P::TypeColumnWidth, luciole::kDetailsColumnWidthKeys[1], [](S& s) -> auto& { return s.columnWidths[1]; },
          {columnWidth<1>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::SizeColumnWidth, luciole::kDetailsColumnWidthKeys[2], [](S& s) -> auto& { return s.columnWidths[2]; },
          {columnWidth<2>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::ModifiedColumnWidth, luciole::kDetailsColumnWidthKeys[3], [](S& s) -> auto& { return s.columnWidths[3]; },
          {columnWidth<3>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::AccessedColumnWidth, luciole::kDetailsColumnWidthKeys[4], [](S& s) -> auto& { return s.columnWidths[4]; },
          {columnWidth<4>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::CreatedColumnWidth, luciole::kDetailsColumnWidthKeys[5], [](S& s) -> auto& { return s.columnWidths[5]; },
          {columnWidth<5>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::OwnerColumnWidth, luciole::kDetailsColumnWidthKeys[6], [](S& s) -> auto& { return s.columnWidths[6]; },
          {columnWidth<6>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::GroupColumnWidth, luciole::kDetailsColumnWidthKeys[7], [](S& s) -> auto& { return s.columnWidths[7]; },
          {columnWidth<7>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::PermissionsColumnWidth, luciole::kDetailsColumnWidthKeys[8],
          [](S& s) -> auto& { return s.columnWidths[8]; }, {columnWidth<8>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::LocationColumnWidth, luciole::kDetailsColumnWidthKeys[9], [](S& s) -> auto& { return s.columnWidths[9]; },
          {columnWidth<9>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::ThumbnailPolicy, "thumbnail_policy", [](S& s) -> auto& { return s.thumbnails.policy; },
          {kThumbnailPolicies}
      ),
      describe(
          P::ThumbnailMaxFileMib, "thumbnail_max_file_mib", [](S& s) -> auto& { return s.thumbnails.maxFileMiB; },
          {between<1, 1024>, "expected an integer from 1 to 1024"}
      ),
      describe(
          P::ThumbnailCacheMib, "thumbnail_cache_mib", [](S& s) -> auto& { return s.thumbnails.cacheMiB; },
          {between<16, 8192>, "expected an integer from 16 to 8192"}
      ),
      describe(P::TerminalCommand, "terminal_command", [](S& s) -> auto& { return s.terminalCommand; }),
      describe(P::Keybinds, "keybinds", [](S& s) -> auto& { return s.keybinds; }),
  };
  constexpr std::size_t kPreferenceCount = std::tuple_size_v<decltype(kPreferences)>;
  static_assert(kPreferenceCount == std::to_underlying(luciole::BrowserPreferenceId::Count));
  static_assert(
      []<std::size_t... I>(std::index_sequence<I...>) {
        return ((std::get<I>(kPreferences).preference.id() == static_cast<luciole::BrowserPreferenceId>(I)) && ...);
      }(std::make_index_sequence<kPreferenceCount>{}),
      "kPreferences must follow BrowserPreferenceId order"
  );

  // Calls `visit` with each descriptor index as a std::integral_constant.
  template <class Visit> void forEachPreference(Visit&& visit) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (visit(std::integral_constant<std::size_t, I>{}), ...);
    }(std::make_index_sequence<kPreferenceCount>{});
  }
  template <class Visit> void visitPreference(luciole::BrowserPreferenceId id, Visit&& visit) {
    forEachPreference([&](auto index) {
      if (index == std::to_underlying(id)) {
        visit(index);
      }
    });
  }

  template <class T> std::string preferenceKey(const Descriptor<T>& descriptor) {
    return std::format("browser.{}", descriptor.key);
  }

  // One optional override per descriptor; nullopt inherits.
  template <class... T> std::tuple<std::optional<T>...> patchFor(const std::tuple<Descriptor<T>...>& descriptors);
  using Patch = decltype(patchFor(kPreferences));

  struct FileDescriptor {
    int fd;
    ~FileDescriptor() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    explicit FileDescriptor(int value) : fd(value) {}
  };

  luciole::ConfigIssue ioIssue(const std::filesystem::path& file, std::string message) {
    return {.file = file, .key = {}, .message = std::move(message)};
  }

  std::string ioMessage(int error) { return std::error_code(error, std::generic_category()).message(); }

  std::expected<std::optional<std::string>, luciole::ConfigIssue>
  readFile(const std::filesystem::path& file, bool optional) {
    if (optional) {
      struct stat linkInfo{};
      if (::lstat(file.c_str(), &linkInfo) < 0) {
        const int error = errno;
        if (error == ENOENT) {
          return std::nullopt;
        }
        return std::unexpected(ioIssue(file, ioMessage(error)));
      }
    }
    // Declarative config symlinks are allowed. Nonblocking open plus fstat
    // rejects FIFOs/devices without waiting for a writer or reading them.
    const FileDescriptor descriptor(::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (descriptor.fd < 0) {
      return std::unexpected(ioIssue(file, ioMessage(errno)));
    }
    struct stat info{};
    if (::fstat(descriptor.fd, &info) < 0) {
      return std::unexpected(ioIssue(file, ioMessage(errno)));
    }
    if (!S_ISREG(info.st_mode)) {
      return std::unexpected(ioIssue(file, "expected a regular TOML file"));
    }
    std::string text;
    std::array<char, 8192> buffer{};
    while (true) {
      const auto count = ::read(descriptor.fd, buffer.data(), buffer.size());
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        return std::unexpected(ioIssue(file, ioMessage(errno)));
      }
      if (count == 0) {
        return text;
      }
      const auto size = static_cast<std::size_t>(count);
      if (text.size() + size > kMaxConfigBytes) {
        return std::unexpected(ioIssue(file, "configuration exceeds the 256 KiB limit"));
      }
      text.append(buffer.data(), size);
    }
  }

  enum class UnknownSettings { Reject, Warn };

  std::expected<Patch, luciole::ConfigIssues> parse(
      const std::filesystem::path& file, std::string_view text, UnknownSettings unknown = UnknownSettings::Reject,
      toml::table* document = nullptr, const luciole::KeybindConfig* keybindBase = nullptr
  ) {
    auto table = luciole::parseToml(text, file);
    if (!table) {
      return std::unexpected(std::move(table.error()));
    }
    luciole::TomlDecoder decoder(file);
    luciole::TomlDecoder warnings(file);
    const auto unknownSetting = [&](const toml::node& node, std::string key) {
      if (unknown == UnknownSettings::Reject) {
        decoder.issue(node, std::move(key), "unknown setting");
      } else {
        warnings.issue(node, std::move(key), "unknown setting; ignored");
      }
    };
    Patch patch;
    for (const auto& [key, node] : *table) {
      if (key == "schema_version") {
        if (!luciole::isSchemaVersion(node)) {
          decoder.issue(
              node, "schema_version", std::format("expected integer schema version {}", luciole::kSchemaVersion)
          );
        }
      } else if (key == "browser") {
        const auto* browser = node.as_table();
        if (browser == nullptr) {
          decoder.issue(node, "browser", "expected a table");
          continue;
        }
        for (const auto& [name, value] : *browser) {
          bool known = false;
          forEachPreference([&](auto index) {
            const auto& descriptor = std::get<index>(kPreferences);
            if (descriptor.key != name.str()) {
              return;
            }
            known = true;
            const auto decoded = [&] {
              if constexpr (
                  std::same_as<typename std::remove_cvref_t<decltype(descriptor)>::Value, luciole::KeybindConfig>
              ) {
                return decode(descriptor.rule, value, keybindBase != nullptr ? *keybindBase : luciole::KeybindConfig{});
              } else {
                return decode(descriptor.rule, value);
              }
            }();
            if (decoded && accepts(descriptor.rule, *decoded)) {
              std::get<index>(patch) = decoded;
            } else {
              decoder.issue(value, preferenceKey(descriptor), expected(descriptor.rule));
            }
          });
          if (!known) {
            unknownSetting(value, std::format("browser.{}", name.str()));
          }
        }
      } else {
        unknownSetting(node, std::string(key.str()));
      }
    }
    if (!decoder.issues.empty()) {
      return std::unexpected(std::move(decoder.issues));
    }
    for (const auto& warning : warnings.issues) {
      std::println(stderr, "luciole: warning: {}", warning.format());
    }
    if (document != nullptr) {
      *document = std::move(*table);
    }
    return patch;
  }

  void applyPatch(luciole::Settings& settings, const Patch& patch) {
    forEachPreference([&](auto index) {
      if (const auto& value = std::get<index>(patch)) {
        std::get<index>(kPreferences).field(settings.browser) = *value;
      }
    });
  }

  void editPatch(Patch& patch, const luciole::OverrideEdit& edit) {
    visitPreference(edit.preference(), [&](auto index) {
      std::get<index>(patch) = edit.value(std::get<index>(kPreferences).preference);
    });
  }

  std::string formatPatch(const Patch& patch, toml::table document, const luciole::BrowserSettings& inherited) {
    toml::table browser;
    if (const auto* existing = document["browser"].as_table()) {
      browser = *existing;
    }
    forEachPreference([&](auto index) {
      const auto& descriptor = std::get<index>(kPreferences);
      browser.erase(descriptor.key);
      if (const auto& value = std::get<index>(patch)) {
        if constexpr (std::same_as<typename std::remove_cvref_t<decltype(descriptor)>::Value, luciole::KeybindConfig>) {
          encode(browser, descriptor.key, *value, inherited.keybinds);
        } else {
          encode(browser, descriptor.key, descriptor.rule, *value);
        }
      }
    });
    document.insert_or_assign("schema_version", luciole::kSchemaVersion);
    if (browser.empty()) {
      document.erase("browser");
    } else {
      document.insert_or_assign("browser", std::move(browser));
    }
    std::ostringstream output;
    output << toml::toml_formatter{
        document,
        (toml::toml_formatter::default_flags & ~toml::format_flags::allow_literal_strings)
            | toml::format_flags::relaxed_float_precision
    };
    auto text = output.str();
    if (!text.ends_with('\n')) {
      text.push_back('\n');
    }
    return text;
  }
} // namespace

namespace luciole {
  std::span<const KeybindCommand> keybindCommands(KeybindScope scope) {
    static const auto commands = [] {
      std::array<KeybindCommand, kKeybindCommandCount> result{};
      for (std::size_t index = 0; index < kKeybindSpecs.size(); ++index) {
        result[index] = kKeybindSpecs[index].command;
      }
      return result;
    }();
    std::size_t offset = 0;
    std::size_t count = kControlKeybindCount;
    if (scope == KeybindScope::FileBrowser) {
      offset = kControlKeybindCount;
      count = kFileBrowserKeybindCount;
    } else if (scope == KeybindScope::TabsAndWindows) {
      offset = kControlKeybindCount + kFileBrowserKeybindCount;
      count = kTabsAndWindowsKeybindCount;
    }
    return std::span<const KeybindCommand>(commands).subspan(offset, count);
  }

  std::string_view keybindCommandKey(KeybindCommand command) {
    return kKeybindSpecs[static_cast<std::size_t>(command)].key;
  }

  std::string_view keybindCommandLabel(KeybindCommand command) {
    return kKeybindSpecs[static_cast<std::size_t>(command)].label;
  }

  KeybindScope keybindCommandScope(KeybindCommand command) {
    return kKeybindSpecs[static_cast<std::size_t>(command)].scope;
  }

  std::optional<KeybindCommand> keybindCommandFromKey(std::string_view key) {
    for (const auto& spec : kKeybindSpecs) {
      if (spec.key == key) {
        return spec.command;
      }
    }
    return std::nullopt;
  }

  KeybindChord normalizeKeybindChord(KeybindChord chord) {
    if (chord.sym == XKB_KEY_ISO_Left_Tab) {
      chord.sym = XKB_KEY_Tab;
      chord.modifiers |= KeybindMod::Shift;
    } else if (chord.sym == XKB_KEY_KP_Enter) {
      chord.sym = XKB_KEY_Return;
    } else {
      chord.sym = static_cast<std::uint32_t>(xkb_keysym_to_lower(static_cast<xkb_keysym_t>(chord.sym)));
    }
    return chord;
  }

  std::optional<KeybindChord> parseKeybindChord(std::string_view rawSpec) {
    std::string spec(trim(rawSpec));
    if (spec.empty()) {
      return std::nullopt;
    }
    if (spec.ends_with("+,")) {
      spec.replace(spec.size() - 1, 1, "comma");
    } else if (spec.ends_with("++")) {
      spec.pop_back();
      spec += "plus";
    }
    const auto chord = parseChordSpec(spec);
    return chord ? std::optional<KeybindChord>(normalizeKeybindChord(*chord)) : std::nullopt;
  }

  std::string keybindChordSpec(const KeybindChord& chord) {
    std::string spec;
    if ((chord.modifiers & KeybindMod::Ctrl) != 0) {
      spec += "Ctrl+";
    }
    if ((chord.modifiers & KeybindMod::Alt) != 0) {
      spec += "Alt+";
    }
    if ((chord.modifiers & KeybindMod::Shift) != 0) {
      spec += "Shift+";
    }
    spec += keysymName(chord.sym);
    return spec;
  }

  std::string keybindChordLabel(const KeybindChord& chord) { return chordLabel(chord); }

  bool keybindChordMatches(const KeybindChord& chord, std::uint32_t sym, std::uint32_t modifiers) noexcept {
    if (sym == XKB_KEY_KP_Enter) {
      sym = XKB_KEY_Return;
    }
    if (sym == XKB_KEY_ISO_Left_Tab) {
      sym = XKB_KEY_Tab;
      modifiers |= KeybindMod::Shift;
    } else if ((modifiers & KeybindMod::Shift) != 0) {
      sym = static_cast<std::uint32_t>(xkb_keysym_to_lower(static_cast<xkb_keysym_t>(sym)));
    }
    return chord.sym == sym && chord.modifiers == modifiers;
  }

  KeybindConfig::KeybindConfig() {
    for (std::size_t command = 0; command < kKeybindSpecs.size(); ++command) {
      for (std::size_t slot = 0; slot < kMaxKeybindChords; ++slot) {
        if (kKeybindSpecs[command].defaults[slot].empty()) {
          break;
        }
        if (const auto chord = parseKeybindChord(kKeybindSpecs[command].defaults[slot])) {
          m_chords[command][slot] = *chord;
        }
      }
    }
  }

  const KeybindChords& KeybindConfig::chords(KeybindCommand command) const {
    return m_chords[static_cast<std::size_t>(command)];
  }

  KeybindChords& KeybindConfig::chords(KeybindCommand command) { return m_chords[static_cast<std::size_t>(command)]; }

  namespace {
    KeybindConfig& activeKeybindStorage() {
      static KeybindConfig config;
      return config;
    }
  } // namespace

  const KeybindConfig& activeKeybinds() { return activeKeybindStorage(); }

  void setActiveKeybinds(const KeybindConfig& config) { activeKeybindStorage() = config; }

  bool keybindMatches(const KeybindConfig& config, KeybindCommand command, std::uint32_t sym, std::uint32_t modifiers) {
    return std::ranges::any_of(config.chords(command), [sym, modifiers](const auto& chord) {
      return chord && keybindChordMatches(*chord, sym, modifiers);
    });
  }

  bool keybindMatches(KeybindCommand command, std::uint32_t sym, std::uint32_t modifiers) {
    return keybindMatches(activeKeybinds(), command, sym, modifiers);
  }

  std::optional<KeybindChord> keybindMatchedChordAllowingExtraModifiers(
      const KeybindConfig& config, KeybindCommand command, std::uint32_t sym, std::uint32_t modifiers
  ) {
    const KeybindChord received = normalizeKeybindChord({sym, modifiers});
    for (const auto& chord : config.chords(command)) {
      if (!chord) {
        continue;
      }
      const KeybindChord expected = normalizeKeybindChord(*chord);
      if (expected.sym == received.sym && (expected.modifiers & ~received.modifiers) == 0) {
        return expected;
      }
    }
    return std::nullopt;
  }

  std::optional<KeybindChord>
  keybindMatchedChordAllowingExtraModifiers(KeybindCommand command, std::uint32_t sym, std::uint32_t modifiers) {
    return keybindMatchedChordAllowingExtraModifiers(activeKeybinds(), command, sym, modifiers);
  }

  bool keybindMatchesAllowingExtraModifiers(
      const KeybindConfig& config, KeybindCommand command, std::uint32_t sym, std::uint32_t modifiers
  ) {
    return keybindMatchedChordAllowingExtraModifiers(config, command, sym, modifiers).has_value();
  }

  bool keybindMatchesAllowingExtraModifiers(KeybindCommand command, std::uint32_t sym, std::uint32_t modifiers) {
    return keybindMatchesAllowingExtraModifiers(activeKeybinds(), command, sym, modifiers);
  }

  std::span<const KeybindCommand> keybindMovementCommands() {
    static constexpr std::array<KeybindCommand, kMovementCommandCount> commands{
        KeybindCommand::MoveLeft,  KeybindCommand::MoveRight, KeybindCommand::MoveUp,     KeybindCommand::MoveDown,
        KeybindCommand::MoveFirst, KeybindCommand::MoveLast,  KeybindCommand::MovePageUp, KeybindCommand::MovePageDown,
    };
    return commands;
  }

  std::optional<KeybindMatch>
  keybindBestMatch(std::span<const KeybindCommand> commands, std::uint32_t sym, std::uint32_t modifiers) {
    const KeybindChord received = normalizeKeybindChord({sym, modifiers});
    std::optional<KeybindMatch> best;
    for (const auto command : commands) {
      for (const auto& stored : activeKeybinds().chords(command)) {
        if (!stored) {
          continue;
        }
        const KeybindChord chord = normalizeKeybindChord(*stored);
        if (chord.sym != received.sym || (chord.modifiers & ~received.modifiers) != 0) {
          continue;
        }
        if (best && std::popcount(chord.modifiers) <= std::popcount(best->chord.modifiers)) {
          continue;
        }
        best = KeybindMatch{command, chord, received.modifiers & ~chord.modifiers};
      }
    }
    return best;
  }

  std::string keybindShortcutLabel(const KeybindChords& chords) {
    std::string label;
    for (const auto& chord : chords) {
      if (!chord) {
        continue;
      }
      if (!label.empty()) {
        label += " / ";
      }
      label += keybindChordLabel(*chord);
    }
    return label;
  }

  std::string keybindShortcutLabel(const KeybindConfig& config, KeybindCommand command) {
    return keybindShortcutLabel(config.chords(command));
  }

  std::string keybindShortcutLabel(KeybindCommand command) { return keybindShortcutLabel(activeKeybinds(), command); }

  std::optional<KeybindCommand>
  keybindConflict(const KeybindConfig& config, KeybindCommand command, std::size_t slot, const KeybindChord& chord) {
    const KeybindChord target = normalizeKeybindChord(chord);
    for (const auto candidate : keybindCommands(keybindCommandScope(command))) {
      const auto& chords = config.chords(candidate);
      for (std::size_t index = 0; index < chords.size(); ++index) {
        if (candidate == command && index == slot) {
          continue;
        }
        if (chords[index] && normalizeKeybindChord(*chords[index]) == target) {
          return candidate;
        }
      }
    }
    return std::nullopt;
  }

  bool keybindConfigValid(const KeybindConfig& config) {
    for (const auto scope : kKeybindScopes) {
      for (const auto command : keybindCommands(scope)) {
        const bool optionalPrintable = command == KeybindCommand::Validate || command == KeybindCommand::QuickPreview;
        const auto& chords = config.chords(command);
        for (std::size_t slot = 0; slot < chords.size(); ++slot) {
          if (!chords[slot]) {
            continue;
          }
          const auto& chord = *chords[slot];
          if (printableConfigKey(chord.sym)
              && ((chord.modifiers == 0 && !optionalPrintable) || chord.modifiers == KeybindMod::Shift)) {
            return false;
          }
          if (keybindConflict(config, command, slot, chord)) {
            return false;
          }
        }
      }
    }
    return true;
  }

  void
  setKeybindChord(KeybindConfig& config, KeybindCommand command, std::size_t slot, std::optional<KeybindChord> chord) {
    auto& chords = config.chords(command);
    if (slot >= chords.size()) {
      return;
    }
    KeybindChords compact{};
    std::size_t count = 0;
    for (std::size_t index = 0; index < chords.size(); ++index) {
      if (index != slot && chords[index]) {
        compact[count++] = chords[index];
      }
    }
    if (chord && chord->sym != 0) {
      const std::size_t at = std::min(slot, count);
      if (at < compact.size()) {
        for (std::size_t index = std::min(count, compact.size() - 1); index > at; --index) {
          compact[index] = compact[index - 1];
        }
        compact[at] = normalizeKeybindChord(*chord);
      }
    }
    chords = compact;
  }

} // namespace luciole

template <class T> T& luciole::preferenceValue(BrowserSettings& settings, PreferenceKey<T> key) {
  T* field = nullptr;
  visitPreference(key.id(), [&](auto index) {
    const auto& descriptor = std::get<index>(kPreferences);
    // A key's type always matches its descriptor; other instantiations never run.
    if constexpr (std::same_as<typename std::remove_cvref_t<decltype(descriptor)>::Value, T>) {
      field = &descriptor.field(settings);
    }
  });
  return *field;
}

template <class T> const T& luciole::preferenceValue(const BrowserSettings& settings, PreferenceKey<T> key) {
  return preferenceValue(const_cast<BrowserSettings&>(settings), key);
}

template std::string& luciole::preferenceValue(BrowserSettings&, PreferenceKey<std::string>);
template const std::string& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<std::string>);
template luciole::KeybindConfig& luciole::preferenceValue(BrowserSettings&, PreferenceKey<KeybindConfig>);
template const luciole::KeybindConfig& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<KeybindConfig>);
template bool& luciole::preferenceValue(BrowserSettings&, PreferenceKey<bool>);
template int& luciole::preferenceValue(BrowserSettings&, PreferenceKey<int>);
template double& luciole::preferenceValue(BrowserSettings&, PreferenceKey<double>);
template luciole::ActivationMode& luciole::preferenceValue(BrowserSettings&, PreferenceKey<ActivationMode>);
template luciole::TypingMode& luciole::preferenceValue(BrowserSettings&, PreferenceKey<TypingMode>);
template luciole::DropAction& luciole::preferenceValue(BrowserSettings&, PreferenceKey<DropAction>);
template luciole::SortField& luciole::preferenceValue(BrowserSettings&, PreferenceKey<SortField>);
template luciole::ViewMode& luciole::preferenceValue(BrowserSettings&, PreferenceKey<ViewMode>);
template luciole::ViewDensity& luciole::preferenceValue(BrowserSettings&, PreferenceKey<ViewDensity>);
template luciole::FilenameTruncation& luciole::preferenceValue(BrowserSettings&, PreferenceKey<FilenameTruncation>);
template luciole::DetailsColumnOrder& luciole::preferenceValue(BrowserSettings&, PreferenceKey<DetailsColumnOrder>);
template luciole::ThumbnailPolicy& luciole::preferenceValue(BrowserSettings&, PreferenceKey<ThumbnailPolicy>);
template luciole::ChooserStartFolder& luciole::preferenceValue(BrowserSettings&, PreferenceKey<ChooserStartFolder>);
template const bool& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<bool>);
template const int& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<int>);
template const double& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<double>);
template const luciole::ActivationMode& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<ActivationMode>);
template const luciole::TypingMode& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<TypingMode>);
template const luciole::DropAction& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<DropAction>);
template const luciole::SortField& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<SortField>);
template const luciole::ViewMode& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<ViewMode>);
template const luciole::ViewDensity& luciole::preferenceValue(const BrowserSettings&, PreferenceKey<ViewDensity>);
template const luciole::FilenameTruncation&
luciole::preferenceValue(const BrowserSettings&, PreferenceKey<FilenameTruncation>);
template const luciole::DetailsColumnOrder&
luciole::preferenceValue(const BrowserSettings&, PreferenceKey<DetailsColumnOrder>);
template const luciole::ThumbnailPolicy&
luciole::preferenceValue(const BrowserSettings&, PreferenceKey<ThumbnailPolicy>);
template const luciole::ChooserStartFolder&
luciole::preferenceValue(const BrowserSettings&, PreferenceKey<ChooserStartFolder>);

std::string_view luciole::sortFieldName(SortField field) {
  const auto* found = std::ranges::find(kSortFields, field, &Named<SortField>::value);
  return found != kSortFields.end() ? found->name : std::string_view{};
}
std::optional<luciole::SortField> luciole::sortFieldFromName(std::string_view name) {
  const auto* found = std::ranges::find(kSortFields, name, &Named<SortField>::name);
  return found != kSortFields.end() ? std::optional(found->value) : std::nullopt;
}
std::string_view luciole::viewModeName(ViewMode mode) {
  const auto* found = std::ranges::find(kViewModes, mode, &Named<ViewMode>::value);
  return found != kViewModes.end() ? found->name : std::string_view{};
}
std::optional<luciole::ViewMode> luciole::viewModeFromName(std::string_view name) {
  const auto* found = std::ranges::find(kViewModes, name, &Named<ViewMode>::name);
  return found != kViewModes.end() ? std::optional(found->value) : std::nullopt;
}

std::vector<luciole::OverrideEdit> luciole::inheritAllPreferences() {
  std::vector<OverrideEdit> edits;
  edits.reserve(kPreferenceCount);
  forEachPreference([&](auto index) { edits.emplace_back(std::get<index>(kPreferences).preference, std::nullopt); });
  return edits;
}

luciole::ConfigPaths luciole::ConfigPaths::fromEnvironment() {
  const auto directory = configHome() / "luciole";
  return {.base = directory / "config.toml", .overrides = directory / "settings.toml"};
}

luciole::ConfigIssues luciole::validateConfigFile(const std::filesystem::path& file) {
  auto text = readFile(file, false);
  if (!text) {
    return {std::move(text.error())};
  }
  auto parsed = parse(file, **text);
  return parsed ? ConfigIssues{} : std::move(parsed.error());
}

static luciole::SettingsResult loadLayers(const luciole::ConfigPaths& paths, bool overrides) {
  luciole::Settings settings;
  luciole::ConfigIssues issues;
  for (const auto* file : {&paths.base, &paths.overrides}) {
    if (file == &paths.overrides && !overrides) {
      continue;
    }
    auto text = readFile(*file, true);
    if (!text) {
      issues.push_back(std::move(text.error()));
      continue;
    }
    if (!*text) {
      continue;
    }
    auto parsed = parse(
        *file, **text, UnknownSettings::Warn, nullptr, file == &paths.overrides ? &settings.browser.keybinds : nullptr
    );
    if (!parsed) {
      auto& errors = parsed.error();
      issues.insert(issues.end(), std::make_move_iterator(errors.begin()), std::make_move_iterator(errors.end()));
    } else {
      applyPatch(settings, *parsed);
    }
  }
  if (!issues.empty()) {
    return std::unexpected(std::move(issues));
  }
  return settings;
}

luciole::SettingsResult luciole::loadSettings(const ConfigPaths& paths) { return loadLayers(paths, true); }
luciole::SettingsResult luciole::loadInheritedSettings(const ConfigPaths& paths) { return loadLayers(paths, false); }

luciole::ConfigService::ConfigService(ConfigPaths paths) : m_paths(std::move(paths)) {}

bool luciole::ConfigService::reload() {
  auto loaded = loadSettings(m_paths);
  if (!loaded) {
    m_issues = std::move(loaded.error());
    return false;
  }
  m_settings = *loaded;
  m_issues.clear();
  return true;
}

luciole::AtomicWriteState luciole::ConfigService::saveOverrides(std::span<const OverrideEdit> edits) {
  if (edits.empty()) {
    return AtomicWriteState::Unchanged;
  }
  m_issues.clear();
  if (!m_paths.base.is_absolute()
      || !m_paths.overrides.is_absolute()
      || m_paths.base.filename() != "config.toml"
      || m_paths.overrides.filename() != "settings.toml"
      || m_paths.base.parent_path().lexically_normal() != m_paths.overrides.parent_path().lexically_normal()) {
    m_issues.push_back(
        ioIssue(m_paths.overrides, "saves require distinct config.toml and settings.toml in one absolute directory")
    );
    return AtomicWriteState::NotCommitted;
  }
  for (const auto& edit : edits) {
    visitPreference(edit.preference(), [&](auto index) {
      const auto& descriptor = std::get<index>(kPreferences);
      if (const auto value = edit.value(descriptor.preference); value && !accepts(descriptor.rule, *value)) {
        m_issues.push_back(
            {.file = m_paths.overrides, .key = preferenceKey(descriptor), .message = "invalid preference value"}
        );
      }
    });
  }
  if (!m_issues.empty()) {
    return AtomicWriteState::NotCommitted;
  }
  Settings candidate;
  Settings inherited;
  const auto result = updateTextFileAtomic(
      m_paths.overrides,
      [&](const std::optional<std::string>& current) -> std::expected<std::optional<std::string>, std::string> {
        std::error_code aliasError;
        if (std::filesystem::equivalent(m_paths.base, m_paths.overrides, aliasError)) {
          return std::unexpected("base configuration and GUI overrides refer to the same file");
        }
        if (aliasError && aliasError != std::errc::no_such_file_or_directory) {
          return std::unexpected(std::format("cannot inspect base configuration: {}", aliasError.message()));
        }
        const auto base = readFile(m_paths.base, true);
        if (!base) {
          m_issues.push_back(base.error());
          return std::unexpected("cannot load base configuration");
        }
        if (*base) {
          const auto parsed = parse(m_paths.base, **base, UnknownSettings::Warn);
          if (!parsed) {
            m_issues = parsed.error();
            return std::unexpected("invalid base configuration");
          }
          applyPatch(candidate, *parsed);
          applyPatch(inherited, *parsed);
        }
        toml::table document;
        auto patch = parse(
            m_paths.overrides, current.value_or(""), UnknownSettings::Warn, &document, &inherited.browser.keybinds
        );
        if (!patch) {
          m_issues = std::move(patch.error());
          return std::unexpected("invalid existing settings");
        }
        const auto previous = *patch;
        for (const auto& edit : edits) {
          editPatch(*patch, edit);
        }
        applyPatch(candidate, *patch);
        if (*patch == previous) {
          return std::nullopt;
        }
        return formatPatch(*patch, std::move(document), inherited.browser);
      }
  );
  if (result.state != AtomicWriteState::NotCommitted) {
    m_settings = std::move(candidate);
  }
  if (!result.error.empty() && m_issues.empty()) {
    m_issues.push_back(ioIssue(m_paths.overrides, result.error));
  }
  return result.state;
}
