/* Copyright (C) 2019 Mr Goldberg
   This file is part of the Goldberg Emulator

   The Goldberg Emulator is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   The Goldberg Emulator is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the Goldberg Emulator; if not, see
   <http://www.gnu.org/licenses/>.  */

#include "dll/steam_controller.h"

#define JOY_ID_START 10
#define STICK_DPAD 3
#define DEADZONE_BUTTON_STICK 0.3



#if !defined(CONTROLLER_SUPPORT)

inline void GamepadInit(void) {}
inline void GamepadShutdown(void) {}
inline void GamepadUpdate(void) {}
inline GAMEPAD_BOOL GamepadIsConnected(GAMEPAD_DEVICE device) { return GAMEPAD_FALSE; }
inline GAMEPAD_BOOL GamepadButtonDown(GAMEPAD_DEVICE device, GAMEPAD_BUTTON button) { return GAMEPAD_FALSE; }
inline float GamepadTriggerLength(GAMEPAD_DEVICE device, GAMEPAD_TRIGGER trigger) { return 0.0; }
inline GAMEPAD_STICKDIR GamepadStickDir(GAMEPAD_DEVICE device, GAMEPAD_STICK stick) { return STICKDIR_CENTER; }
inline void GamepadStickNormXY(GAMEPAD_DEVICE device, GAMEPAD_STICK stick, float* outX, float* outY) {}
inline float GamepadStickLength(GAMEPAD_DEVICE device, GAMEPAD_STICK stick) { return 0.0; }
inline void GamepadSetRumble(GAMEPAD_DEVICE device, float left, float right,  unsigned int rumble_length_ms) {}

#endif

namespace {

using Action_Button_Map = std::map<std::string, std::pair<std::set<std::string>, std::string>>;
using Action_Set_Map = std::map<std::string, Action_Button_Map>;

struct VdfEntry {
    std::map<std::string, std::vector<VdfEntry>> children{};
    std::string value{};
    bool is_string{};
};

class VdfParser {
public:
    explicit VdfParser(std::string text)
        : text(std::move(text))
    {}

    VdfEntry parse()
    {
        return parse_object(false);
    }

private:
    std::string text;
    size_t pos{};

    static bool is_space(char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    void skip_ignored()
    {
        while (pos < text.size()) {
            if (is_space(text[pos])) {
                ++pos;
                continue;
            }

            if ((pos + 1) < text.size() && text[pos] == '/' && text[pos + 1] == '/') {
                pos += 2;
                while (pos < text.size() && text[pos] != '\n') {
                    ++pos;
                }
                continue;
            }

            break;
        }
    }

    std::string parse_string()
    {
        skip_ignored();
        if (pos >= text.size()) return {};

        if (text[pos] == '"') {
            ++pos;
            std::string out{};
            while (pos < text.size()) {
                char c = text[pos++];
                if (c == '\\' && pos < text.size()) {
                    out.push_back(text[pos++]);
                    continue;
                }

                if (c == '"') {
                    break;
                }

                out.push_back(c);
            }
            return out;
        }

        size_t start = pos;
        while (pos < text.size() && !is_space(text[pos]) && text[pos] != '{' && text[pos] != '}') {
            ++pos;
        }
        return text.substr(start, pos - start);
    }

    VdfEntry parse_object(bool expect_closing_brace)
    {
        VdfEntry obj{};

        while (true) {
            skip_ignored();
            if (pos >= text.size()) break;

            if (text[pos] == '}') {
                if (expect_closing_brace) {
                    ++pos;
                }
                break;
            }

            std::string key = parse_string();
            if (key.empty()) break;

            skip_ignored();

            VdfEntry child{};
            if (pos < text.size() && text[pos] == '{') {
                ++pos;
                child = parse_object(true);
            } else {
                child.value = parse_string();
                child.is_string = true;
            }

            obj.children[key].push_back(std::move(child));
        }

        return obj;
    }
};

static std::string str_to_upper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c){ return std::toupper(c); });
    return value;
}

static std::string str_to_lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c){ return std::tolower(c); });
    return value;
}

static bool looks_like_json(std::string_view value)
{
    for (char c : value) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        return c == '{' || c == '[';
    }

    return false;
}

static VdfEntry json_to_vdf_entry(const nlohmann::json &value)
{
    VdfEntry entry{};
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it->is_array()) {
                for (auto &item : *it) {
                    entry.children[it.key()].push_back(json_to_vdf_entry(item));
                }
            } else {
                entry.children[it.key()].push_back(json_to_vdf_entry(*it));
            }
        }

        return entry;
    }

    if (value.is_array()) {
        for (auto &item : value) {
            entry.children["item"].push_back(json_to_vdf_entry(item));
        }

        return entry;
    }

    entry.is_string = true;
    if (value.is_string()) {
        entry.value = value.get<std::string>();
    } else if (value.is_boolean()) {
        entry.value = value.get<bool>() ? "1" : "0";
    } else if (value.is_number_unsigned()) {
        entry.value = std::to_string(value.get<uint64_t>());
    } else if (value.is_number_integer()) {
        entry.value = std::to_string(value.get<int64_t>());
    } else if (value.is_number_float()) {
        entry.value = value.dump();
    }

    return entry;
}

static bool load_controller_mapping_tree(const std::filesystem::path &config_path, VdfEntry &root)
{
    std::ifstream input(config_path);
    if (!input.is_open()) return false;
    common_helpers::consume_bom(input);

    std::stringstream buffer{};
    buffer << input.rdbuf();
    std::string text = buffer.str();

    const std::string extension = str_to_lower(config_path.extension().string());
    if (extension == ".json" || looks_like_json(text)) {
        try {
            root = json_to_vdf_entry(nlohmann::json::parse(text));
            return true;
        } catch (const nlohmann::json::exception &) {
            if (extension == ".json") {
                return false;
            }
        }
    }

    VdfParser parser(std::move(text));
    root = parser.parse();
    return true;
}

static const std::vector<VdfEntry>* find_children_ci(const VdfEntry &obj, const std::string_view key)
{
    for (auto &entry : obj.children) {
        if (common_helpers::str_cmp_insensitive(entry.first, key)) {
            return &entry.second;
        }
    }
    return nullptr;
}

static const VdfEntry* find_first_child_ci(const VdfEntry &obj, const std::string_view key)
{
    auto children = find_children_ci(obj, key);
    if (!children || children->empty()) return nullptr;
    return &children->front();
}

static bool has_child_ci(const VdfEntry &obj, const std::string_view key)
{
    return find_children_ci(obj, key) != nullptr;
}

static std::string get_first_string_ci(const VdfEntry &obj, const std::string_view key)
{
    auto child = find_first_child_ci(obj, key);
    if (!child || !child->is_string) return {};
    return child->value;
}

static const VdfEntry* find_manifest_root(const VdfEntry &root)
{
    if (has_child_ci(root, "configurations") || has_child_ci(root, "controller_mappings")) {
        return &root;
    }

    for (auto &entry : root.children) {
        for (auto &child : entry.second) {
            if (has_child_ci(child, "configurations") || has_child_ci(child, "controller_mappings")) {
                return &child;
            }
        }
    }

    return nullptr;
}

static std::vector<std::string> split_ws(const std::string &value)
{
    std::istringstream input(value);
    std::vector<std::string> out{};
    std::string part{};
    while (input >> part) {
        out.push_back(part);
    }
    return out;
}

static std::filesystem::path resolve_vdf_path(const std::filesystem::path &base_dir, std::string raw_path)
{
    if (raw_path.empty()) return {};

    auto scheme_sep = raw_path.find("://");
    if (scheme_sep != std::string::npos) {
        raw_path = raw_path.substr(scheme_sep + 3);
    }

    std::filesystem::path path = std::filesystem::u8path(raw_path);
    if (path.is_absolute()) return path;
    return base_dir / path;
}

static bool add_action_binding(
    std::map<std::string, std::map<std::string, std::pair<std::set<std::string>, std::string>>> &action_sets,
    const std::string &action_set_name,
    std::string action_name,
    std::string binding,
    std::string source_mode = {}
)
{
    if (action_set_name.empty() || action_name.empty() || binding.empty()) return false;

    action_name = str_to_upper(std::move(action_name));
    binding = str_to_upper(std::move(binding));
    auto &action_binding = action_sets[str_to_upper(action_set_name)][action_name];
    action_binding.first.insert(binding);
    if (!source_mode.empty() && action_binding.second.empty()) {
        action_binding.second = std::move(source_mode);
    }
    return true;
}

static void collect_binding_strings(const VdfEntry &obj, std::vector<std::string> &bindings)
{
    for (auto &entry : obj.children) {
        if (common_helpers::str_cmp_insensitive(entry.first, "binding")) {
            for (auto &binding_value : entry.second) {
                if (binding_value.is_string) {
                    bindings.push_back(binding_value.value);
                }
            }
            continue;
        }

        for (auto &child : entry.second) {
            collect_binding_strings(child, bindings);
        }
    }
}

static void add_group_input_bindings(
    std::map<std::string, std::map<std::string, std::pair<std::set<std::string>, std::string>>> &action_sets,
    const std::string &action_set_name,
    const VdfEntry &group,
    const std::map<std::string, std::string> &keymap,
    const std::string &forced_button = {}
)
{
    auto inputs = find_first_child_ci(group, "inputs");
    if (!inputs) return;

    for (auto &input_entry : inputs->children) {
        const std::string input_name_lower = str_to_lower(input_entry.first);
        std::string button_binding = forced_button;
        if (button_binding.empty()) {
            auto mapped = keymap.find(input_name_lower);
            if (mapped == keymap.end()) continue;
            button_binding = mapped->second;
        }

        for (auto &input_definition : input_entry.second) {
            std::vector<std::string> bindings{};
            collect_binding_strings(input_definition, bindings);
            for (auto &binding_value : bindings) {
                auto binding_parts = split_ws(binding_value);
                if (binding_parts.size() < 2) continue;

                std::string action_name{};
                if (common_helpers::str_cmp_insensitive(binding_parts[0], "game_action") && binding_parts.size() >= 3) {
                    action_name = binding_parts[2];
                } else if (common_helpers::str_cmp_insensitive(binding_parts[0], "action") && binding_parts.size() >= 2) {
                    action_name = binding_parts[1];
                } else if (common_helpers::str_cmp_insensitive(binding_parts[0], "xinput_button") && binding_parts.size() >= 2) {
                    action_name = binding_parts[1];
                }

                if (!action_name.empty() && action_name.back() == ',') {
                    action_name.pop_back();
                }

                add_action_binding(action_sets, action_set_name, action_name, button_binding);
            }
        }
    }
}

static bool load_controller_mappings_from_vdf(
    const VdfEntry &controller_mappings,
    Action_Set_Map &action_sets,
    std::map<std::string, std::string> &action_set_layer_parents,
    Action_Set_Map &action_set_layers,
    std::string &default_action_set_name
)
{
    static const std::map<std::string, std::string> keymap_digital = {
        {"button_a", "A"},
        {"button_b", "B"},
        {"button_x", "X"},
        {"button_y", "Y"},
        {"dpad_north", "DUP"},
        {"dpad_south", "DDOWN"},
        {"dpad_east", "DRIGHT"},
        {"dpad_west", "DLEFT"},
        {"button_escape", "START"},
        {"button_menu", "BACK"},
        {"left_bumper", "LBUMPER"},
        {"right_bumper", "RBUMPER"},
        {"button_back_left", "Y"},
        {"button_back_right", "A"},
        {"button_back_left_upper", "X"},
        {"button_back_right_upper", "B"},
    };
    static const std::map<std::string, std::string> keymap_left_joystick = {
        {"dpad_north", "DLJOYUP"},
        {"dpad_south", "DLJOYDOWN"},
        {"dpad_west", "DLJOYLEFT"},
        {"dpad_east", "DLJOYRIGHT"},
        {"click", "LSTICK"},
    };
    static const std::map<std::string, std::string> keymap_right_joystick = {
        {"dpad_north", "DRJOYUP"},
        {"dpad_south", "DRJOYDOWN"},
        {"dpad_west", "DRJOYLEFT"},
        {"dpad_east", "DRJOYRIGHT"},
        {"click", "RSTICK"},
    };
    static const std::set<std::string> supported_keys_digital = {"switch", "button_diamond", "dpad"};
    static const std::set<std::string> supported_keys_triggers = {"left_trigger", "right_trigger"};
    static const std::set<std::string> supported_keys_joystick = {"joystick", "right_joystick", "dpad"};

    std::map<std::string, const VdfEntry*> groups_by_id{};
    if (auto groups = find_children_ci(controller_mappings, "group")) {
        for (auto &group : *groups) {
            std::string group_id = get_first_string_ci(group, "id");
            if (!group_id.empty()) {
                groups_by_id[group_id] = &group;
            }
        }
    }

    std::set<std::string> supported_action_sets{};
    if (auto actions = find_first_child_ci(controller_mappings, "actions")) {
        for (auto &action_entry : actions->children) {
            supported_action_sets.insert(str_to_upper(action_entry.first));
        }
    }

    std::set<std::string> supported_action_layers{};
    if (auto action_layers = find_first_child_ci(controller_mappings, "action_layers")) {
        for (auto &layer_entry : action_layers->children) {
            const std::string layer_name_upper = str_to_upper(layer_entry.first);
            supported_action_layers.insert(layer_name_upper);
            for (auto &layer_value : layer_entry.second) {
                std::string parent_name = get_first_string_ci(layer_value, "parent_set_name");
                if (!parent_name.empty()) {
                    action_set_layer_parents[layer_name_upper] = str_to_upper(parent_name);
                    break;
                }
            }
        }
    }

    auto presets = find_children_ci(controller_mappings, "preset");
    if (!presets) return false;

    bool loaded_any{};
    for (auto &preset : *presets) {
        const std::string preset_name = get_first_string_ci(preset, "name");
        if (preset_name.empty()) continue;

        const std::string preset_name_upper = str_to_upper(preset_name);
        const bool is_action_layer =
            get_first_string_ci(preset, "set_layer") == "1" ||
            has_child_ci(preset, "parent_set_name") ||
            supported_action_layers.count(preset_name_upper);

        if (!is_action_layer && preset_name_upper != "DEFAULT" && !supported_action_sets.count(preset_name_upper)) {
            continue;
        }

        auto group_source_bindings = find_first_child_ci(preset, "group_source_bindings");
        if (!group_source_bindings) continue;

        Action_Set_Map &target_maps = is_action_layer ? action_set_layers : action_sets;
        const std::string target_name = preset_name_upper;
        if (!is_action_layer && default_action_set_name.empty()) {
            default_action_set_name = target_name;
        }

        if (is_action_layer) {
            std::string parent_name = get_first_string_ci(preset, "parent_set_name");
            if (parent_name.empty()) {
                auto parent = action_set_layer_parents.find(target_name);
                if (parent != action_set_layer_parents.end()) {
                    parent_name = parent->second;
                }
            } else {
                parent_name = str_to_upper(parent_name);
            }

            if (!parent_name.empty()) {
                action_set_layer_parents[target_name] = parent_name;
            }
        }

        for (auto &binding_entry : group_source_bindings->children) {
            auto group = groups_by_id.find(binding_entry.first);
            if (group == groups_by_id.end()) continue;

            for (auto &binding_desc : binding_entry.second) {
                if (!binding_desc.is_string) continue;
                auto binding_parts = split_ws(binding_desc.value);
                if (binding_parts.size() < 2 || !common_helpers::str_cmp_insensitive(binding_parts[1], "active")) {
                    continue;
                }

                const std::string binding_name = str_to_lower(binding_parts[0]);
                const VdfEntry &group_object = *group->second;
                if (supported_keys_digital.count(binding_name)) {
                    add_group_input_bindings(target_maps, target_name, group_object, keymap_digital);
                    loaded_any = true;
                }

                const std::string group_mode = str_to_lower(get_first_string_ci(group_object, "mode"));
                if (supported_keys_triggers.count(binding_name) && group_mode == "trigger") {
                    if (auto gameactions = find_first_child_ci(group_object, "gameactions")) {
                        std::string action_name = get_first_string_ci(*gameactions, preset_name);
                        if (!action_name.empty()) {
                            loaded_any |= add_action_binding(
                                target_maps,
                                target_name,
                                action_name,
                                binding_name == "left_trigger" ? "LTRIGGER" : "RTRIGGER",
                                "trigger"
                            );
                        }
                    }

                    add_group_input_bindings(
                        target_maps,
                        target_name,
                        group_object,
                        keymap_digital,
                        binding_name == "left_trigger" ? "DLTRIGGER" : "DRTRIGGER"
                    );
                    loaded_any = true;
                }

                if (supported_keys_joystick.count(binding_name) && group_mode == "joystick_move") {
                    if (auto gameactions = find_first_child_ci(group_object, "gameactions")) {
                        std::string action_name = get_first_string_ci(*gameactions, preset_name);
                        if (!action_name.empty()) {
                            std::string analog_binding = "DPAD";
                            if (binding_name == "joystick") {
                                analog_binding = "LJOY";
                            } else if (binding_name == "right_joystick") {
                                analog_binding = "RJOY";
                            }

                            loaded_any |= add_action_binding(
                                target_maps,
                                target_name,
                                action_name,
                                analog_binding,
                                "joystick_move"
                            );
                        }
                    }

                    if (binding_name == "joystick") {
                        add_group_input_bindings(target_maps, target_name, group_object, keymap_digital, "LSTICK");
                        loaded_any = true;
                    } else if (binding_name == "right_joystick") {
                        add_group_input_bindings(target_maps, target_name, group_object, keymap_digital, "RSTICK");
                        loaded_any = true;
                    }
                } else if (supported_keys_joystick.count(binding_name) && group_mode == "dpad") {
                    if (binding_name == "joystick") {
                        add_group_input_bindings(target_maps, target_name, group_object, keymap_left_joystick);
                        loaded_any = true;
                    } else if (binding_name == "right_joystick") {
                        add_group_input_bindings(target_maps, target_name, group_object, keymap_right_joystick);
                        loaded_any = true;
                    }
                }
            }
        }
    }

    return loaded_any;
}

static bool load_controller_mappings_vdf_file(
    const std::filesystem::path &config_path,
    Action_Set_Map &action_sets,
    std::map<std::string, std::string> &action_set_layer_parents,
    Action_Set_Map &action_set_layers,
    std::string &default_action_set_name
)
{
    VdfEntry root{};
    if (!load_controller_mapping_tree(config_path, root)) return false;

    const VdfEntry *controller_mappings = find_first_child_ci(root, "controller_mappings");
    if (!controller_mappings) {
        controller_mappings = find_manifest_root(root);
        if (controller_mappings && !has_child_ci(*controller_mappings, "controller_mappings")) {
            controller_mappings = nullptr;
        } else if (controller_mappings) {
            controller_mappings = find_first_child_ci(*controller_mappings, "controller_mappings");
        }
    }

    if (!controller_mappings) return false;
    return load_controller_mappings_from_vdf(*controller_mappings, action_sets, action_set_layer_parents, action_set_layers, default_action_set_name);
}

static bool load_action_manifest_vdf(
    const std::filesystem::path &manifest_path,
    Action_Set_Map &action_sets,
    std::map<std::string, std::string> &action_set_layer_parents,
    Action_Set_Map &action_set_layers,
    std::string &default_action_set_name
)
{
    VdfEntry root{};
    if (!load_controller_mapping_tree(manifest_path, root)) return false;

    const VdfEntry *manifest_root = find_manifest_root(root);
    if (!manifest_root) return false;

    if (has_child_ci(*manifest_root, "controller_mappings")) {
        auto controller_mappings = find_first_child_ci(*manifest_root, "controller_mappings");
        return controller_mappings && load_controller_mappings_from_vdf(*controller_mappings, action_sets, action_set_layer_parents, action_set_layers, default_action_set_name);
    }

    auto configurations = find_first_child_ci(*manifest_root, "configurations");
    if (!configurations) return false;

    static const std::vector<std::string> controller_types = {
        "controller_xboxone",
        "controller_xbox360",
        "controller_steamcontroller_gordon",
        "controller_ps4",
        "controller_ps5",
        "controller_switch_pro",
        "controller_neptune",
    };

    for (auto &controller_type : controller_types) {
        auto configs_for_type = find_first_child_ci(*configurations, controller_type);
        if (!configs_for_type) continue;

        for (auto &config_entry : configs_for_type->children) {
            for (auto &config_value : config_entry.second) {
                std::string path_string = get_first_string_ci(config_value, "path");
                if (path_string.empty()) {
                    path_string = get_first_string_ci(config_value, "url");
                }

                auto config_path = resolve_vdf_path(manifest_path.parent_path(), path_string);
                if (config_path.empty()) continue;
                if (load_controller_mappings_vdf_file(config_path, action_sets, action_set_layer_parents, action_set_layers, default_action_set_name)) {
                    return true;
                }
            }
        }
    }

    return false;
}

static const char *debug_input_source_mode_name(enum EInputSourceMode mode)
{
    switch (mode) {
    case k_EInputSourceMode_None: return "None";
    case k_EInputSourceMode_Dpad: return "Dpad";
    case k_EInputSourceMode_Buttons: return "Buttons";
    case k_EInputSourceMode_FourButtons: return "FourButtons";
    case k_EInputSourceMode_AbsoluteMouse: return "AbsoluteMouse";
    case k_EInputSourceMode_RelativeMouse: return "RelativeMouse";
    case k_EInputSourceMode_JoystickMove: return "JoystickMove";
    case k_EInputSourceMode_JoystickMouse: return "JoystickMouse";
    case k_EInputSourceMode_JoystickCamera: return "JoystickCamera";
    case k_EInputSourceMode_ScrollWheel: return "ScrollWheel";
    case k_EInputSourceMode_Trigger: return "Trigger";
    case k_EInputSourceMode_TouchMenu: return "TouchMenu";
    case k_EInputSourceMode_MouseJoystick: return "MouseJoystick";
    case k_EInputSourceMode_MouseRegion: return "MouseRegion";
    case k_EInputSourceMode_RadialMenu: return "RadialMenu";
    case k_EInputSourceMode_SingleButton: return "SingleButton";
    case k_EInputSourceMode_Switches: return "Switches";
    default: return "Unknown";
    }
}

static std::string debug_describe_digital_binding(int button)
{
    switch (button) {
    case BUTTON_A: return "A";
    case BUTTON_B: return "B";
    case BUTTON_X: return "X";
    case BUTTON_Y: return "Y";
    case BUTTON_LEFT_SHOULDER: return "LB";
    case BUTTON_RIGHT_SHOULDER: return "RB";
    case BUTTON_LEFT_THUMB: return "LTHUMB";
    case BUTTON_RIGHT_THUMB: return "RTHUMB";
    case BUTTON_BACK: return "BACK";
    case BUTTON_START: return "START";
    case BUTTON_DPAD_UP: return "DUP";
    case BUTTON_DPAD_DOWN: return "DDOWN";
    case BUTTON_DPAD_LEFT: return "DLEFT";
    case BUTTON_DPAD_RIGHT: return "DRIGHT";
    case BUTTON_LTRIGGER: return "LTRIGGER_BTN";
    case BUTTON_RTRIGGER: return "RTRIGGER_BTN";
    case BUTTON_STICK_LEFT_UP: return "LSTICK_UP";
    case BUTTON_STICK_LEFT_DOWN: return "LSTICK_DOWN";
    case BUTTON_STICK_LEFT_LEFT: return "LSTICK_LEFT";
    case BUTTON_STICK_LEFT_RIGHT: return "LSTICK_RIGHT";
    case BUTTON_STICK_RIGHT_UP: return "RSTICK_UP";
    case BUTTON_STICK_RIGHT_DOWN: return "RSTICK_DOWN";
    case BUTTON_STICK_RIGHT_LEFT: return "RSTICK_LEFT";
    case BUTTON_STICK_RIGHT_RIGHT: return "RSTICK_RIGHT";
    default:
        if (button >= JOY_ID_START) {
            return "JOY_" + std::to_string(button - JOY_ID_START);
        }
        return "BTN_" + std::to_string(button);
    }
}

static std::string debug_describe_binding_set(const std::set<int> &bindings)
{
    std::string out;
    for (auto binding : bindings) {
        if (!out.empty()) {
            out += ",";
        }
        out += debug_describe_digital_binding(binding);
    }
    return out;
}

static std::string debug_build_raw_gamepad_snapshot(GAMEPAD_DEVICE device)
{
    float left_x = 0.0f, left_y = 0.0f, right_x = 0.0f, right_y = 0.0f;
    GamepadStickNormXY(device, STICK_LEFT, &left_x, &left_y);
    GamepadStickNormXY(device, STICK_RIGHT, &right_x, &right_y);

    const float left_len = GamepadStickLength(device, STICK_LEFT);
    const float right_len = GamepadStickLength(device, STICK_RIGHT);
    const float lt = GamepadTriggerLength(device, TRIGGER_LEFT);
    const float rt = GamepadTriggerLength(device, TRIGGER_RIGHT);

    char buffer[512];
    snprintf(
        buffer,
        sizeof(buffer),
        "A=%d B=%d X=%d Y=%d LB=%d RB=%d BACK=%d START=%d LTHUMB=%d RTHUMB=%d DUP=%d DDOWN=%d DLEFT=%d DRIGHT=%d LT=%.3f RT=%.3f LS=(%.3f,%.3f len=%.3f) RS=(%.3f,%.3f len=%.3f)",
        GamepadButtonDown(device, BUTTON_A),
        GamepadButtonDown(device, BUTTON_B),
        GamepadButtonDown(device, BUTTON_X),
        GamepadButtonDown(device, BUTTON_Y),
        GamepadButtonDown(device, BUTTON_LEFT_SHOULDER),
        GamepadButtonDown(device, BUTTON_RIGHT_SHOULDER),
        GamepadButtonDown(device, BUTTON_BACK),
        GamepadButtonDown(device, BUTTON_START),
        GamepadButtonDown(device, BUTTON_LEFT_THUMB),
        GamepadButtonDown(device, BUTTON_RIGHT_THUMB),
        GamepadButtonDown(device, BUTTON_DPAD_UP),
        GamepadButtonDown(device, BUTTON_DPAD_DOWN),
        GamepadButtonDown(device, BUTTON_DPAD_LEFT),
        GamepadButtonDown(device, BUTTON_DPAD_RIGHT),
        lt,
        rt,
        left_x,
        left_y,
        left_len,
        right_x,
        right_y,
        right_len
    );
    return buffer;
}

static void debug_log_raw_gamepad_state(const char *context, ControllerHandle_t controllerHandle)
{
    if (controllerHandle == 0) return;
    const int gamepad_index = static_cast<int>(controllerHandle) - 1;
    if (gamepad_index < 0 || gamepad_index >= GAMEPAD_COUNT) return;
    GAMEPAD_DEVICE device = (GAMEPAD_DEVICE)gamepad_index;
    if (!GamepadIsConnected(device)) return;
    const std::string snapshot = debug_build_raw_gamepad_snapshot(device);
    static std::map<ControllerHandle_t, std::string> previous_snapshots{};
    auto previous = previous_snapshots.find(controllerHandle);
    if (previous != previous_snapshots.end() && previous->second == snapshot) {
        return;
    }

    previous_snapshots[controllerHandle] = snapshot;
    PRINT_DEBUG("%s controller %llu raw_state %s", context, static_cast<unsigned long long>(controllerHandle), snapshot.c_str());
}

static void debug_log_action_snapshot(
    const char *context,
    ControllerHandle_t controllerHandle,
    uint64 actionHandle,
    const std::string &snapshot
)
{
    const std::string key =
        std::string(context) + ":" +
        std::to_string(controllerHandle) + ":" +
        std::to_string(actionHandle);
    static std::map<std::string, std::string> previous_snapshots{};
    auto previous = previous_snapshots.find(key);
    if (previous != previous_snapshots.end() && previous->second == snapshot) {
        return;
    }

    previous_snapshots[key] = snapshot;
    PRINT_DEBUG("%s", snapshot.c_str());
}

} // namespace


Controller_Action::Controller_Action(ControllerHandle_t controller_handle) {
    this->controller_handle = controller_handle;
}

static void merge_controller_map(struct Controller_Map &target, const struct Controller_Map &overlay)
{
    for (auto &entry : overlay.active_digital) {
        target.active_digital[entry.first] = entry.second;
    }

    for (auto &entry : overlay.active_analog) {
        target.active_analog[entry.first] = entry.second;
    }
}

void Controller_Action::rebuild_active_map(
    const std::map<ControllerActionSetHandle_t, struct Controller_Map> &controller_maps,
    const std::map<ControllerActionSetHandle_t, ControllerActionSetHandle_t> &action_set_layer_parents
)
{
    active_map = {};
    auto map = controller_maps.find(active_set);
    if (map == controller_maps.end()) return;

    active_map = map->second;
    for (auto &layer_handle : active_layers) {
        auto parent = action_set_layer_parents.find(layer_handle);
        if (parent == action_set_layer_parents.end() || parent->second != active_set) continue;

        auto layer_map = controller_maps.find(layer_handle);
        if (layer_map == controller_maps.end()) continue;
        merge_controller_map(active_map, layer_map->second);
    }
}

void Controller_Action::activate_action_set(
    ControllerActionSetHandle_t active_set,
    const std::map<ControllerActionSetHandle_t, struct Controller_Map> &controller_maps,
    const std::map<ControllerActionSetHandle_t, ControllerActionSetHandle_t> &action_set_layer_parents
)
{
    this->active_set = active_set;
    active_layers.clear();
    rebuild_active_map(controller_maps, action_set_layer_parents);
}

void Controller_Action::activate_action_set_layer(
    ControllerActionSetHandle_t active_layer,
    const std::map<ControllerActionSetHandle_t, struct Controller_Map> &controller_maps,
    const std::map<ControllerActionSetHandle_t, ControllerActionSetHandle_t> &action_set_layer_parents
)
{
    auto parent = action_set_layer_parents.find(active_layer);
    if (parent == action_set_layer_parents.end() || parent->second != active_set) return;

    if (std::find(active_layers.begin(), active_layers.end(), active_layer) == active_layers.end()) {
        active_layers.push_back(active_layer);
    }

    rebuild_active_map(controller_maps, action_set_layer_parents);
}

void Controller_Action::deactivate_action_set_layer(
    ControllerActionSetHandle_t active_layer,
    const std::map<ControllerActionSetHandle_t, struct Controller_Map> &controller_maps,
    const std::map<ControllerActionSetHandle_t, ControllerActionSetHandle_t> &action_set_layer_parents
)
{
    active_layers.erase(std::remove(active_layers.begin(), active_layers.end(), active_layer), active_layers.end());
    rebuild_active_map(controller_maps, action_set_layer_parents);
}

void Controller_Action::deactivate_all_action_set_layers(
    const std::map<ControllerActionSetHandle_t, struct Controller_Map> &controller_maps,
    const std::map<ControllerActionSetHandle_t, ControllerActionSetHandle_t> &action_set_layer_parents
)
{
    active_layers.clear();
    rebuild_active_map(controller_maps, action_set_layer_parents);
}

std::set<int> Controller_Action::button_id(ControllerDigitalActionHandle_t handle) {
    auto a = active_map.active_digital.find(handle);
    if (a == active_map.active_digital.end()) return {};
    return a->second;
}

std::pair<std::set<int>, enum EInputSourceMode> Controller_Action::analog_id(ControllerAnalogActionHandle_t handle) {
    auto a = active_map.active_analog.find(handle);
    if (a == active_map.active_analog.end()) return std::pair<std::set<int>, enum EInputSourceMode>({}, k_EInputSourceMode_None);
    return a->second;
}



const std::map<std::string, int> Steam_Controller::button_strings = {
    {"DUP", BUTTON_DPAD_UP},
    {"DDOWN", BUTTON_DPAD_DOWN},
    {"DLEFT", BUTTON_DPAD_LEFT},
    {"DRIGHT", BUTTON_DPAD_RIGHT},
    {"START", BUTTON_START},
    {"BACK", BUTTON_BACK},
    {"LSTICK", BUTTON_LEFT_THUMB},
    {"RSTICK", BUTTON_RIGHT_THUMB},
    {"LBUMPER", BUTTON_LEFT_SHOULDER},
    {"RBUMPER", BUTTON_RIGHT_SHOULDER},
    {"A", BUTTON_A},
    {"B", BUTTON_B},
    {"X", BUTTON_X},
    {"Y", BUTTON_Y},
    {"DLTRIGGER", BUTTON_LTRIGGER},
    {"DRTRIGGER", BUTTON_RTRIGGER},
    {"DLJOYUP", BUTTON_STICK_LEFT_UP},
    {"DLJOYDOWN", BUTTON_STICK_LEFT_DOWN},
    {"DLJOYLEFT", BUTTON_STICK_LEFT_LEFT},
    {"DLJOYRIGHT", BUTTON_STICK_LEFT_RIGHT},
    {"DRJOYUP", BUTTON_STICK_RIGHT_UP},
    {"DRJOYDOWN", BUTTON_STICK_RIGHT_DOWN},
    {"DRJOYLEFT", BUTTON_STICK_RIGHT_LEFT},
    {"DRJOYRIGHT", BUTTON_STICK_RIGHT_RIGHT},
};

const std::map<std::string, int> Steam_Controller::analog_strings = {
    {"LTRIGGER", TRIGGER_LEFT},
    {"RTRIGGER", TRIGGER_RIGHT},
    {"LJOY", STICK_LEFT + JOY_ID_START},
    {"RJOY", STICK_RIGHT + JOY_ID_START},
    {"DPAD", STICK_DPAD + JOY_ID_START},
};

const std::map<std::string, enum EInputSourceMode> Steam_Controller::analog_input_modes = {
    {"joystick_move", k_EInputSourceMode_JoystickMove},
    {"joystick_camera", k_EInputSourceMode_JoystickCamera},
    {"trigger", k_EInputSourceMode_Trigger},
};


void Steam_Controller::set_handles()
{
    action_handles.clear();
    digital_action_handles.clear();
    analog_action_handles.clear();
    controller_maps.clear();
    action_set_layer_parents.clear();

    uint64 handle_num = 1;
    auto add_action_set = [&](const std::string &set_name, const Action_Button_Map &buttons) {
        ControllerActionSetHandle_t action_handle_num = handle_num;
        ++handle_num;

        action_handles[set_name] = action_handle_num;
        for (auto & config_key : buttons) {
            uint64 current_handle_num = handle_num;
            ++handle_num;

            for (auto & button_string : config_key.second.first) {
                auto digital = button_strings.find(button_string);
                if (digital != button_strings.end()) {
                    ControllerDigitalActionHandle_t digital_handle_num = current_handle_num;

                    if (digital_action_handles.find(config_key.first) == digital_action_handles.end()) {
                        digital_action_handles[config_key.first] = digital_handle_num;
                    } else {
                        digital_handle_num = digital_action_handles[config_key.first];
                    }

                    controller_maps[action_handle_num].active_digital[digital_handle_num].insert(digital->second);
                } else {
                    auto analog = analog_strings.find(button_string);
                    if (analog != analog_strings.end()) {
                        ControllerAnalogActionHandle_t analog_handle_num = current_handle_num;

                        enum EInputSourceMode source_mode;
                        if (analog->second == TRIGGER_LEFT || analog->second == TRIGGER_RIGHT) {
                            source_mode = k_EInputSourceMode_Trigger;
                        } else {
                            source_mode = k_EInputSourceMode_JoystickMove;
                        }

                        auto input_mode = analog_input_modes.find(config_key.second.second);
                        if (input_mode != analog_input_modes.end()) {
                            source_mode = input_mode->second;
                        }

                        if (analog_action_handles.find(config_key.first) == analog_action_handles.end()) {
                            analog_action_handles[config_key.first] = analog_handle_num;
                        } else {
                            analog_handle_num = analog_action_handles[config_key.first];
                        }

                        controller_maps[action_handle_num].active_analog[analog_handle_num].first.insert(analog->second);
                        controller_maps[action_handle_num].active_analog[analog_handle_num].second = source_mode;

                    } else {
                        PRINT_DEBUG("Did not recognize controller button %s", button_string.c_str());
                        continue;
                    }
                }
            }
        }
    };

    for (auto &set : settings->controller_settings.action_sets) {
        add_action_set(set.first, set.second);
    }

    for (auto &set : settings->controller_settings.action_set_layers) {
        add_action_set(set.first, set.second);
        auto parent = settings->controller_settings.action_set_layer_parents.find(set.first);
        if (parent == settings->controller_settings.action_set_layer_parents.end()) continue;

        auto parent_handle = action_handles.find(parent->second);
        if (parent_handle == action_handles.end()) continue;
        action_set_layer_parents[action_handles[set.first]] = parent_handle->second;
    }
}

ControllerActionSetHandle_t Steam_Controller::get_default_action_set_handle() const
{
    auto default_action_handle = action_handles.find(default_action_set_name);
    if (default_action_handle != action_handles.end() && !action_set_layer_parents.count(default_action_handle->second)) {
        return default_action_handle->second;
    }

    for (auto &action_handle : action_handles) {
        if (!action_set_layer_parents.count(action_handle.second)) {
            return action_handle.second;
        }
    }

    return 0;
}

std::string Steam_Controller::get_action_set_name_for_handle(ControllerActionSetHandle_t handle) const
{
    for (auto &action_handle : action_handles) {
        if (action_handle.second == handle) {
            return action_handle.first;
        }
    }

    return {};
}

void Steam_Controller::activate_action_layer_name(Controller_Action &controller, const std::string &layer_name)
{
    auto layer_handle = action_handles.find(layer_name);
    if (layer_handle == action_handles.end()) return;
    controller.activate_action_set_layer(layer_handle->second, controller_maps, action_set_layer_parents);
}

void Steam_Controller::refresh_controllers(
    const std::map<ControllerHandle_t, std::string> &previous_action_sets,
    const std::map<ControllerHandle_t, std::vector<std::string>> &previous_action_layers
)
{
    const ControllerActionSetHandle_t default_action_set = get_default_action_set_handle();
    const std::string default_action_set_name = get_action_set_name_for_handle(default_action_set);
    for (auto &controller : controllers) {
        controller.second.deactivate_all_action_set_layers(controller_maps, action_set_layer_parents);

        ControllerActionSetHandle_t action_set_to_activate = default_action_set;
        std::string previous_action_set_name{};
        auto previous_action_set = previous_action_sets.find(controller.first);
        if (previous_action_set != previous_action_sets.end() && !previous_action_set->second.empty()) {
            previous_action_set_name = previous_action_set->second;
            auto current_action_set = action_handles.find(previous_action_set->second);
            if (current_action_set != action_handles.end() && !action_set_layer_parents.count(current_action_set->second)) {
                action_set_to_activate = current_action_set->second;
            }
        }

        PRINT_DEBUG(
            "refresh_controllers controller %llu previous_set='%s' default_set='%s' selected_set='%s' selected_handle=%llu",
            static_cast<unsigned long long>(controller.first),
            previous_action_set_name.c_str(),
            default_action_set_name.c_str(),
            get_action_set_name_for_handle(action_set_to_activate).c_str(),
            static_cast<unsigned long long>(action_set_to_activate)
        );

        controller.second.activate_action_set(action_set_to_activate, controller_maps, action_set_layer_parents);

        auto previous_layers = previous_action_layers.find(controller.first);
        if (previous_layers != previous_action_layers.end()) {
            for (auto &layer_name : previous_layers->second) {
                activate_action_layer_name(controller.second, layer_name);
            }
        }

        for (auto &layer_name : global_active_layer_names) {
            auto layer_handle = action_handles.find(layer_name);
            if (layer_handle == action_handles.end()) continue;
            auto parent = action_set_layer_parents.find(layer_handle->second);
            if (parent == action_set_layer_parents.end() || parent->second != controller.second.active_set) continue;
            controller.second.activate_action_set_layer(layer_handle->second, controller_maps, action_set_layer_parents);
        }
    }
}


void Steam_Controller::background_rumble(Rumble_Thread_Data *data)
{
    while (true) {
        unsigned short left, right;
        unsigned int rumble_length_ms;
        int gamepad = -1;
        while (gamepad == -1) {
            std::unique_lock<std::mutex> lck(data->rumble_mutex);
            if (data->kill_rumble_thread) {
                return;
            }

            data->rumble_thread_cv.wait_for(lck, std::chrono::milliseconds(1000));
            if (data->kill_rumble_thread) {
                return;
            }

            for (int i = 0; i < GAMEPAD_COUNT; ++i) {
                if (data->data[i].new_data) {
                    left = data->data[i].left;
                    right = data->data[i].right;
                    rumble_length_ms = data->data[i].rumble_length_ms;
                    data->data[i].new_data = false;
                    if (data->data[i].last_left != left || data->data[i].last_right != right) {
                        gamepad = i;
                        data->data[i].last_left = left;
                        data->data[i].last_right = right;
                        break;
                    }
                }
            }
        }

        GamepadSetRumble((GAMEPAD_DEVICE)gamepad, ((float)left) / 65535.0f, ((float)right) / 65535.0f, rumble_length_ms);
    }
}

void Steam_Controller::steam_run_every_runcb(void *object)
{
    // PRINT_DEBUG_ENTRY();

    Steam_Controller *steam_controller = (Steam_Controller *)object;
    steam_controller->RunCallbacks();
}

Steam_Controller::Steam_Controller(class Settings *settings, class SteamCallResults *callback_results, class SteamCallBacks *callbacks, class RunEveryRunCB *run_every_runcb)
{
    this->settings = settings;
    this->callback_results = callback_results;
    this->callbacks = callbacks;
    this->run_every_runcb = run_every_runcb;

    set_handles();
    disabled = !settings->controller_settings.enabled && action_handles.empty();
    initialized = false;
    
    this->run_every_runcb->add(&Steam_Controller::steam_run_every_runcb, this);
}

Steam_Controller::~Steam_Controller()
{
    //TODO rm network callbacks
    //TODO rumble thread
    Shutdown();
    this->run_every_runcb->remove(&Steam_Controller::steam_run_every_runcb, this);
}

// Init and Shutdown must be called when starting/ending use of this interface
bool Steam_Controller::Init(bool bExplicitlyCallRunFrame)
{
    PRINT_DEBUG("%u", bExplicitlyCallRunFrame);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    pending_explicitly_call_run_frame = bExplicitlyCallRunFrame;
    if (initialized) {
        return true;
    }

    if (disabled) {
        return true;
    }

    GamepadInit();
    GamepadUpdate();

    std::map<ControllerHandle_t, std::string> previous_action_sets{};
    std::map<ControllerHandle_t, std::vector<std::string>> previous_action_layers{};
    for (auto &controller : controllers) {
        previous_action_sets[controller.first] = get_action_set_name_for_handle(controller.second.active_set);
        auto persisted_layers = controller_active_layer_names.find(controller.first);
        if (persisted_layers != controller_active_layer_names.end()) {
            previous_action_layers[controller.first].assign(persisted_layers->second.begin(), persisted_layers->second.end());
        }
    }

    controllers = std::map<ControllerHandle_t, struct Controller_Action>();
    for (int i = 1; i < 5; ++i) {
        struct Controller_Action cont_action(i);
        controllers.insert(std::pair<ControllerHandle_t, struct Controller_Action>(i, cont_action));
    }

    refresh_controllers(previous_action_sets, previous_action_layers);

    rumble_thread_data = new Rumble_Thread_Data();
    background_rumble_thread = std::thread(background_rumble, rumble_thread_data);

    initialized = true;
    explicitly_call_run_frame = pending_explicitly_call_run_frame;
    return true;
}

bool Steam_Controller::Init( const char *pchAbsolutePathToControllerConfigVDF )
{
    PRINT_DEBUG("%s", pchAbsolutePathToControllerConfigVDF ? pchAbsolutePathToControllerConfigVDF : "(null)");
    if (pchAbsolutePathToControllerConfigVDF) {
        std::lock_guard<std::recursive_mutex> lock(global_mutex);

        std::map<ControllerHandle_t, std::string> previous_action_sets{};
        std::map<ControllerHandle_t, std::vector<std::string>> previous_action_layers{};
        for (auto &controller : controllers) {
            previous_action_sets[controller.first] = get_action_set_name_for_handle(controller.second.active_set);
            auto persisted_layers = controller_active_layer_names.find(controller.first);
            if (persisted_layers != controller_active_layer_names.end()) {
                previous_action_layers[controller.first].assign(persisted_layers->second.begin(), persisted_layers->second.end());
            }
        }

        Action_Set_Map action_sets =
            settings->controller_settings.action_sets;
        std::map<std::string, std::string> action_set_layer_parents =
            settings->controller_settings.action_set_layer_parents;
        Action_Set_Map action_set_layers =
            settings->controller_settings.action_set_layers;
        std::string default_action_set_name{};
        if (load_controller_mappings_vdf_file(std::filesystem::u8path(pchAbsolutePathToControllerConfigVDF), action_sets, action_set_layer_parents, action_set_layers, default_action_set_name)) {
            settings->controller_settings.action_sets = std::move(action_sets);
            settings->controller_settings.action_set_layer_parents = std::move(action_set_layer_parents);
            settings->controller_settings.action_set_layers = std::move(action_set_layers);
            this->default_action_set_name = std::move(default_action_set_name);
            set_handles();
            if (this->default_action_set_name.empty()) {
                this->default_action_set_name = get_action_set_name_for_handle(get_default_action_set_handle());
            }
            disabled = !settings->controller_settings.enabled && action_handles.empty();
            refresh_controllers(previous_action_sets, previous_action_layers);
        }
    }

    return Init();
}

bool Steam_Controller::Init()
{
    return Init(true);
}

bool Steam_Controller::Shutdown()
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (disabled || !initialized) {
        return true;
    }

    controllers = std::map<ControllerHandle_t, struct Controller_Action>();
    rumble_thread_data->rumble_mutex.lock();
    rumble_thread_data->kill_rumble_thread = true;
    rumble_thread_data->rumble_mutex.unlock();
    rumble_thread_data->rumble_thread_cv.notify_one();
    background_rumble_thread.join();
    delete rumble_thread_data;
    rumble_thread_data = nullptr;
    GamepadShutdown();
    initialized = false;
    return true;
}

void Steam_Controller::SetOverrideMode( const char *pchMode )
{
    PRINT_DEBUG_TODO();
}

// Set the absolute path to the Input Action Manifest file containing the in-game actions
// and file paths to the official configurations. Used in games that bundle Steam Input
// configurations inside of the game depot instead of using the Steam Workshop
bool Steam_Controller::SetInputActionManifestFilePath( const char *pchInputActionManifestAbsolutePath )
{
    PRINT_DEBUG("%s", pchInputActionManifestAbsolutePath ? pchInputActionManifestAbsolutePath : "(null)");
    if (!pchInputActionManifestAbsolutePath || !pchInputActionManifestAbsolutePath[0]) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    std::map<ControllerHandle_t, std::string> previous_action_sets{};
    std::map<ControllerHandle_t, std::vector<std::string>> previous_action_layers{};
    for (auto &controller : controllers) {
        previous_action_sets[controller.first] = get_action_set_name_for_handle(controller.second.active_set);
        auto persisted_layers = controller_active_layer_names.find(controller.first);
        if (persisted_layers != controller_active_layer_names.end()) {
            previous_action_layers[controller.first].assign(persisted_layers->second.begin(), persisted_layers->second.end());
        }
    }

    Action_Set_Map action_sets =
        settings->controller_settings.action_sets;
    std::map<std::string, std::string> action_set_layer_parents =
        settings->controller_settings.action_set_layer_parents;
    Action_Set_Map action_set_layers =
        settings->controller_settings.action_set_layers;
    std::string default_action_set_name{};
    if (!load_action_manifest_vdf(std::filesystem::u8path(pchInputActionManifestAbsolutePath), action_sets, action_set_layer_parents, action_set_layers, default_action_set_name)) {
        return false;
    }

    settings->controller_settings.action_sets = std::move(action_sets);
    settings->controller_settings.action_set_layer_parents = std::move(action_set_layer_parents);
    settings->controller_settings.action_set_layers = std::move(action_set_layers);
    this->default_action_set_name = std::move(default_action_set_name);
    set_handles();
    if (this->default_action_set_name.empty()) {
        this->default_action_set_name = get_action_set_name_for_handle(get_default_action_set_handle());
    }
    disabled = !settings->controller_settings.enabled && action_handles.empty();
    refresh_controllers(previous_action_sets, previous_action_layers);

    if (!disabled && !initialized) {
        return Init(pending_explicitly_call_run_frame);
    }

    return !action_handles.empty();
}

bool Steam_Controller::BWaitForData( bool bWaitForever, uint32 unTimeout )
{
    PRINT_DEBUG_TODO();
    //TODO SteamInput005
    return false;
}

// Returns true if new data has been received since the last time action data was accessed
// via GetDigitalActionData or GetAnalogActionData. The game will still need to call
// SteamInput()->RunFrame() or SteamAPI_RunCallbacks() before this to update the data stream
bool Steam_Controller::BNewDataAvailable()
{
    PRINT_DEBUG_TODO();
    //TODO SteamInput005
    return false;
}

// Enable SteamInputDeviceConnected_t and SteamInputDeviceDisconnected_t callbacks.
// Each controller that is already connected will generate a device connected
// callback when you enable them
void Steam_Controller::EnableDeviceCallbacks()
{
    PRINT_DEBUG_TODO();
    //TODO SteamInput005
    return;
}

// Enable SteamInputActionEvent_t callbacks. Directly calls your callback function
// for lower latency than standard Steam callbacks. Supports one callback at a time.
// Note: this is called within either SteamInput()->RunFrame or by SteamAPI_RunCallbacks
void Steam_Controller::EnableActionEventCallbacks( SteamInputActionEventCallbackPointer pCallback )
{
    PRINT_DEBUG_TODO();
    //TODO SteamInput005
    return;
}

// Synchronize API state with the latest Steam Controller inputs available. This
// is performed automatically by SteamAPI_RunCallbacks, but for the absolute lowest
// possible latency, you call this directly before reading controller state.
void Steam_Controller::RunFrame(bool bReservedValue)
{
    if (disabled || !initialized) {
        return;
    }
    PRINT_DEBUG_ENTRY();

    GamepadUpdate();
}

void Steam_Controller::RunFrame()
{
    RunFrame(true);
}

bool Steam_Controller::GetControllerState( uint32 unControllerIndex, SteamControllerState001_t *pState )
{
    PRINT_DEBUG_TODO();
    return false;
}

// Enumerate currently connected controllers
// handlesOut should point to a STEAM_CONTROLLER_MAX_COUNT sized array of ControllerHandle_t handles
// Returns the number of handles written to handlesOut
int Steam_Controller::GetConnectedControllers( InputHandle_t *handlesOut )
{
    return GetConnectedControllers(reinterpret_cast<ControllerHandle_t *>(handlesOut));
}

int Steam_Controller::GetConnectedControllers( ControllerHandle_t *handlesOut )
{
    PRINT_DEBUG_ENTRY();
    if (!handlesOut) return 0;
    if (disabled) {
        return 0;
    }

    int count = 0;
    if (GamepadIsConnected(GAMEPAD_0)) {
        *handlesOut = GAMEPAD_0 + 1;
        debug_log_raw_gamepad_state("GetConnectedControllers", *handlesOut);
        ++handlesOut;
        ++count;
    }
    if (GamepadIsConnected(GAMEPAD_1)) {
        *handlesOut = GAMEPAD_1 + 1;
        debug_log_raw_gamepad_state("GetConnectedControllers", *handlesOut);
        ++handlesOut;
        ++count;
    }
    if (GamepadIsConnected(GAMEPAD_2)) {
        *handlesOut = GAMEPAD_2 + 1;
        debug_log_raw_gamepad_state("GetConnectedControllers", *handlesOut);
        ++handlesOut;
        ++count;
    }
    if (GamepadIsConnected(GAMEPAD_3)) {
        *handlesOut = GAMEPAD_3 + 1;
        debug_log_raw_gamepad_state("GetConnectedControllers", *handlesOut);
        ++handlesOut;
        ++count;
    }

    PRINT_DEBUG("returned %i connected controllers", count);
    return count;
}


// Invokes the Steam overlay and brings up the binding screen
// Returns false is overlay is disabled / unavailable, or the user is not in Big Picture mode
bool Steam_Controller::ShowBindingPanel( ControllerHandle_t controllerHandle )
{
    PRINT_DEBUG_TODO();
    return false;
}


// ACTION SETS
// Lookup the handle for an Action Set. Best to do this once on startup, and store the handles for all future API calls.
ControllerActionSetHandle_t Steam_Controller::GetActionSetHandle( const char *pszActionSetName )
{
    PRINT_DEBUG("%s", pszActionSetName);
    if (!pszActionSetName) return 0;
    std::string upper_action_name(pszActionSetName);
    std::transform(upper_action_name.begin(), upper_action_name.end(), upper_action_name.begin(),[](unsigned char c){ return std::toupper(c); });

    auto set_handle = action_handles.find(upper_action_name);
    if (set_handle == action_handles.end()) return 0;

    PRINT_DEBUG("%s ret %llu", pszActionSetName, set_handle->second);
    return set_handle->second;
}


// Reconfigure the controller to use the specified action set (ie 'Menu', 'Walk' or 'Drive')
// This is cheap, and can be safely called repeatedly. It's often easier to repeatedly call it in
// your state loops, instead of trying to place it in all of your state transitions.
void Steam_Controller::ActivateActionSet( ControllerHandle_t controllerHandle, ControllerActionSetHandle_t actionSetHandle )
{
    PRINT_DEBUG(
        "%llu %llu '%s'",
        static_cast<unsigned long long>(controllerHandle),
        static_cast<unsigned long long>(actionSetHandle),
        get_action_set_name_for_handle(actionSetHandle).c_str()
    );
    if (controllerHandle == STEAM_CONTROLLER_HANDLE_ALL_CONTROLLERS) {
        for (auto & c: controllers) {
            controller_active_layer_names[c.first].clear();
            c.second.activate_action_set(actionSetHandle, controller_maps, action_set_layer_parents);
            PRINT_DEBUG("controller %llu active_set now '%s'", static_cast<unsigned long long>(c.first), get_action_set_name_for_handle(c.second.active_set).c_str());
            for (auto &layer_name : global_active_layer_names) {
                auto layer_handle = action_handles.find(layer_name);
                if (layer_handle == action_handles.end()) continue;
                auto parent = action_set_layer_parents.find(layer_handle->second);
                if (parent == action_set_layer_parents.end() || parent->second != c.second.active_set) continue;
                c.second.activate_action_set_layer(layer_handle->second, controller_maps, action_set_layer_parents);
            }
        }
        return;
    }

    auto controller = controllers.find(controllerHandle);
    if (controller == controllers.end()) return;

    controller_active_layer_names[controllerHandle].clear();
    controller->second.activate_action_set(actionSetHandle, controller_maps, action_set_layer_parents);
    PRINT_DEBUG("controller %llu active_set now '%s'", static_cast<unsigned long long>(controllerHandle), get_action_set_name_for_handle(controller->second.active_set).c_str());
    for (auto &layer_name : global_active_layer_names) {
        auto layer_handle = action_handles.find(layer_name);
        if (layer_handle == action_handles.end()) continue;
        auto parent = action_set_layer_parents.find(layer_handle->second);
        if (parent == action_set_layer_parents.end() || parent->second != controller->second.active_set) continue;
        controller->second.activate_action_set_layer(layer_handle->second, controller_maps, action_set_layer_parents);
    }
}

ControllerActionSetHandle_t Steam_Controller::GetCurrentActionSet( ControllerHandle_t controllerHandle )
{
    //TODO: should return zero if no action set specifically activated with ActivateActionSet
    PRINT_DEBUG("%llu", controllerHandle);
    auto controller = controllers.find(controllerHandle);
    if (controller == controllers.end()) return 0;

    return controller->second.active_set;
}


void Steam_Controller::ActivateActionSetLayer( ControllerHandle_t controllerHandle, ControllerActionSetHandle_t actionSetLayerHandle )
{
    PRINT_DEBUG("%llu %llu", controllerHandle, actionSetLayerHandle);
    const std::string layer_name = get_action_set_name_for_handle(actionSetLayerHandle);
    if (controllerHandle == STEAM_CONTROLLER_HANDLE_ALL_CONTROLLERS) {
        auto parent = action_set_layer_parents.find(actionSetLayerHandle);
        bool activated_any{};
        for (auto &c : controllers) {
            if (parent != action_set_layer_parents.end() && c.second.active_set == parent->second) {
                activated_any = true;
            }
            c.second.activate_action_set_layer(actionSetLayerHandle, controller_maps, action_set_layer_parents);
        }
        if (activated_any && !layer_name.empty()) {
            global_active_layer_names.insert(layer_name);
        }
        return;
    }

    auto controller = controllers.find(controllerHandle);
    if (controller == controllers.end()) return;
    auto parent = action_set_layer_parents.find(actionSetLayerHandle);
    if (parent == action_set_layer_parents.end() || parent->second != controller->second.active_set) return;
    if (!layer_name.empty()) {
        controller_active_layer_names[controllerHandle].insert(layer_name);
    }
    controller->second.activate_action_set_layer(actionSetLayerHandle, controller_maps, action_set_layer_parents);
}

void Steam_Controller::DeactivateActionSetLayer( ControllerHandle_t controllerHandle, ControllerActionSetHandle_t actionSetLayerHandle )
{
    PRINT_DEBUG("%llu %llu", controllerHandle, actionSetLayerHandle);
    const std::string layer_name = get_action_set_name_for_handle(actionSetLayerHandle);
    if (controllerHandle == STEAM_CONTROLLER_HANDLE_ALL_CONTROLLERS) {
        if (!layer_name.empty()) {
            global_active_layer_names.erase(layer_name);
            for (auto &persisted_layers : controller_active_layer_names) {
                persisted_layers.second.erase(layer_name);
            }
        }
        for (auto &c : controllers) {
            c.second.deactivate_action_set_layer(actionSetLayerHandle, controller_maps, action_set_layer_parents);
        }
        return;
    }

    auto controller = controllers.find(controllerHandle);
    if (controller == controllers.end()) return;
    if (!layer_name.empty()) {
        controller_active_layer_names[controllerHandle].erase(layer_name);
    }
    controller->second.deactivate_action_set_layer(actionSetLayerHandle, controller_maps, action_set_layer_parents);
}

void Steam_Controller::DeactivateAllActionSetLayers( ControllerHandle_t controllerHandle )
{
    PRINT_DEBUG("%llu", controllerHandle);
    if (controllerHandle == STEAM_CONTROLLER_HANDLE_ALL_CONTROLLERS) {
        global_active_layer_names.clear();
        controller_active_layer_names.clear();
        for (auto &c : controllers) {
            c.second.deactivate_all_action_set_layers(controller_maps, action_set_layer_parents);
        }
        return;
    }

    auto controller = controllers.find(controllerHandle);
    if (controller == controllers.end()) return;
    controller_active_layer_names[controllerHandle].clear();
    controller->second.deactivate_all_action_set_layers(controller_maps, action_set_layer_parents);
}

int Steam_Controller::GetActiveActionSetLayers( ControllerHandle_t controllerHandle, ControllerActionSetHandle_t *handlesOut )
{
    PRINT_DEBUG("%llu", controllerHandle);
    if (controllerHandle == STEAM_CONTROLLER_HANDLE_ALL_CONTROLLERS) return 0;

    auto controller = controllers.find(controllerHandle);
    if (controller == controllers.end()) return 0;

    int count = 0;
    for (auto &active_layer : controller->second.active_layers) {
        if (count >= STEAM_INPUT_MAX_ACTIVE_LAYERS) break;
        if (handlesOut) {
            *handlesOut = active_layer;
            ++handlesOut;
        }
        ++count;
    }

    return count;
}



// ACTIONS
// Lookup the handle for a digital action. Best to do this once on startup, and store the handles for all future API calls.
ControllerDigitalActionHandle_t Steam_Controller::GetDigitalActionHandle( const char *pszActionName )
{
    PRINT_DEBUG("%s", pszActionName);
    if (!pszActionName) return 0;
    std::string upper_action_name(pszActionName);
    std::transform(upper_action_name.begin(), upper_action_name.end(), upper_action_name.begin(),[](unsigned char c){ return std::toupper(c); });

    auto handle = digital_action_handles.find(upper_action_name);
    if (handle == digital_action_handles.end()) {
        //apparently GetDigitalActionHandle also works with analog handles
        handle = analog_action_handles.find(upper_action_name);
        if (handle == analog_action_handles.end()) return 0;
    }

    PRINT_DEBUG("%s ret %llu", pszActionName, handle->second);
    return handle->second;
}


// Returns the current state of the supplied digital game action
ControllerDigitalActionData_t Steam_Controller::GetDigitalActionData( ControllerHandle_t controllerHandle, ControllerDigitalActionHandle_t digitalActionHandle )
{
    PRINT_DEBUG("%llu %llu", controllerHandle, digitalActionHandle);
    ControllerDigitalActionData_t digitalData;
    digitalData.bActive = false;
    digitalData.bState = false;

    auto controller = controllers.find(controllerHandle);
    if (controller == controllers.end()) {
        debug_log_action_snapshot(
            "GetDigitalActionData",
            controllerHandle,
            digitalActionHandle,
            "controller " + std::to_string(controllerHandle) +
                " missing while reading digital handle " + std::to_string(digitalActionHandle)
        );
        return digitalData;
    }

    std::set<int> buttons = controller->second.button_id(digitalActionHandle);
    if (!buttons.size()) {
        debug_log_action_snapshot(
            "GetDigitalActionData",
            controllerHandle,
            digitalActionHandle,
            "digital handle " + std::to_string(digitalActionHandle) +
                " not mapped in active set '" + get_action_set_name_for_handle(controller->second.active_set) +
                "' on controller " + std::to_string(controllerHandle)
        );
        return digitalData;
    }
    digitalData.bActive = true;

    GAMEPAD_DEVICE device = (GAMEPAD_DEVICE)(controllerHandle - 1);
    debug_log_raw_gamepad_state("GetDigitalActionData", controllerHandle);

    for (auto button : buttons) {
        bool pressed = false;
        if (button < BUTTON_COUNT) {
            pressed = GamepadButtonDown(device, (GAMEPAD_BUTTON)button);
        } else {
            switch (button) {
                case BUTTON_LTRIGGER:
                    pressed = GamepadTriggerLength(device, TRIGGER_LEFT) > 0.8;
                    break;
                case BUTTON_RTRIGGER:
                    pressed = GamepadTriggerLength(device, TRIGGER_RIGHT) > 0.8;
                    break;
                case BUTTON_STICK_LEFT_UP:
                case BUTTON_STICK_LEFT_DOWN:
                case BUTTON_STICK_LEFT_LEFT:
                case BUTTON_STICK_LEFT_RIGHT: {
                    float x = 0, y = 0, len = GamepadStickLength(device, STICK_LEFT);
                    GamepadStickNormXY(device, STICK_LEFT, &x, &y);
                    x *= len;
                    y *= len;
                    if (button == BUTTON_STICK_LEFT_UP) pressed = y > DEADZONE_BUTTON_STICK;
                    if (button == BUTTON_STICK_LEFT_DOWN) pressed = y < -DEADZONE_BUTTON_STICK;
                    if (button == BUTTON_STICK_LEFT_RIGHT) pressed = x > DEADZONE_BUTTON_STICK;
                    if (button == BUTTON_STICK_LEFT_LEFT) pressed = x < -DEADZONE_BUTTON_STICK;
                    break;
                }
                case BUTTON_STICK_RIGHT_UP:
                case BUTTON_STICK_RIGHT_DOWN:
                case BUTTON_STICK_RIGHT_LEFT:
                case BUTTON_STICK_RIGHT_RIGHT: {
                    float x = 0, y = 0, len = GamepadStickLength(device, STICK_RIGHT);
                    GamepadStickNormXY(device, STICK_RIGHT, &x, &y);
                    x *= len;
                    y *= len;
                    if (button == BUTTON_STICK_RIGHT_UP) pressed = y > DEADZONE_BUTTON_STICK;
                    if (button == BUTTON_STICK_RIGHT_DOWN) pressed = y < -DEADZONE_BUTTON_STICK;
                    if (button == BUTTON_STICK_RIGHT_RIGHT) pressed = x > DEADZONE_BUTTON_STICK;
                    if (button == BUTTON_STICK_RIGHT_LEFT) pressed = x < -DEADZONE_BUTTON_STICK;
                    break;
                }
                default:
                    break;
            }
        }

        if (pressed) {
            digitalData.bState = true;
            break;
        }
    }

    debug_log_action_snapshot(
        "GetDigitalActionData",
        controllerHandle,
        digitalActionHandle,
        "digital handle " + std::to_string(digitalActionHandle) +
            " controller " + std::to_string(controllerHandle) +
            " active_set '" + get_action_set_name_for_handle(controller->second.active_set) +
            "' bindings=[" + debug_describe_binding_set(buttons) +
            "] active=" + std::to_string(digitalData.bActive) +
            " state=" + std::to_string(digitalData.bState)
    );
    return digitalData;
}


// Get the origin(s) for a digital action within an action set. Returns the number of origins supplied in originsOut. Use this to display the appropriate on-screen prompt for the action.
// originsOut should point to a STEAM_CONTROLLER_MAX_ORIGINS sized array of EControllerActionOrigin handles
int Steam_Controller::GetDigitalActionOrigins( ControllerHandle_t controllerHandle, ControllerActionSetHandle_t actionSetHandle, ControllerDigitalActionHandle_t digitalActionHandle, EControllerActionOrigin *originsOut )
{
    PRINT_DEBUG_ENTRY();
    EInputActionOrigin origins[STEAM_CONTROLLER_MAX_ORIGINS];
    int ret = GetDigitalActionOrigins(controllerHandle, actionSetHandle, digitalActionHandle, origins );
    for (int i = 0; i < ret; ++i) {
        originsOut[i] = (EControllerActionOrigin)(origins[i] - ((long)k_EInputActionOrigin_XBox360_A - (long)k_EControllerActionOrigin_XBox360_A));
    }

    return ret;
}

int Steam_Controller::GetDigitalActionOrigins( InputHandle_t inputHandle, InputActionSetHandle_t actionSetHandle, InputDigitalActionHandle_t digitalActionHandle, EInputActionOrigin *originsOut )
{
    PRINT_DEBUG_ENTRY();
    auto controller = controllers.find(inputHandle);
    if (controller == controllers.end()) return 0;

    auto map = controller_maps.find(actionSetHandle);
    if (map == controller_maps.end()) return 0;

    auto a = map->second.active_digital.find(digitalActionHandle);
    if (a == map->second.active_digital.end()) return 0;

    int count = 0;
    for (auto button: a->second) {
        switch (button) {
            case BUTTON_A:
                originsOut[count] = k_EInputActionOrigin_XBox360_A;
                break;
            case BUTTON_B:
                originsOut[count] = k_EInputActionOrigin_XBox360_B;
                break;
            case BUTTON_X:
                originsOut[count] = k_EInputActionOrigin_XBox360_X;
                break;
            case BUTTON_Y:
                originsOut[count] = k_EInputActionOrigin_XBox360_Y;
                break;
            case BUTTON_LEFT_SHOULDER:
                originsOut[count] = k_EInputActionOrigin_XBox360_LeftBumper;
                break;
            case BUTTON_RIGHT_SHOULDER:
                originsOut[count] = k_EInputActionOrigin_XBox360_RightBumper;
                break;
            case BUTTON_START:
                originsOut[count] = k_EInputActionOrigin_XBox360_Start;
                break;
            case BUTTON_BACK:
                originsOut[count] = k_EInputActionOrigin_XBox360_Back;
                break;
            case BUTTON_LTRIGGER:
                originsOut[count] = k_EInputActionOrigin_XBox360_LeftTrigger_Click;
                break;
            case BUTTON_RTRIGGER:
                originsOut[count] = k_EInputActionOrigin_XBox360_RightTrigger_Click;
                break;
            case BUTTON_LEFT_THUMB:
                originsOut[count] = k_EInputActionOrigin_XBox360_LeftStick_Click;
                break;
            case BUTTON_RIGHT_THUMB:
                originsOut[count] = k_EInputActionOrigin_XBox360_RightStick_Click;
                break;

            case BUTTON_STICK_LEFT_UP:
                originsOut[count] = k_EInputActionOrigin_XBox360_LeftStick_DPadNorth;
                break;
            case BUTTON_STICK_LEFT_DOWN:
                originsOut[count] = k_EInputActionOrigin_XBox360_LeftStick_DPadSouth;
                break;
            case BUTTON_STICK_LEFT_LEFT:
                originsOut[count] = k_EInputActionOrigin_XBox360_LeftStick_DPadWest;
                break;
            case BUTTON_STICK_LEFT_RIGHT:
                originsOut[count] = k_EInputActionOrigin_XBox360_LeftStick_DPadEast;
                break;

            case BUTTON_STICK_RIGHT_UP:
                originsOut[count] = k_EInputActionOrigin_XBox360_RightStick_DPadNorth;
                break;
            case BUTTON_STICK_RIGHT_DOWN:
                originsOut[count] = k_EInputActionOrigin_XBox360_RightStick_DPadSouth;
                break;
            case BUTTON_STICK_RIGHT_LEFT:
                originsOut[count] = k_EInputActionOrigin_XBox360_RightStick_DPadWest;
                break;
            case BUTTON_STICK_RIGHT_RIGHT:
                originsOut[count] = k_EInputActionOrigin_XBox360_RightStick_DPadEast;
                break;

            case BUTTON_DPAD_UP:
                originsOut[count] = k_EInputActionOrigin_XBox360_DPad_North;
                break;
            case BUTTON_DPAD_DOWN:
                originsOut[count] = k_EInputActionOrigin_XBox360_DPad_South;
                break;
            case BUTTON_DPAD_LEFT:
                originsOut[count] = k_EInputActionOrigin_XBox360_DPad_West;
                break;
            case BUTTON_DPAD_RIGHT:
                originsOut[count] = k_EInputActionOrigin_XBox360_DPad_East;
                break;

            default:
                originsOut[count] = k_EInputActionOrigin_None;
                break;
        }

        ++count;
        if (count >= STEAM_INPUT_MAX_ORIGINS) {
            break;
        }
    }

    return count;
}

// Returns a localized string (from Steam's language setting) for the user-facing action name corresponding to the specified handle
const char* Steam_Controller::GetStringForDigitalActionName( InputDigitalActionHandle_t eActionHandle )
{
    PRINT_DEBUG_TODO();
    //TODO SteamInput005
    return "Button String";
}

// Lookup the handle for an analog action. Best to do this once on startup, and store the handles for all future API calls.
ControllerAnalogActionHandle_t Steam_Controller::GetAnalogActionHandle( const char *pszActionName )
{
    PRINT_DEBUG("%s", pszActionName);
    if (!pszActionName) return 0;
    std::string upper_action_name(pszActionName);
    std::transform(upper_action_name.begin(), upper_action_name.end(), upper_action_name.begin(),[](unsigned char c){ return std::toupper(c); });

    auto handle = analog_action_handles.find(upper_action_name);
    if (handle == analog_action_handles.end()) return 0;

    return handle->second;
}


// Returns the current state of these supplied analog game action
ControllerAnalogActionData_t Steam_Controller::GetAnalogActionData( ControllerHandle_t controllerHandle, ControllerAnalogActionHandle_t analogActionHandle )
{
    PRINT_DEBUG("%llu %llu", controllerHandle, analogActionHandle);
    GAMEPAD_DEVICE device = (GAMEPAD_DEVICE)(controllerHandle - 1);

    ControllerAnalogActionData_t data;
    data.eMode = k_EInputSourceMode_None;
    data.x = data.y = 0;
    data.bActive = false;

    auto controller = controllers.find(controllerHandle);
    if (controller == controllers.end()) {
        debug_log_action_snapshot(
            "GetAnalogActionData",
            controllerHandle,
            analogActionHandle,
            "controller " + std::to_string(controllerHandle) +
                " missing while reading analog handle " + std::to_string(analogActionHandle)
        );
        return data;
    }

    auto analog = controller->second.analog_id(analogActionHandle);
    if (!analog.first.size()) {
        debug_log_action_snapshot(
            "GetAnalogActionData",
            controllerHandle,
            analogActionHandle,
            "analog handle " + std::to_string(analogActionHandle) +
                " not mapped in active set '" + get_action_set_name_for_handle(controller->second.active_set) +
                "' on controller " + std::to_string(controllerHandle)
        );
        return data;
    }

    data.bActive = true;
    data.eMode = analog.second;
    debug_log_raw_gamepad_state("GetAnalogActionData", controllerHandle);

    for (auto a : analog.first) {
        if (a >= JOY_ID_START) {
            int joystick_id = a - JOY_ID_START;
            if (joystick_id == STICK_DPAD) {
                int mov_y = (int)GamepadButtonDown(device, BUTTON_DPAD_UP) - (int)GamepadButtonDown(device, BUTTON_DPAD_DOWN);
                int mov_x = (int)GamepadButtonDown(device, BUTTON_DPAD_RIGHT) - (int)GamepadButtonDown(device, BUTTON_DPAD_LEFT);
                if (mov_y || mov_x) {
                    data.x = static_cast<float>(mov_x);
                    data.y = static_cast<float>(mov_y);
                    float length = 1.0f / std::sqrt(data.x * data.x + data.y * data.y);
                    data.x = data.x * length;
                    data.y = data.y * length;
                }
            } else {
                GamepadStickNormXY(device, (GAMEPAD_STICK) joystick_id, &data.x, &data.y);
                float length = GamepadStickLength(device, (GAMEPAD_STICK) joystick_id);
                data.x = data.x * length;
                data.y = data.y * length;
            }
        } else {
            data.x = GamepadTriggerLength(device, (GAMEPAD_TRIGGER) a);
        }

        if (data.x || data.y) {
            break;
        }
    }

    char analog_snapshot[512];
    snprintf(
        analog_snapshot,
        sizeof(analog_snapshot),
        "analog handle %llu controller %llu active_set '%s' mode=%s bindings=[%s] active=%i x=%.3f y=%.3f",
        static_cast<unsigned long long>(analogActionHandle),
        static_cast<unsigned long long>(controllerHandle),
        get_action_set_name_for_handle(controller->second.active_set).c_str(),
        debug_input_source_mode_name(data.eMode),
        debug_describe_binding_set(analog.first).c_str(),
        data.bActive,
        data.x,
        data.y
    );
    debug_log_action_snapshot(
        "GetAnalogActionData",
        controllerHandle,
        analogActionHandle,
        analog_snapshot
    );
    return data;
}


// Get the origin(s) for an analog action within an action set. Returns the number of origins supplied in originsOut. Use this to display the appropriate on-screen prompt for the action.
// originsOut should point to a STEAM_CONTROLLER_MAX_ORIGINS sized array of EControllerActionOrigin handles
int Steam_Controller::GetAnalogActionOrigins( ControllerHandle_t controllerHandle, ControllerActionSetHandle_t actionSetHandle, ControllerAnalogActionHandle_t analogActionHandle, EControllerActionOrigin *originsOut )
{
    PRINT_DEBUG_ENTRY();
    EInputActionOrigin origins[STEAM_CONTROLLER_MAX_ORIGINS];
    int ret = GetAnalogActionOrigins(controllerHandle, actionSetHandle, analogActionHandle, origins );
    for (int i = 0; i < ret; ++i) {
        originsOut[i] = (EControllerActionOrigin)(origins[i] - ((long)k_EInputActionOrigin_XBox360_A - (long)k_EControllerActionOrigin_XBox360_A));
    }

    return ret;
}

int Steam_Controller::GetAnalogActionOrigins( InputHandle_t inputHandle, InputActionSetHandle_t actionSetHandle, InputAnalogActionHandle_t analogActionHandle, EInputActionOrigin *originsOut )
{
    PRINT_DEBUG_ENTRY();
    auto controller = controllers.find(inputHandle);
    if (controller == controllers.end()) return 0;

    auto map = controller_maps.find(actionSetHandle);
    if (map == controller_maps.end()) return 0;

    auto a = map->second.active_analog.find(analogActionHandle);
    if (a == map->second.active_analog.end()) return 0;

    int count = 0;
    for (auto b: a->second.first) {
        switch (b) {
            case TRIGGER_LEFT:
                originsOut[count] = k_EInputActionOrigin_XBox360_LeftTrigger_Pull;
                break;
            case TRIGGER_RIGHT:
                originsOut[count] = k_EInputActionOrigin_XBox360_RightTrigger_Pull;
                break;
            case STICK_LEFT + JOY_ID_START:
                originsOut[count] = k_EInputActionOrigin_XBox360_LeftStick_Move;
                break;
            case STICK_RIGHT + JOY_ID_START:
                originsOut[count] = k_EInputActionOrigin_XBox360_RightStick_Move;
                break;
            case STICK_DPAD + JOY_ID_START:
                originsOut[count] = k_EInputActionOrigin_XBox360_DPad_Move;
                break;
            default:
                originsOut[count] = k_EInputActionOrigin_None;
                break;
        }

        ++count;
        if (count >= STEAM_INPUT_MAX_ORIGINS) {
            break;
        }
    }

    return count;
}

    
void Steam_Controller::StopAnalogActionMomentum( ControllerHandle_t controllerHandle, ControllerAnalogActionHandle_t eAction )
{
    PRINT_DEBUG("%llu %llu", controllerHandle, eAction);
}


// Trigger a haptic pulse on a controller
void Steam_Controller::TriggerHapticPulse( ControllerHandle_t controllerHandle, ESteamControllerPad eTargetPad, unsigned short usDurationMicroSec )
{
    PRINT_DEBUG_TODO();
}

// Trigger a haptic pulse on a controller
void Steam_Controller::Legacy_TriggerHapticPulse( InputHandle_t inputHandle, ESteamControllerPad eTargetPad, unsigned short usDurationMicroSec )
{
    PRINT_DEBUG_TODO();
    TriggerHapticPulse(inputHandle, eTargetPad, usDurationMicroSec );
}

void Steam_Controller::TriggerHapticPulse( uint32 unControllerIndex, ESteamControllerPad eTargetPad, unsigned short usDurationMicroSec )
{
    PRINT_DEBUG("old");
    TriggerHapticPulse(unControllerIndex, eTargetPad, usDurationMicroSec );
}

// Trigger a pulse with a duty cycle of usDurationMicroSec / usOffMicroSec, unRepeat times.
// nFlags is currently unused and reserved for future use.
void Steam_Controller::TriggerRepeatedHapticPulse( ControllerHandle_t controllerHandle, ESteamControllerPad eTargetPad, unsigned short usDurationMicroSec, unsigned short usOffMicroSec, unsigned short unRepeat, unsigned int nFlags )
{
    PRINT_DEBUG_TODO();
}

void Steam_Controller::Legacy_TriggerRepeatedHapticPulse( InputHandle_t inputHandle, ESteamControllerPad eTargetPad, unsigned short usDurationMicroSec, unsigned short usOffMicroSec, unsigned short unRepeat, unsigned int nFlags )
{
    PRINT_DEBUG_TODO();
    TriggerRepeatedHapticPulse(inputHandle, eTargetPad, usDurationMicroSec, usOffMicroSec, unRepeat, nFlags);
}


// Send a haptic pulse, works on Steam Deck and Steam Controller devices
void Steam_Controller::TriggerSimpleHapticEvent( InputHandle_t inputHandle, EControllerHapticLocation eHapticLocation, uint8 nIntensity, char nGainDB, uint8 nOtherIntensity, char nOtherGainDB )
{
    PRINT_DEBUG_TODO();
}

// Tigger a vibration event on supported controllers.  
void Steam_Controller::TriggerVibration( ControllerHandle_t controllerHandle, unsigned short usLeftSpeed, unsigned short usRightSpeed )
{
    PRINT_DEBUG("%hu %hu", usLeftSpeed, usRightSpeed);
    auto controller = controllers.find(controllerHandle);
    if (controller == controllers.end()) return;

    unsigned int rumble_length_ms = 0;

#if defined(__linux__)
    //FIXME: shadow of the tomb raider on linux doesn't seem to turn off the rumble so I made it expire after 100ms. Need to check if this is how linux steam actually behaves.
    rumble_length_ms = 100;
#endif

    unsigned gamepad_device = static_cast<unsigned int>(controllerHandle - 1);
    if (gamepad_device > GAMEPAD_COUNT) return;
    rumble_thread_data->rumble_mutex.lock();
    rumble_thread_data->data[gamepad_device].new_data = true;
    rumble_thread_data->data[gamepad_device].left = usLeftSpeed;
    rumble_thread_data->data[gamepad_device].right = usRightSpeed;
    rumble_thread_data->data[gamepad_device].rumble_length_ms = rumble_length_ms;
    rumble_thread_data->rumble_mutex.unlock();
    rumble_thread_data->rumble_thread_cv.notify_one();
}

// Trigger a vibration event on supported controllers including Xbox trigger impulse rumble - Steam will translate these commands into haptic pulses for Steam Controllers
void Steam_Controller::TriggerVibrationExtended( InputHandle_t inputHandle, unsigned short usLeftSpeed, unsigned short usRightSpeed, unsigned short usLeftTriggerSpeed, unsigned short usRightTriggerSpeed )
{
    PRINT_DEBUG_TODO();
    TriggerVibration(inputHandle, usLeftSpeed, usRightSpeed);
    //TODO trigger impulse rumbles
}

// Set the controller LED color on supported controllers.  
void Steam_Controller::SetLEDColor( ControllerHandle_t controllerHandle, uint8 nColorR, uint8 nColorG, uint8 nColorB, unsigned int nFlags )
{
    PRINT_DEBUG_TODO();
}


// Returns the associated gamepad index for the specified controller, if emulating a gamepad
int Steam_Controller::GetGamepadIndexForController( ControllerHandle_t ulControllerHandle )
{
    PRINT_DEBUG_ENTRY();
    auto controller = controllers.find(ulControllerHandle);
    if (controller == controllers.end()) {
        PRINT_DEBUG("controller %llu has no gamepad index", static_cast<unsigned long long>(ulControllerHandle));
        return -1;
    }

    const int out = static_cast<int>(ulControllerHandle) - 1;
    PRINT_DEBUG("controller %llu -> gamepad index %i", static_cast<unsigned long long>(ulControllerHandle), out);
    return out;
}


// Returns the associated controller handle for the specified emulated gamepad
ControllerHandle_t Steam_Controller::GetControllerForGamepadIndex( int nIndex )
{
    PRINT_DEBUG("%i", nIndex);
    ControllerHandle_t out = nIndex + 1;
    auto controller = controllers.find(out);
    if (controller == controllers.end()) {
        PRINT_DEBUG("gamepad index %i has no controller handle", nIndex);
        return 0;
    }
    PRINT_DEBUG("gamepad index %i -> controller handle %llu", nIndex, static_cast<unsigned long long>(out));
    return out;
}


// Returns raw motion data from the specified controller
ControllerMotionData_t Steam_Controller::GetMotionData( ControllerHandle_t controllerHandle )
{
    PRINT_DEBUG_TODO();
    ControllerMotionData_t data = {};
    return data;
}


// Attempt to display origins of given action in the controller HUD, for the currently active action set
// Returns false is overlay is disabled / unavailable, or the user is not in Big Picture mode
bool Steam_Controller::ShowDigitalActionOrigins( ControllerHandle_t controllerHandle, ControllerDigitalActionHandle_t digitalActionHandle, float flScale, float flXPosition, float flYPosition )
{
    PRINT_DEBUG_TODO();
    return true;
}

bool Steam_Controller::ShowAnalogActionOrigins( ControllerHandle_t controllerHandle, ControllerAnalogActionHandle_t analogActionHandle, float flScale, float flXPosition, float flYPosition )
{
    PRINT_DEBUG_TODO();
    return true;
}


// Returns a localized string (from Steam's language setting) for the specified origin
const char* Steam_Controller::GetStringForActionOrigin( EControllerActionOrigin eOrigin )
{
    PRINT_DEBUG_TODO();
    return "Button String";
}

const char* Steam_Controller::GetStringForActionOrigin( EInputActionOrigin eOrigin )
{
    PRINT_DEBUG_TODO();
    return "Button String";
}

// Returns a localized string (from Steam's language setting) for the user-facing action name corresponding to the specified handle
const char* Steam_Controller::GetStringForAnalogActionName( InputAnalogActionHandle_t eActionHandle )
{
    PRINT_DEBUG_TODO();
    //TODO SteamInput005
    return "Button String";
}

// Get a local path to art for on-screen glyph for a particular origin 
const char* Steam_Controller::GetGlyphForActionOrigin( EControllerActionOrigin eOrigin )
{
    PRINT_DEBUG("%i", eOrigin);

    if (steamcontroller_glyphs.empty()) {
        std::string dir = settings->glyphs_directory;
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_A] = dir + "button_a.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_B] = dir + "button_b.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_X] = dir + "button_x.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_Y] = dir + "button_y.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_LeftBumper] = dir + "shoulder_l.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_RightBumper] = dir + "shoulder_r.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_Start] = dir + "xbox_button_start.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_Back] = dir + "xbox_button_select.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_LeftTrigger_Pull] = dir + "trigger_l_pull.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_LeftTrigger_Click] = dir + "trigger_l_click.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_RightTrigger_Pull] = dir + "trigger_r_pull.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_RightTrigger_Click] = dir + "trigger_r_click.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_LeftStick_Move] = dir + "stick_l_move.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_LeftStick_Click] = dir + "stick_l_click.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_LeftStick_DPadNorth] = dir + "stick_dpad_n.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_LeftStick_DPadSouth] = dir + "stick_dpad_s.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_LeftStick_DPadWest] = dir + "stick_dpad_w.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_LeftStick_DPadEast] = dir + "stick_dpad_e.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_RightStick_Move] = dir + "stick_r_move.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_RightStick_Click] = dir + "stick_r_click.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_RightStick_DPadNorth] = dir + "stick_dpad_n.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_RightStick_DPadSouth] = dir + "stick_dpad_s.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_RightStick_DPadWest] = dir + "stick_dpad_w.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_RightStick_DPadEast] = dir + "stick_dpad_e.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_DPad_North] = dir + "xbox_button_dpad_n.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_DPad_South] = dir + "xbox_button_dpad_s.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_DPad_West] = dir + "xbox_button_dpad_w.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_DPad_East] = dir + "xbox_button_dpad_e.png";
        steamcontroller_glyphs[k_EControllerActionOrigin_XBox360_DPad_Move] = dir + "xbox_button_dpad_move.png";
    }

    auto glyph = steamcontroller_glyphs.find(eOrigin);
    if (glyph == steamcontroller_glyphs.end()) return "";
    return glyph->second.c_str();
}

const char* Steam_Controller::GetGlyphForActionOrigin( EInputActionOrigin eOrigin )
{
    PRINT_DEBUG("steaminput %i", eOrigin);
    if (steaminput_glyphs.empty()) {
        std::string dir = settings->glyphs_directory;
        steaminput_glyphs[k_EInputActionOrigin_XBox360_A] = dir + "button_a.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_B] = dir + "button_b.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_X] = dir + "button_x.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_Y] = dir + "button_y.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_LeftBumper] = dir + "shoulder_l.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_RightBumper] = dir + "shoulder_r.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_Start] = dir + "xbox_button_start.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_Back] = dir + "xbox_button_select.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_LeftTrigger_Pull] = dir + "trigger_l_pull.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_LeftTrigger_Click] = dir + "trigger_l_click.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_RightTrigger_Pull] = dir + "trigger_r_pull.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_RightTrigger_Click] = dir + "trigger_r_click.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_LeftStick_Move] = dir + "stick_l_move.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_LeftStick_Click] = dir + "stick_l_click.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_LeftStick_DPadNorth] = dir + "stick_dpad_n.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_LeftStick_DPadSouth] = dir + "stick_dpad_s.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_LeftStick_DPadWest] = dir + "stick_dpad_w.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_LeftStick_DPadEast] = dir + "stick_dpad_e.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_RightStick_Move] = dir + "stick_r_move.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_RightStick_Click] = dir + "stick_r_click.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_RightStick_DPadNorth] = dir + "stick_dpad_n.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_RightStick_DPadSouth] = dir + "stick_dpad_s.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_RightStick_DPadWest] = dir + "stick_dpad_w.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_RightStick_DPadEast] = dir + "stick_dpad_e.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_DPad_North] = dir + "xbox_button_dpad_n.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_DPad_South] = dir + "xbox_button_dpad_s.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_DPad_West] = dir + "xbox_button_dpad_w.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_DPad_East] = dir + "xbox_button_dpad_e.png";
        steaminput_glyphs[k_EInputActionOrigin_XBox360_DPad_Move] = dir + "xbox_button_dpad_move.png";
        //steaminput_glyphs[] = dir + "";
    }

    auto glyph = steaminput_glyphs.find(eOrigin);
    if (glyph == steaminput_glyphs.end()) return "";
    return glyph->second.c_str();
}

// Get a local path to a PNG file for the provided origin's glyph. 
const char* Steam_Controller::GetGlyphPNGForActionOrigin( EInputActionOrigin eOrigin, ESteamInputGlyphSize eSize, uint32 unFlags )
{
    PRINT_DEBUG_TODO();
    //TODO SteamInput005
    return GetGlyphForActionOrigin(eOrigin);
}

// Get a local path to a SVG file for the provided origin's glyph. 
const char* Steam_Controller::GetGlyphSVGForActionOrigin( EInputActionOrigin eOrigin, uint32 unFlags )
{
    PRINT_DEBUG_TODO();
    //TODO SteamInput005
    return "";
}

// Get a local path to an older, Big Picture Mode-style PNG file for a particular origin
const char* Steam_Controller::GetGlyphForActionOrigin_Legacy( EInputActionOrigin eOrigin )
{
    PRINT_DEBUG_ENTRY();
    return GetGlyphForActionOrigin(eOrigin);
}

// Returns the input type for a particular handle
ESteamInputType Steam_Controller::GetInputTypeForHandle( ControllerHandle_t controllerHandle )
{
    PRINT_DEBUG("%llu", controllerHandle);
    auto controller = controllers.find(controllerHandle);
    if (controller == controllers.end()) return k_ESteamInputType_Unknown;
    
    // Playstation
    if (settings->controller_settings.controller_type_override == "PS3") return k_ESteamInputType_PS3Controller;
    if (settings->controller_settings.controller_type_override == "PS4") return k_ESteamInputType_PS4Controller;
    if (settings->controller_settings.controller_type_override == "PS5") return k_ESteamInputType_PS5Controller;
    // Xbox
    if (settings->controller_settings.controller_type_override == "XBOX360") return k_ESteamInputType_XBox360Controller;
    if (settings->controller_settings.controller_type_override == "XBOXONE") return k_ESteamInputType_XBoxOneController;
    // Nintendo
    if (settings->controller_settings.controller_type_override == "SWITCH") return k_ESteamInputType_SwitchProController;

    return k_ESteamInputType_XBox360Controller;
}

const char* Steam_Controller::GetStringForXboxOrigin( EXboxOrigin eOrigin )
{
    PRINT_DEBUG_TODO();
    return "";
}

const char* Steam_Controller::GetGlyphForXboxOrigin( EXboxOrigin eOrigin )
{
    PRINT_DEBUG_TODO();
    return "";
}

EControllerActionOrigin Steam_Controller::GetActionOriginFromXboxOrigin_( ControllerHandle_t controllerHandle, EXboxOrigin eOrigin )
{
    PRINT_DEBUG_TODO();
    return k_EControllerActionOrigin_None;
}

EInputActionOrigin Steam_Controller::GetActionOriginFromXboxOrigin( InputHandle_t inputHandle, EXboxOrigin eOrigin )
{
    PRINT_DEBUG_TODO();
    return k_EInputActionOrigin_None;
}

EControllerActionOrigin Steam_Controller::TranslateActionOrigin( ESteamInputType eDestinationInputType, EControllerActionOrigin eSourceOrigin )
{
    PRINT_DEBUG_TODO();
    return k_EControllerActionOrigin_None;
}

EInputActionOrigin Steam_Controller::TranslateActionOrigin( ESteamInputType eDestinationInputType, EInputActionOrigin eSourceOrigin )
{
    PRINT_DEBUG("steaminput destinationinputtype %d sourceorigin %d", eDestinationInputType, eSourceOrigin );
 
    if (eDestinationInputType == k_ESteamInputType_XBox360Controller)
        return eSourceOrigin;
 
    return k_EInputActionOrigin_None;
}

bool Steam_Controller::GetControllerBindingRevision( ControllerHandle_t controllerHandle, int *pMajor, int *pMinor )
{
    PRINT_DEBUG_TODO();
    return false;
}

bool Steam_Controller::GetDeviceBindingRevision( InputHandle_t inputHandle, int *pMajor, int *pMinor )
{
    PRINT_DEBUG_TODO();
    return false;
}

uint32 Steam_Controller::GetRemotePlaySessionID( InputHandle_t inputHandle )
{
    PRINT_DEBUG_TODO();
    return 0;
}

// Get a bitmask of the Steam Input Configuration types opted in for the current session. Returns ESteamInputConfigurationEnableType values.?	
// Note: user can override the settings from the Steamworks Partner site so the returned values may not exactly match your default configuration
uint16 Steam_Controller::GetSessionInputConfigurationSettings()
{
    PRINT_DEBUG_TODO();
    return 0;
}

// Set the trigger effect for a DualSense controller
void Steam_Controller::SetDualSenseTriggerEffect( InputHandle_t inputHandle, const ScePadTriggerEffectParam *pParam )
{
    PRINT_DEBUG_TODO();
}

void Steam_Controller::RunCallbacks()
{
    if (!explicitly_call_run_frame) {
        RunFrame();
    }
}
