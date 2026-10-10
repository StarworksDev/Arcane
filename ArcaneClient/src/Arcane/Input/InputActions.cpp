#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Base/Engine.hpp>   // ExecutablePathUtf8: the exe directory off-Windows
#include <Arcane/Core/Constant.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Input/InputSettings.hpp>

#include <Json.hpp>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Arcane
{
    namespace
    {
        // exe-relative path resolution (Assets.cpp pattern)
        std::filesystem::path ResolveInputPath(const std::filesystem::path& path)
        {
            if (path.is_absolute())
                return path;
#ifdef _WIN32
            wchar_t modulePath[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
                return std::filesystem::path(modulePath).parent_path() / path;
#else
            if (const std::string self = ExecutablePathUtf8(); !self.empty())
                return std::filesystem::path(self).parent_path() / path;
#endif
            return path;
        }

        // local binary file reader (Assets.cpp pattern)
        std::vector<uint8_t> ReadInputFileBytes(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                return {};
            const std::streamsize size = file.tellg();
            if (size <= 0)
                return {};
            file.seekg(0, std::ios::beg);
            std::vector<uint8_t> bytes((size_t)size);
            if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
                return {};
            return bytes;
        }

        // ControlSource / ControlId
        enum class ControlSource : uint8_t
        {
            None,
            Scancode,
            Keycode,
            MouseButton,
            GamepadButton,
            GamepadAxis,
            GamepadStick,   // resolved as a 2D vector (gamepad stick)
        };

        struct ControlId
        {
            ControlSource source = ControlSource::None;
            uint32_t code = 0;
        };

        // ---- the path compiler's vocabulary tables ----
        // Namespace-scope so the compiler (TryCompileSinglePath) and the
        // vocabulary statics (KnownControls / DisplayForPath) read the SAME
        // rows: a picker built from them can never offer a spelling the
        // compiler refuses.

        // LOVE->SDL key name translation table
        // LOVE names appear in the asset JSON; SDL name-lookup functions want
        // SDL names. Everything not in this table passes through unchanged
        // (single letters, digits, f1..f24 all match SDL names directly).
        // Each row also carries its scancode: SDL's name table is MUTABLE --
        // the Windows video driver renames LGUI/RGUI to "Left/Right Windows"
        // (SDL_windowskeyboard.c, SDL_SetScancodeName), Cocoa renames Alt and
        // GUI -- so a name lookup of "Left GUI" returns UNKNOWN in any
        // windowed host. The scancode is what the name meant.
        struct LoveKeyName { const char* love; const char* sdl; SDL_Scancode scancode; };
        constexpr LoveKeyName kLoveKeyNames[] = {
            { "lshift",    "Left Shift",  SDL_SCANCODE_LSHIFT    },
            { "rshift",    "Right Shift", SDL_SCANCODE_RSHIFT    },
            { "lctrl",     "Left Ctrl",   SDL_SCANCODE_LCTRL     },
            { "rctrl",     "Right Ctrl",  SDL_SCANCODE_RCTRL     },
            { "lalt",      "Left Alt",    SDL_SCANCODE_LALT      },
            { "ralt",      "Right Alt",   SDL_SCANCODE_RALT      },
            { "lgui",      "Left GUI",    SDL_SCANCODE_LGUI      },
            { "rgui",      "Right GUI",   SDL_SCANCODE_RGUI      },
            { "return",    "Return",      SDL_SCANCODE_RETURN    },
            { "escape",    "Escape",      SDL_SCANCODE_ESCAPE    },
            { "grave",     "`",           SDL_SCANCODE_GRAVE     },   // SDL names this key by its glyph, not "Grave"
            { "space",     "Space",       SDL_SCANCODE_SPACE     },
            { "tab",       "Tab",         SDL_SCANCODE_TAB       },
            { "backspace", "Backspace",   SDL_SCANCODE_BACKSPACE },
            { "up",        "Up",          SDL_SCANCODE_UP        },
            { "down",      "Down",        SDL_SCANCODE_DOWN      },
            { "left",      "Left",        SDL_SCANCODE_LEFT      },
            { "right",     "Right",       SDL_SCANCODE_RIGHT     },
        };

        const LoveKeyName* FindLoveKey(const std::string& loveName)
        {
            for (const auto& e : kLoveKeyNames)
                if (loveName == e.love)
                    return &e;
            return nullptr;
        }

        const char* LoveToSdlName(const std::string& loveName)
        {
            const LoveKeyName* e = FindLoveKey(loveName);
            return e ? e->sdl : nullptr;  // nullptr = pass through
        }

        // Gamepad token tables: control name -> bit/index, plus its readable
        // name. The bit/index order MUST match the InputDevices sampler's
        // snapshot layout (gamepadButtons bits, gamepadAxes).
        ARC_CONSTANT("sentinel: no gamepad token")
        constexpr int kNoGamepadToken = -1;

        struct GamepadToken { const char* name; int index; const char* display; };

        ARC_CONSTANT("SDL vocabulary: binding-token names mapped to SDL gamepad enum values; the binding file format")
        constexpr GamepadToken kGamepadButtonTokens[] = {
            { "buttonSouth",      0,  "South Button"      },
            { "buttonEast",       1,  "East Button"       },
            { "buttonWest",       2,  "West Button"       },
            { "buttonNorth",      3,  "North Button"      },
            { "dpadUp",           4,  "D-Pad Up"          },
            { "dpadDown",         5,  "D-Pad Down"        },
            { "dpadLeft",         6,  "D-Pad Left"        },
            { "dpadRight",        7,  "D-Pad Right"       },
            { "leftShoulder",     8,  "Left Shoulder"     },
            { "rightShoulder",    9,  "Right Shoulder"    },
            { "start",            10, "Start"             },
            { "back",             11, "Back"              },
            { "guide",            12, "Guide"             },
            { "leftStickPress",   13, "Left Stick Press"  },
            { "rightStickPress",  14, "Right Stick Press" },
        };

        ARC_CONSTANT("SDL vocabulary: binding-token names mapped to SDL gamepad enum values; the binding file format")
        constexpr GamepadToken kGamepadAxisTokens[] = {
            { "leftStick/x",   0, "Left Stick X"  },
            { "leftStick/y",   1, "Left Stick Y"  },
            { "rightStick/x",  2, "Right Stick X" },
            { "rightStick/y",  3, "Right Stick Y" },
            { "leftTrigger",   4, "Left Trigger"  },
            { "rightTrigger",  5, "Right Trigger" },
        };

        // Sticks resolve as a 2D vector: 0 = leftStick, 1 = rightStick.
        ARC_CONSTANT("SDL vocabulary: binding-token names mapped to SDL gamepad enum values; the binding file format")
        constexpr GamepadToken kGamepadStickTokens[] = {
            { "leftStick",  0, "Left Stick"  },
            { "rightStick", 1, "Right Stick" },
        };

        template <std::size_t N>
        int FindGamepadToken(const GamepadToken (&table)[N], const std::string& token)
        {
            for (const auto& e : table)
                if (token == e.name)
                    return e.index;
            return kNoGamepadToken;
        }

        int GamepadButtonToken(const std::string& token) { return FindGamepadToken(kGamepadButtonTokens, token); }
        int GamepadAxisToken(const std::string& token)   { return FindGamepadToken(kGamepadAxisTokens, token); }
        int GamepadStickToken(const std::string& token)  { return FindGamepadToken(kGamepadStickTokens, token); }

        // Mouse: the named buttons (array index = snapshot bit) and the
        // numbered form "button/N", N in 1..kMouseButtonCount (bit N-1).
        struct MouseButtonName { const char* name; const char* display; };
        constexpr MouseButtonName kMouseNamedButtons[] = {
            { "leftButton",   "Left Button"   },
            { "rightButton",  "Right Button"  },
            { "middleButton", "Middle Button" },
        };
        ARC_CONSTANT("SDL vocabulary: the mouse buttons SDL reports")
        constexpr int kMouseButtonCount = 5;

        // Path compiler
        // The quiet core: compiles a single simple path (no '+<' separator) to a
        // ControlId, nullopt for anything the compiler does not know, with NO
        // warning -- IsKnownControlPath and the editor's picker ask this
        // question thousands of times a session. An empty path is a silent
        // None, as it always was.
        // <Gamepad>/ paths compile to typed GamepadButton/Axis/Stick ControlIds.
        std::optional<ControlId> TryCompileSinglePath(const std::string& path)
        {
            // Parse <Device>/ctrl
            if (path.empty())
                return ControlId{};
            if (path[0] != '<')
                return std::nullopt;
            auto closeAngle = path.find('>');
            if (closeAngle == std::string::npos || closeAngle + 1 >= path.size() || path[closeAngle + 1] != '/')
                return std::nullopt;
            std::string device = path.substr(1, closeAngle - 1);
            std::string ctrl   = path.substr(closeAngle + 2);  // after '<Device>/'

            if (device == "Keyboard")
            {
                // Check for scancode sub-path
                constexpr std::string_view kScanPrefix = "scancode/";
                if (ctrl.size() > kScanPrefix.size() &&
                    ctrl.substr(0, kScanPrefix.size()) == kScanPrefix)
                {
                    std::string scanName = ctrl.substr(kScanPrefix.size());
                    const LoveKeyName* love = FindLoveKey(scanName);
                    SDL_Scancode sc = love ? love->scancode : SDL_GetScancodeFromName(scanName.c_str());
                    if (sc == SDL_SCANCODE_UNKNOWN)
                        return std::nullopt;
                    return ControlId{ ControlSource::Scancode, (uint32_t)sc };
                }
                else
                {
                    // Keycode path: translate LOVE name -> SDL name, then look up
                    const LoveKeyName* love = FindLoveKey(ctrl);
                    SDL_Keycode kc = SDL_GetKeyFromName(love ? love->sdl : ctrl.c_str());
                    // A renamed SDL scancode name: resolve the row's scancode
                    // through the layout exactly as SDL_GetKeyFromName would
                    // have for the original name.
                    if (kc == SDLK_UNKNOWN && love)
                        kc = SDL_GetKeyFromScancode(love->scancode, SDL_KMOD_NONE, false);
                    if (kc == SDLK_UNKNOWN)
                        return std::nullopt;
                    return ControlId{ ControlSource::Keycode, (uint32_t)kc };
                }
            }
            else if (device == "Mouse")
            {
                // leftButton=bit0, rightButton=bit1, middleButton=bit2, button/N=bit(N-1)
                for (uint32_t bit = 0; bit < std::size(kMouseNamedButtons); ++bit)
                    if (ctrl == kMouseNamedButtons[bit].name)
                        return ControlId{ ControlSource::MouseButton, bit };
                // button/N form
                if (ctrl.size() > 7 && ctrl.substr(0, 7) == "button/")
                {
                    try
                    {
                        int n = std::stoi(ctrl.substr(7));
                        if (n >= 1 && n <= kMouseButtonCount)
                            return ControlId{ ControlSource::MouseButton, (uint32_t)(n - 1) };
                    }
                    catch (...) {}
                }
                return std::nullopt;
            }
            else if (device == "Gamepad")
            {
                // Stick vector (leftStick / rightStick)
                int stick = GamepadStickToken(ctrl);
                if (stick >= 0)
                    return ControlId{ ControlSource::GamepadStick, (uint32_t)stick };

                // Axis (leftTrigger, rightTrigger, leftStick/x, etc.)
                int axis = GamepadAxisToken(ctrl);
                if (axis >= 0)
                    return ControlId{ ControlSource::GamepadAxis, (uint32_t)axis };

                // Button
                int btn = GamepadButtonToken(ctrl);
                if (btn >= 0)
                    return ControlId{ ControlSource::GamepadButton, (uint32_t)btn };

                return std::nullopt;
            }
            // Unknown device (e.g. <Wheel>)
            return std::nullopt;
        }

        // The loading compiler: the quiet core plus ONE load-time warn naming
        // map/action/path; an unknown path compiles to {None,0}.
        ControlId CompileSinglePath(const std::string& path,
                                    const std::string& mapName,
                                    const std::string& actionName)
        {
            if (const auto id = TryCompileSinglePath(path)) return *id;
            ARC_WARN("input: unknown control path '{}' in {}/{}", path, mapName, actionName);
            return {};
        }

        // CompilePath's '+' rule: a '+' separates chord parts only where the
        // next part begins. Every simple path starts with '<Device>', so "+<"
        // is the separator and any other '+' belongs to a control name (SDL's
        // "Keypad +", "Keypad +/-"). The last part is always emitted.
        std::vector<std::string> SplitChordParts(std::string_view path)
        {
            std::vector<std::string> parts;
            std::size_t start = 0;
            for (std::size_t i = 0; i + 1 < path.size(); ++i)
                if (path[i] == '+' && path[i + 1] == '<') { parts.emplace_back(path.substr(start, i - start)); start = i + 1; }
            parts.emplace_back(path.substr(start));
            return parts;
        }

        // Compiles a possibly-chorded path ("+<"-split) to a list of ControlIds.
        // An empty INTERIOR part ("+<Keyboard>/a") compiles to a silent {None},
        // so the chord can never fire; a trailing empty part (the whole of "")
        // is dropped. A trailing '+' is now part of the control name
        // ("<Keyboard>/a+" is one unknown control).
        std::vector<ControlId> CompilePath(const std::string& path,
                                           const std::string& mapName,
                                           const std::string& actionName)
        {
            std::vector<ControlId> chord;
            const std::vector<std::string> parts = SplitChordParts(path);
            for (std::size_t i = 0; i < parts.size(); ++i)
            {
                if (parts[i].empty() && i + 1 == parts.size()) continue;
                chord.push_back(CompileSinglePath(parts[i], mapName, actionName));
            }
            return chord;
        }
        // ResolveControl
        // Returns raw value [0,1] for buttons, signed float for axes.
        // Capture suppression lives HERE and only here (spec rule).
        float ResolveControl(const ControlId& id, const InputSnapshot& snap)
        {
            switch (id.source)
            {
            case ControlSource::Scancode:
                if (snap.wantCaptureKeyboard) return 0.0f;
                return snap.ScancodeDown(id.code) ? 1.0f : 0.0f;

            case ControlSource::Keycode:
                if (snap.wantCaptureKeyboard) return 0.0f;
                return snap.KeycodeDown(id.code) ? 1.0f : 0.0f;

            case ControlSource::MouseButton:
                if (snap.wantCaptureMouse) return 0.0f;
                return ((snap.mouseButtons >> id.code) & 1) ? 1.0f : 0.0f;

            case ControlSource::GamepadButton:
                if (!snap.gamepadConnected) return 0.0f;
                return ((snap.gamepadButtons >> id.code) & 1) ? 1.0f : 0.0f;

            case ControlSource::GamepadAxis:
                if (!snap.gamepadConnected) return 0.0f;
                if (id.code < 6) return snap.gamepadAxes[id.code];
                return 0.0f;

            case ControlSource::GamepadStick:
                // Sticks resolve to a vec2 via the composite/vector path
                // (the scalar ResolveControl path is not used for sticks).
                return 0.0f;

            case ControlSource::None:
            default:
                return 0.0f;
            }
        }

        // Resolve a chord: nonzero only if every part magnitude >= threshold
        // (input.pressThreshold). Returns the weakest part's magnitude when
        // all are down, else 0. (oracle resolvePath chord logic)
        float ResolveChord(const std::vector<ControlId>& chord, const InputSnapshot& snap, float threshold)
        {
            if (chord.empty()) return 0.0f;
            float minMag = 1.0f;
            for (const auto& id : chord)
            {
                const float mag = std::abs(ResolveControl(id, snap));
                if (mag < threshold) return 0.0f;
                minMag = std::min(minMag, mag);
            }
            return minMag;
        }

        // Processor ops
        struct ProcessorOp
        {
            enum class Kind { Invert, Scale, Deadzone, NormalizeVector2 } kind;
            // Unset = the input.deadzone.defaultMin / defaultMax of the
            // settings the evaluation runs under (DeadzoneBounds).
            std::optional<float> min;
            std::optional<float> max;
            float factor = 1.0f;
        };

        // A deadzone processor's [min, max]: its own parameters, else the
        // input.deadzone.* defaults.
        std::pair<float, float> DeadzoneBounds(const ProcessorOp& op, const InputDeadzoneSettings& dz)
        {
            return { op.min.value_or(dz.defaultMin), op.max.value_or(dz.defaultMax) };
        }

        ProcessorOp ParseProcessorToken(const std::string& token)
        {
            ProcessorOp op;
            // Try to match name(args) form
            auto paren = token.find('(');
            std::string name = (paren == std::string::npos) ? token : token.substr(0, paren);
            std::string argStr = (paren == std::string::npos) ? "" : token.substr(paren + 1);
            if (!argStr.empty() && argStr.back() == ')') argStr.pop_back();

            if (name == "invert")
            {
                op.kind = ProcessorOp::Kind::Invert;
            }
            else if (name == "scale")
            {
                op.kind = ProcessorOp::Kind::Scale;
                // parse factor=N
                auto pos = argStr.find("factor=");
                if (pos != std::string::npos)
                {
                    try { op.factor = std::stof(argStr.substr(pos + 7)); }
                    catch (...) {}
                }
            }
            else if (name == "deadzone")
            {
                op.kind = ProcessorOp::Kind::Deadzone;
                // parse min=N,max=N
                auto minPos = argStr.find("min=");
                if (minPos != std::string::npos)
                {
                    try { op.min = std::stof(argStr.substr(minPos + 4)); }
                    catch (...) {}
                }
                auto maxPos = argStr.find("max=");
                if (maxPos != std::string::npos)
                {
                    try { op.max = std::stof(argStr.substr(maxPos + 4)); }
                    catch (...) {}
                }
            }
            else if (name == "normalizeVector2")
            {
                op.kind = ProcessorOp::Kind::NormalizeVector2;
            }
            else
            {
                // Unknown processor: warn at load time, evaluate as passthrough
                // (Scale by 1 is identity for both scalar and vector forms).
                ARC_WARN("input: unknown processor '{}' (ignored)", name);
                op.kind = ProcessorOp::Kind::Scale;
                op.factor = 1.0f;
            }
            return op;
        }

        // CompiledBinding
        struct CompiledBinding
        {
            Guid id;
            std::vector<ControlId> chord;   // size 1 = simple path; size>1 = chord
            std::string path;               // original string (deferred features)
            std::vector<std::string> groups;
            std::vector<ProcessorOp> processors;
            bool isComposite = false;
            std::string compositeType;      // "2DVector" or "1DAxis"
            // Compiled composite parts: parts[part_name] = array of CompiledBinding
            // Populated at load for 2DVector (up/down/left/right) and 1DAxis (positive/negative).
            std::unordered_map<std::string, std::vector<CompiledBinding>> parts;
        };

        // Interaction
        struct Interaction
        {
            enum class Kind { Press, Hold, Tap } kind = Kind::Press;
            // Hold/Tap only. Unset = undecorated token: EvalAction resolves it
            // from the per-Update InputSettings read (input.holdSeconds /
            // input.tapSeconds, Live). An explicit duration= overrides.
            std::optional<float> duration;
        };

        Interaction ParseInteraction(const std::string& token)
        {
            Interaction it;
            auto paren = token.find('(');
            std::string name = (paren == std::string::npos) ? token : token.substr(0, paren);
            std::string argStr = (paren == std::string::npos) ? "" : token.substr(paren + 1);
            if (!argStr.empty() && argStr.back() == ')') argStr.pop_back();

            if (name == "hold")
            {
                it.kind = Interaction::Kind::Hold;
                auto pos = argStr.find("duration=");
                if (pos != std::string::npos)
                {
                    try { it.duration = std::stof(argStr.substr(pos + 9)); }
                    catch (...) {}
                }
            }
            else if (name == "tap")
            {
                it.kind = Interaction::Kind::Tap;
                auto pos = argStr.find("duration=");
                if (pos != std::string::npos)
                {
                    try { it.duration = std::stof(argStr.substr(pos + 9)); }
                    catch (...) {}
                }
            }
            else
            {
                it.kind = Interaction::Kind::Press;
            }
            return it;
        }

        // Action
        struct Action
        {
            Guid id;
            std::string name;
            std::string type;           // "Button" | "Value"
            std::string controlType;    // "Vector2" or empty
            InputActionType nativeType = InputActionType::Button;

            std::vector<CompiledBinding> bindings;
            Interaction interaction;

            // Per-frame eval state
            bool    prevDown = false;
            bool    curDown  = false;
            float   strength = 0.0f;
            glm::vec2 vec    = { 0.0f, 0.0f };

            // Phase state
            bool    started   = false;
            bool    performed = false;
            bool    canceled  = false;
            bool    _perfFired = false;
            bool    _tapValid  = false;
            double  heldTime   = 0.0;

            // Buffered press state
            uint64_t lastPressFrame = 0;
            bool     bufConsumed    = true;

            // Device contribution tracking
            bool kbmContrib = false;
            bool padContrib = false;
        };

        // Map
        struct Map
        {
            Guid id;
            std::string name;
            bool blocking = false;
            std::unordered_map<std::string, Action> actions;
        };

        // Compile bindings from JSON
        // Forward declaration needed because CompileBinding calls itself recursively
        // for composite parts.
        CompiledBinding CompileBinding(const nlohmann::json& bj,
                                       const std::string& mapName,
                                       const std::string& actionName);

        // Compile an array of binding JSON objects into a vector of CompiledBinding.
        std::vector<CompiledBinding> CompileBindingArray(const nlohmann::json& arr,
                                                          const std::string& mapName,
                                                          const std::string& actionName)
        {
            std::vector<CompiledBinding> result;
            if (!arr.is_array()) return result;
            for (const auto& bj : arr)
                result.push_back(CompileBinding(bj, mapName, actionName));
            return result;
        }

        CompiledBinding CompileBinding(const nlohmann::json& bj,
                                       const std::string& mapName,
                                       const std::string& actionName)
        {
            CompiledBinding cb;
            if (bj.contains("id") && bj["id"].is_string())
            {
                if (const auto parsed = Guid::FromString(bj["id"].get<std::string>()))
                    cb.id = *parsed;
            }
            if (bj.contains("groups") && bj["groups"].is_array())
            {
                for (const auto& group : bj["groups"])
                    if (group.is_string()) cb.groups.push_back(group.get<std::string>());
            }

            // Check for composite
            if (bj.contains("composite") && bj["composite"].is_string())
            {
                cb.isComposite = true;
                cb.compositeType = bj["composite"].get<std::string>();

                // Parse composite parts (oracle: binding.parts[key] = array of bindings).
                // 2DVector: up/down/left/right; 1DAxis: positive/negative.
                if (bj.contains("parts") && bj["parts"].is_object())
                {
                    const auto& partsJson = bj["parts"];
                    if (cb.compositeType == "2DVector")
                    {
                        for (const char* key : { "up", "down", "left", "right" })
                        {
                            if (partsJson.contains(key))
                                cb.parts[key] = CompileBindingArray(partsJson[key], mapName, actionName);
                        }
                    }
                    else if (cb.compositeType == "1DAxis")
                    {
                        for (const char* key : { "positive", "negative" })
                        {
                            if (partsJson.contains(key))
                                cb.parts[key] = CompileBindingArray(partsJson[key], mapName, actionName);
                        }
                    }
                }

                // Processors on composites
                if (bj.contains("processors") && bj["processors"].is_array())
                {
                    for (const auto& pt : bj["processors"])
                    {
                        if (pt.is_string())
                            cb.processors.push_back(ParseProcessorToken(pt.get<std::string>()));
                    }
                }
                return cb;
            }

            // Simple or chord path
            if (bj.contains("path") && bj["path"].is_string())
            {
                cb.path = bj["path"].get<std::string>();
                cb.chord = CompilePath(cb.path, mapName, actionName);
            }

            // Processors
            if (bj.contains("processors") && bj["processors"].is_array())
            {
                for (const auto& pt : bj["processors"])
                {
                    if (pt.is_string())
                        cb.processors.push_back(ParseProcessorToken(pt.get<std::string>()));
                }
            }

            return cb;
        }

        nlohmann::json NativeAsLegacy(const InputActionAsset& asset)
        {
            nlohmann::json document = asset.ToJson();
            for (auto& map : document["actionMaps"])
            {
                for (auto& action : map["actions"])
                {
                    const std::string type = action["type"].get<std::string>();
                    if (type == "Axis1D")
                        action["type"] = "Value";
                    else if (type == "Axis2D")
                    {
                        action["type"] = "Value";
                        action["controlType"] = "Vector2";
                    }
                    for (auto& binding : action["bindings"])
                    {
                        if (!binding.contains("composite")) continue;
                        nlohmann::json buckets = nlohmann::json::object();
                        for (const auto& part : binding["parts"])
                        {
                            const std::string role = part["name"].get<std::string>();
                            if (!buckets.contains(role)) buckets[role] = nlohmann::json::array();
                            buckets[role].push_back(part);
                        }
                        binding["parts"] = std::move(buckets);
                    }
                }
            }
            return document;
        }

        // InputActionsImpl
        class InputActionsImpl final : public InputActions
        {
            struct ActionRef { Map* map = nullptr; Action* action = nullptr; };
            struct BindingRef
            {
                CompiledBinding* binding = nullptr;
                Action* action = nullptr;
                std::string mapName;
                std::string actionName;
            };

        public:
            bool LoadJson(const nlohmann::json& doc) override
            {
                if (!doc.is_object() || !doc.contains("actionMaps") || !doc["actionMaps"].is_array())
                {
                    ARC_WARN("input: LoadJson failed: missing or invalid 'actionMaps' array");
                    return false;
                }

                m_maps.clear();
                m_contextStack.clear();
                m_frame = 0;
                m_pendingTransitions.clear();
                m_fixedTransitions.clear();
                m_overflowReported = false;
                m_nativeAsset.reset();
                m_mapById.clear();
                m_actionById.clear();
                m_bindingById.clear();
                m_activeSchemeGroup.clear();

                for (const auto& mj : doc["actionMaps"])
                {
                    if (!mj.is_object()) continue;
                    std::string mapName = mj.contains("name") && mj["name"].is_string()
                                         ? mj["name"].get<std::string>() : "";
                    if (mapName.empty()) continue;

                    Map m;
                    m.name = mapName;
                    m.blocking = mj.contains("blocking") && mj["blocking"].is_boolean()
                                 ? mj["blocking"].get<bool>() : false;

                    if (mj.contains("actions") && mj["actions"].is_array())
                    {
                        for (const auto& aj : mj["actions"])
                        {
                            if (!aj.is_object()) continue;
                            std::string aName = aj.contains("name") && aj["name"].is_string()
                                                ? aj["name"].get<std::string>() : "";
                            if (aName.empty()) continue;

                            Action a;
                            a.name = aName;
                            a.type = aj.contains("type") && aj["type"].is_string()
                                     ? aj["type"].get<std::string>() : "Button";
                            a.controlType = aj.contains("controlType") && aj["controlType"].is_string()
                                            ? aj["controlType"].get<std::string>() : "";

                            // Interaction (first entry only, oracle pattern)
                            if (aj.contains("interactions") && aj["interactions"].is_array()
                                && !aj["interactions"].empty() && aj["interactions"][0].is_string())
                            {
                                a.interaction = ParseInteraction(aj["interactions"][0].get<std::string>());
                            }

                            // Bindings
                            if (aj.contains("bindings") && aj["bindings"].is_array())
                            {
                                for (const auto& bj : aj["bindings"])
                                {
                                    a.bindings.push_back(CompileBinding(bj, mapName, aName));
                                }
                            }

                            m.actions[aName] = std::move(a);
                        }
                    }

                    m_maps[mapName] = std::move(m);
                }

                return true;
            }

            bool LoadAsset(const InputActionAsset& asset) override
            {
                std::string error;
                auto validated = InputActionAsset::FromJson(asset.ToJson(), &error);
                if (!validated)
                {
                    ARC_WARN("input: LoadAsset failed: {}", error);
                    return false;
                }

                // The legacy evaluator keys maps/actions by name. Check those
                // keys before replacing the last usable compiled definition.
                std::unordered_set<std::string> mapNames;
                for (const auto& map : validated->actionMaps)
                {
                    if (!mapNames.insert(map.name).second)
                    {
                        ARC_WARN("input: LoadAsset failed: duplicate map name '{}'", map.name);
                        return false;
                    }
                    std::unordered_set<std::string> actionNames;
                    for (const auto& action : map.actions)
                    {
                        if (!actionNames.insert(action.name).second)
                        {
                            ARC_WARN("input: LoadAsset failed: duplicate action name '{}' in '{}'",
                                     action.name, map.name);
                            return false;
                        }
                    }
                }

                if (!LoadJson(NativeAsLegacy(*validated)))
                    return false;
                m_nativeAsset = std::move(*validated);
                for (const auto& mapDef : m_nativeAsset->actionMaps)
                {
                    Map& map = m_maps.at(mapDef.name);
                    map.id = mapDef.id;
                    m_mapById[map.id] = &map;
                    for (const auto& actionDef : mapDef.actions)
                    {
                        Action& action = map.actions.at(actionDef.name);
                        action.id = actionDef.id;
                        action.nativeType = actionDef.type;
                        m_actionById[action.id] = { &map, &action };
                        for (auto& binding : action.bindings)
                            IndexBinding(binding, action, mapDef.name, actionDef.name);
                    }
                }
                return true;
            }

            bool SetControlScheme(std::string_view schemeName) override
            {
                if (schemeName.empty())
                {
                    m_activeSchemeGroup.clear();
                    return true;
                }
                if (!m_nativeAsset) return false;
                for (const auto& scheme : m_nativeAsset->controlSchemes)
                {
                    if (scheme.name == schemeName)
                    {
                        m_activeSchemeGroup = scheme.bindingGroup;
                        return true;
                    }
                }
                return false;
            }

            std::optional<Guid> FindAction(std::string_view mapName,
                                            std::string_view actionName) const override
            {
                if (!m_nativeAsset) return std::nullopt;
                for (const auto& map : m_nativeAsset->actionMaps)
                {
                    if (map.name != mapName) continue;
                    for (const auto& action : map.actions)
                        if (action.name == actionName) return action.id;
                }
                return std::nullopt;
            }

            std::optional<Guid> FindAction(std::string_view actionName) const override
            {
                if (!m_nativeAsset) return std::nullopt;
                std::optional<Guid> found;
                for (const auto& map : m_nativeAsset->actionMaps)
                {
                    for (const auto& action : map.actions)
                    {
                        if (action.name != actionName) continue;
                        if (found) return std::nullopt;
                        found = action.id;
                    }
                }
                return found;
            }

            InputActionValue Value(const Guid& actionId) const override
            {
                InputActionValue value;
                const auto it = m_actionById.find(actionId);
                if (it == m_actionById.end()) return value;
                const Action& action = *it->second.action;
                value.type = action.nativeType;
                if (!MapVisible(it->second.map)) return value;
                value.down = action.curDown;
                value.scalar = action.strength;
                value.vector = action.vec;
                if (action.canceled) value.phase = InputActionPhase::Canceled;
                else if (action.performed) value.phase = InputActionPhase::Performed;
                else if (action.started) value.phase = InputActionPhase::Started;
                return value;
            }

            std::optional<bool> ButtonDown(const Guid& actionId) const override
            {
                const auto it = m_actionById.find(actionId);
                if (it == m_actionById.end() || it->second.action->nativeType != InputActionType::Button)
                    return std::nullopt;
                return Value(actionId).down;
            }

            std::optional<float> ScalarValue(const Guid& actionId) const override
            {
                const auto it = m_actionById.find(actionId);
                if (it == m_actionById.end() || it->second.action->nativeType != InputActionType::Axis1D)
                    return std::nullopt;
                return Value(actionId).scalar;
            }

            std::optional<glm::vec2> VectorValue(const Guid& actionId) const override
            {
                const auto it = m_actionById.find(actionId);
                if (it == m_actionById.end() || it->second.action->nativeType != InputActionType::Axis2D)
                    return std::nullopt;
                return Value(actionId).vector;
            }

            std::optional<InputActionPhase> Phase(const Guid& actionId) const override
            {
                if (!m_actionById.contains(actionId)) return std::nullopt;
                return Value(actionId).phase;
            }

            bool Pressed(const Guid& id) const override
            {
                const auto it = m_actionById.find(id);
                return it != m_actionById.end() && MapVisible(it->second.map) &&
                    it->second.action->curDown && !it->second.action->prevDown;
            }
            bool Released(const Guid& id) const override
            {
                const auto it = m_actionById.find(id);
                return it != m_actionById.end() && MapVisible(it->second.map) &&
                    !it->second.action->curDown && it->second.action->prevDown;
            }
            bool Started(const Guid& id) const override
            {
                const auto it = m_actionById.find(id);
                return it != m_actionById.end() && MapVisible(it->second.map) && it->second.action->started;
            }
            bool Performed(const Guid& id) const override
            {
                const auto it = m_actionById.find(id);
                return it != m_actionById.end() && MapVisible(it->second.map) && it->second.action->performed;
            }
            bool Canceled(const Guid& id) const override
            {
                const auto it = m_actionById.find(id);
                return it != m_actionById.end() && MapVisible(it->second.map) && it->second.action->canceled;
            }

            bool SetBindingPath(const Guid& bindingId, std::string_view path) override
            {
                const auto it = m_bindingById.find(bindingId);
                if (it == m_bindingById.end() || it->second.binding->isComposite)
                    return false;
                BindingRef& ref = it->second;
                std::vector<ControlId> compiled = CompilePath(std::string(path), ref.mapName, ref.actionName);
                if (compiled.empty() || std::any_of(compiled.begin(), compiled.end(),
                    [](const ControlId& control) { return control.source == ControlSource::None; }))
                    return false;
                ref.binding->path = std::string(path);
                ref.binding->chord = std::move(compiled);
                Action& action = *ref.action;
                action.prevDown = false;
                action.curDown = false;
                action.strength = 0.0f;
                action.vec = glm::vec2(0.0f);
                action.started = action.performed = action.canceled = false;
                action._perfFired = action._tapValid = false;
                action.heldTime = 0.0;
                action.bufConsumed = true;
                return true;
            }

            std::vector<InputMapInfo> Maps() const override
            {
                std::vector<InputMapInfo> result;
                if (!m_nativeAsset) return result;
                result.reserve(m_nativeAsset->actionMaps.size());
                for (const auto& map : m_nativeAsset->actionMaps)
                    result.push_back({ map.id, map.name, map.blocking, map.priority });
                return result;
            }

            std::vector<InputActionInfo> Actions(const Guid& mapId) const override
            {
                std::vector<InputActionInfo> result;
                if (!m_nativeAsset) return result;
                for (const auto& map : m_nativeAsset->actionMaps)
                {
                    if (map.id != mapId) continue;
                    result.reserve(map.actions.size());
                    for (const auto& action : map.actions)
                        result.push_back({ action.id, map.id, action.name, action.type });
                    break;
                }
                return result;
            }

            std::vector<InputBindingInfo> Bindings(const Guid& actionId) const override
            {
                std::vector<InputBindingInfo> result;
                if (!m_nativeAsset) return result;
                for (const auto& map : m_nativeAsset->actionMaps)
                {
                    for (const auto& action : map.actions)
                    {
                        if (action.id != actionId) continue;
                        result.reserve(action.bindings.size());
                        for (const auto& binding : action.bindings)
                        {
                            InputBindingInfo info;
                            info.id = binding.id;
                            info.actionId = action.id;
                            info.authoredPath = binding.path;
                            info.effectivePath = EffectivePath(binding.id, binding.path);
                            info.composite = binding.composite;
                            info.groups = binding.groups;
                            for (const auto& part : binding.parts)
                                info.parts.push_back({ part.id, part.name, part.path,
                                    EffectivePath(part.id, part.path), part.groups });
                            result.push_back(std::move(info));
                        }
                        return result;
                    }
                }
                return result;
            }

            std::string BindingDisplayString(const Guid& bindingId) const override
            {
                const auto it = m_bindingById.find(bindingId);
                if (it == m_bindingById.end()) return {};
                const CompiledBinding& binding = *it->second.binding;
                if (binding.isComposite)
                    return binding.compositeType == "1DAxis" ? "1D Axis" : "2D Vector";
                const auto d = DisplayForPath(binding.path);
                return d.device.empty() ? d.control : d.device + " " + d.control;
            }

            float BindingValue(const Guid& bindingId) const override
            {
                const auto it = m_bindingById.find(bindingId);
                if (it == m_bindingById.end()) return 0.0f;
                const CompiledBinding& binding = *it->second.binding;
                const float threshold = Settings<InputSettings>().pressThreshold;
                if (!binding.isComposite) return RawBindingValue(binding, m_lastSnap, threshold);
                float best = 0.0f;
                for (const auto& [role, parts] : binding.parts)
                    for (const CompiledBinding& part : parts)
                        best = std::max(best, std::abs(RawBindingValue(part, m_lastSnap, threshold)));
                return best;
            }

            bool LoadFile(const std::filesystem::path& path) override
            {
                auto resolved = ResolveInputPath(path);
                auto bytes = ReadInputFileBytes(resolved);
                if (bytes.empty())
                {
                    ARC_WARN("input: LoadFile failed: '{}' not found or empty",
                             resolved.string());
                    return false;
                }

                auto doc = nlohmann::json::parse(
                    bytes.begin(), bytes.end(),
                    /*cb=*/nullptr,
                    /*allow_exceptions=*/false);
                if (doc.is_discarded())
                {
                    ARC_WARN("input: LoadFile failed: JSON parse error in '{}'",
                             resolved.string());
                    return false;
                }

                return LoadJson(doc);
            }

            void Update(double dt, const InputSnapshot& snap) override
            {
                m_lastSnap = snap;
                ++m_frame;

                // ONE settings read per evaluation, passed down to every action.
                const InputSettings& input = Settings<InputSettings>();
                const float threshold = input.pressThreshold;
                const InputDeadzoneSettings& deadzone = Settings<InputDeadzoneSettings>();

                bool kbmActive = false;
                float padMaxMag = 0.0f;

                for (auto& [mapName, m] : m_maps)
                {
                    // Edges are queued for the fixed step under the SAME
                    // visibility rule the frame queries apply (Pressed/Started/
                    // ... return neutral for a map that is not on the context
                    // stack, or sits below a blocking one): the fixed-step
                    // consumer must not see a Jump the frame consumer would not.
                    const bool visible = MapVisible(&m);
                    for (auto& [aName, a] : m.actions)
                    {
                        EvalAction(a, dt, snap, input, deadzone);
                        if (a.id.IsValid() && visible)
                        {
                            if (a.started) QueueTransition(a.id, InputActionPhase::Started);
                            if (a.performed) QueueTransition(a.id, InputActionPhase::Performed);
                            if (a.prevDown && !a.curDown)
                                QueueTransition(a.id, InputActionPhase::Canceled);
                        }

                        // Track device contribution for active-device hysteresis
                        if (a.kbmContrib) kbmActive = true;
                        if (a.padContrib) padMaxMag = std::max(padMaxMag, a.strength);
                    }
                }

                // Active device: kbm wins immediately; gamepad needs magnitude > input.pressThreshold
                if (kbmActive)
                    m_activeDevice = InputDevice::Kbm;
                else if (padMaxMag > threshold)
                    m_activeDevice = InputDevice::Gamepad;
            }

            void BeginFixedStep() override
            {
                m_fixedTransitions = std::move(m_pendingTransitions);
                m_pendingTransitions.clear();
                m_overflowReported = false;
            }

            bool PressedThisFixedStep(const Guid& action) const override
            {
                return std::any_of(m_fixedTransitions.begin(), m_fixedTransitions.end(),
                    [&](const InputActionTransition& t)
                    { return t.action == action && t.phase == InputActionPhase::Started; });
            }

            bool ReleasedThisFixedStep(const Guid& action) const override
            {
                return std::any_of(m_fixedTransitions.begin(), m_fixedTransitions.end(),
                    [&](const InputActionTransition& t)
                    { return t.action == action && t.phase == InputActionPhase::Canceled; });
            }

            std::span<const InputActionTransition> TransitionsThisFixedStep() const override
            {
                return m_fixedTransitions;
            }

            void PushContext(std::string_view map) override
            {
                std::string name(map);
                if (m_maps.find(name) == m_maps.end())
                {
                    ARC_WARN("input: PushContext: unknown map '{}'", name);
                    return;
                }
                m_contextStack.push_back(name);
            }

            void PopContext() override
            {
                if (!m_contextStack.empty())
                    m_contextStack.pop_back();
            }

            void SetBaseContext(std::string_view map) override
            {
                m_contextStack.clear();
                std::string name(map);
                if (m_maps.find(name) != m_maps.end())
                    m_contextStack.push_back(name);
            }

            void SwapBaseContext(std::string_view map) override
            {
                std::string name(map);
                if (m_maps.find(name) == m_maps.end())
                {
                    ARC_WARN("input: SwapBaseContext: unknown map '{}'", name);
                    return;
                }
                if (m_contextStack.empty())
                    m_contextStack.push_back(name);
                else
                    m_contextStack[0] = name;
            }

            std::string ActiveContext() const override
            {
                return m_contextStack.empty() ? "" : m_contextStack.back();
            }

            bool Down(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a && a->curDown;
            }

            bool Pressed(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a && a->curDown && !a->prevDown;
            }

            bool Released(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a && a->prevDown && !a->curDown;
            }

            bool Started(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a && a->started;
            }

            bool Performed(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a && a->performed;
            }

            bool Canceled(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a && a->canceled;
            }

            float Strength(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a ? a->strength : 0.0f;
            }

            glm::vec2 Axis(std::string_view action) const override
            {
                const Action* a = Resolve(action);
                return a ? a->vec : glm::vec2(0.0f);
            }

            bool Buffered(std::string_view action, int frames) override
            {
                Action* a = ResolveMutable(action);
                if (!a || a->bufConsumed) return false;
                if ((int64_t)(m_frame - a->lastPressFrame) <= frames)
                {
                    a->bufConsumed = true;
                    return true;
                }
                return false;
            }

            InputDevice ActiveDevice() const override
            {
                return m_activeDevice;
            }

        private:
            std::unordered_map<std::string, Map> m_maps;
            std::vector<std::string> m_contextStack;
            uint64_t m_frame = 0;
            InputDevice m_activeDevice = InputDevice::Kbm;
            std::optional<InputActionAsset> m_nativeAsset;
            std::string m_activeSchemeGroup;
            std::unordered_map<Guid, Map*> m_mapById;
            std::unordered_map<Guid, ActionRef> m_actionById;
            std::unordered_map<Guid, BindingRef> m_bindingById;
            std::vector<InputActionTransition> m_pendingTransitions;
            std::vector<InputActionTransition> m_fixedTransitions;
            bool m_overflowReported = false;
            // input.maxQueuedTransitions, latched at construction (Restart).
            const std::size_t m_maxTransitions = Settings<InputSettings>().maxQueuedTransitions;
            InputSnapshot m_lastSnap{};   // the last Update's snapshot, for BindingValue

            void QueueTransition(const Guid& action, InputActionPhase phase)
            {
                if (m_pendingTransitions.size() == m_maxTransitions)
                {
                    m_pendingTransitions.erase(m_pendingTransitions.begin());
                    if (!m_overflowReported)
                    {
                        ARC_WARN("input: fixed-step transition queue overflow; oldest transition discarded");
                        m_overflowReported = true;
                    }
                }
                m_pendingTransitions.push_back({ action, phase, m_frame });
            }

            void IndexBinding(CompiledBinding& binding, Action& action,
                              const std::string& mapName, const std::string& actionName)
            {
                if (binding.id.IsValid())
                    m_bindingById[binding.id] = { &binding, &action, mapName, actionName };
                for (auto& [role, parts] : binding.parts)
                    for (auto& part : parts)
                        IndexBinding(part, action, mapName, actionName);
            }

            bool MapVisible(const Map* target) const
            {
                for (int i = static_cast<int>(m_contextStack.size()) - 1; i >= 0; --i)
                {
                    const auto it = m_maps.find(m_contextStack[i]);
                    if (it == m_maps.end()) continue;
                    if (&it->second == target) return true;
                    if (it->second.blocking) return false;
                }
                return false;
            }

            std::string EffectivePath(const Guid& id, const std::string& authored) const
            {
                const auto it = m_bindingById.find(id);
                return it == m_bindingById.end() ? authored : it->second.binding->path;
            }

            static bool SchemeAllows(const CompiledBinding& binding, std::string_view activeGroup)
            {
                if (activeGroup.empty() || binding.groups.empty()) return true;
                return std::find(binding.groups.begin(), binding.groups.end(), activeGroup) != binding.groups.end();
            }

            // Resolve action name through context stack top-down.
            // A blocking map stops fall-through (oracle resolve()).
            const Action* Resolve(std::string_view name) const
            {
                for (int i = (int)m_contextStack.size() - 1; i >= 0; --i)
                {
                    auto mit = m_maps.find(m_contextStack[i]);
                    if (mit == m_maps.end()) continue;
                    const Map& m = mit->second;
                    auto ait = m.actions.find(std::string(name));
                    if (ait != m.actions.end()) return &ait->second;
                    if (m.blocking) return nullptr;
                }
                return nullptr;
            }

            Action* ResolveMutable(std::string_view name)
            {
                for (int i = (int)m_contextStack.size() - 1; i >= 0; --i)
                {
                    auto mit = m_maps.find(m_contextStack[i]);
                    if (mit == m_maps.end()) continue;
                    Map& m = mit->second;
                    auto ait = m.actions.find(std::string(name));
                    if (ait != m.actions.end()) return &ait->second;
                    if (m.blocking) return nullptr;
                }
                return nullptr;
            }

            // Evaluate one action (oracle evalAction).
            // Implements composite resolution, GamepadStick vector path,
            // best-vector / best-scalar split, and Vector2 action reporting.
            // pressThreshold, holdSeconds and tapSeconds all come from the ONE
            // InputSettings read Update makes.
            void EvalAction(Action& a, double dt, const InputSnapshot& snap, const InputSettings& input,
                            const InputDeadzoneSettings& deadzone)
            {
                const float threshold = input.pressThreshold;
                bool isVec = (a.controlType == "Vector2");
                float bestScalar = 0.0f;
                float bestScalarMag = 0.0f;
                glm::vec2 bestVec(0.0f, 0.0f);
                float bestVecLen = 0.0f;
                bool kbmContrib = false;
                bool padContrib = false;

                for (const auto& b : a.bindings)
                {
                    if (!SchemeAllows(b, m_activeSchemeGroup)) continue;
                    if (b.isComposite)
                    {
                        // Composite resolution (oracle: resolveComposite / partStrength).
                        auto getPartBindings = [&](const std::string& key) -> const std::vector<CompiledBinding>*
                        {
                            auto it = b.parts.find(key);
                            return (it != b.parts.end()) ? &it->second : nullptr;
                        };

                        static const std::vector<CompiledBinding> kEmpty;
                        auto getPart = [&](const std::string& key) -> const std::vector<CompiledBinding>&
                        {
                            const auto* p = getPartBindings(key);
                            return p ? *p : kEmpty;
                        };

                        if (b.compositeType == "1DAxis")
                        {
                            // oracle: pos - neg, then scalar processors
                            float pos = PartStrength(getPart("positive"), snap, m_activeSchemeGroup, threshold, deadzone);
                            float neg = PartStrength(getPart("negative"), snap, m_activeSchemeGroup, threshold, deadzone);
                            float val = ApplyScalarProcessors(b.processors, pos - neg, deadzone);
                            float mag = std::abs(val);
                            if (mag > bestScalarMag)
                            {
                                bestScalarMag = mag;
                                bestScalar    = val;
                            }
                            // Composite parts are assumed keyboard-sourced (all authored composites are
                            // WASD today). Gamepad-sourced composites would need device plumbing from
                            // PartStrength; deferred until an asset needs it.
                            if (mag >= threshold) kbmContrib = true;
                        }
                        else  // 2DVector (default)
                        {
                            // oracle: vec = {right-left, down-up}, then vector processors
                            float up    = PartStrength(getPart("up"),    snap, m_activeSchemeGroup, threshold, deadzone);
                            float down  = PartStrength(getPart("down"),  snap, m_activeSchemeGroup, threshold, deadzone);
                            float left  = PartStrength(getPart("left"),  snap, m_activeSchemeGroup, threshold, deadzone);
                            float right = PartStrength(getPart("right"), snap, m_activeSchemeGroup, threshold, deadzone);
                            glm::vec2 rawVec(right - left, down - up);
                            glm::vec2 vec = ApplyVectorProcessors(b.processors, rawVec, deadzone);
                            float len = glm::length(vec);
                            if (len > bestVecLen)
                            {
                                bestVecLen = len;
                                bestVec    = vec;
                            }
                            // Device contribution: composite parts are keyboard (scancodes)
                            if (len >= threshold) kbmContrib = true;
                        }
                        continue;
                    }

                    // Simple / chord path -- check for GamepadStick (vector path)
                    if (b.chord.size() == 1 && b.chord[0].source == ControlSource::GamepadStick)
                    {
                        glm::vec2 rawVec = ResolveControlVec(b.chord[0], snap);
                        glm::vec2 vec = ApplyVectorProcessors(b.processors, rawVec, deadzone);
                        float len = glm::length(vec);
                        if (len > bestVecLen)
                        {
                            bestVecLen = len;
                            bestVec    = vec;
                        }
                        if (len >= threshold) padContrib = true;
                        continue;
                    }

                    // Scalar path (keyboard / mouse / gamepad button / gamepad axis / chord)
                    float raw = ResolveChord(b.chord, snap, threshold);
                    float val = ApplyScalarProcessors(b.processors, raw, deadzone);
                    float mag = std::abs(val);
                    if (mag > bestScalarMag)
                    {
                        bestScalarMag = mag;
                        bestScalar    = val;
                    }

                    // Device contribution tracking
                    if (mag >= threshold)
                    {
                        for (const auto& id : b.chord)
                        {
                            if (id.source == ControlSource::Scancode ||
                                id.source == ControlSource::Keycode  ||
                                id.source == ControlSource::MouseButton)
                            {
                                kbmContrib = true;
                            }
                            else if (id.source == ControlSource::GamepadButton ||
                                     id.source == ControlSource::GamepadAxis   ||
                                     id.source == ControlSource::GamepadStick)
                            {
                                padContrib = true;
                            }
                        }
                    }
                }

                a.prevDown = a.curDown;

                if (isVec)
                {
                    // Vector2 action: report the winning vector binding (max length).
                    // strength = length (oracle: a.strength = bestVecLen for hysteresis).
                    a.vec      = bestVec;
                    a.strength = bestVecLen;
                    a.curDown  = bestVecLen >= threshold;
                }
                else
                {
                    a.vec      = glm::vec2(0.0f);
                    a.strength = bestScalar;
                    a.curDown  = bestScalarMag >= threshold;
                }

                a.kbmContrib = kbmContrib;
                a.padContrib = padContrib;

                // Interaction phase logic (oracle evalAction)
                bool rising  = a.curDown && !a.prevDown;
                bool falling = a.prevDown && !a.curDown;

                if (rising)
                {
                    a.heldTime      = 0.0;
                    a._perfFired    = false;
                    a._tapValid     = true;
                    a.lastPressFrame = m_frame;
                    a.bufConsumed   = false;
                }
                else if (a.curDown)
                {
                    a.heldTime += dt;
                }

                a.started   = rising;
                a.performed = false;
                a.canceled  = false;

                const Interaction& it = a.interaction;
                if (it.kind == Interaction::Kind::Hold)
                {
                    const float duration = it.duration.value_or(input.holdSeconds);
                    if (a.curDown && !a._perfFired && a.heldTime >= (double)duration)
                    {
                        a.performed  = true;
                        a._perfFired = true;
                    }
                    else if (falling && !a._perfFired)
                    {
                        a.canceled = true;
                    }
                }
                else if (it.kind == Interaction::Kind::Tap)
                {
                    const float duration = it.duration.value_or(input.tapSeconds);
                    if (a.curDown && a.heldTime > (double)duration)
                        a._tapValid = false;
                    if (falling)
                    {
                        if (a._tapValid && a.heldTime <= (double)duration)
                            a.performed = true;
                        else
                            a.canceled = true;
                    }
                }
                else  // Press (default)
                {
                    a.performed = rising;
                    a.canceled  = falling;
                }
            }

            // Apply scalar processors in order.
            static float ApplyScalarProcessors(const std::vector<ProcessorOp>& procs, float v,
                                               const InputDeadzoneSettings& deadzone)
            {
                for (const auto& op : procs)
                {
                    switch (op.kind)
                    {
                    case ProcessorOp::Kind::Invert:
                        v = -v;
                        break;
                    case ProcessorOp::Kind::Scale:
                        v *= op.factor;
                        break;
                    case ProcessorOp::Kind::Deadzone:
                    {
                        const auto [lo, hi] = DeadzoneBounds(op, deadzone);
                        float m = std::abs(v);
                        float sgn = (v < 0.0f) ? -1.0f : 1.0f;
                        if (m < lo) v = 0.0f;
                        else if (m > hi) v = sgn;
                        else v = sgn * (m - lo) / (hi - lo);
                        break;
                    }
                    case ProcessorOp::Kind::NormalizeVector2:
                        // Scalar NormalizeVector2 is a no-op on scalars (vectors handled below)
                        break;
                    }
                }
                return v;
            }

            // Apply vector processors in order (oracle: applyVector in Input.lua).
            // NormalizeVector2: len > 1e-6 -> v/len, else {0,0}.
            // Deadzone (radial): len < min -> {0,0}; else scale = (len>max?1:(len-min)/(max-min));
            //   k = scale/len; return v*k.
            // Invert: {-v.x, -v.y}.
            // Scale: v * factor (passthrough).
            static glm::vec2 ApplyVectorProcessors(const std::vector<ProcessorOp>& procs, glm::vec2 v,
                                                   const InputDeadzoneSettings& deadzone)
            {
                for (const auto& op : procs)
                {
                    switch (op.kind)
                    {
                    case ProcessorOp::Kind::NormalizeVector2:
                    {
                        float len = glm::length(v);
                        v = (len > 1e-6f) ? (v / len) : glm::vec2(0.0f, 0.0f);
                        break;
                    }
                    case ProcessorOp::Kind::Deadzone:
                    {
                        const auto [lo, hi] = DeadzoneBounds(op, deadzone);
                        float len = glm::length(v);
                        if (len < lo)
                        {
                            v = glm::vec2(0.0f, 0.0f);
                        }
                        else
                        {
                            float scaled = (len > hi) ? 1.0f : (len - lo) / (hi - lo);
                            float k = scaled / len;
                            v = v * k;
                        }
                        break;
                    }
                    case ProcessorOp::Kind::Invert:
                        v = glm::vec2(-v.x, -v.y);
                        break;
                    case ProcessorOp::Kind::Scale:
                        v = v * op.factor;
                        break;
                    }
                }
                return v;
            }

            // Resolve a GamepadStick control to a 2D vector.
            // idx=0 -> leftStick (axes 0,1); idx=1 -> rightStick (axes 2,3).
            // Gamepad is NOT capture-suppressed (spec rule; captured by ResolveControl
            // for scalar paths -- sticks use this dedicated vec path instead).
            static glm::vec2 ResolveControlVec(const ControlId& id, const InputSnapshot& snap)
            {
                if (id.source == ControlSource::GamepadStick && snap.gamepadConnected)
                {
                    uint32_t base = id.code * 2;  // 0->axes[0,1], 1->axes[2,3]
                    if (base + 1 < 6)
                        return glm::vec2(snap.gamepadAxes[base], snap.gamepadAxes[base + 1]);
                }
                return glm::vec2(0.0f, 0.0f);
            }

            // One simple/chord binding's raw value, before processors: the
            // chord value, or a stick binding's vector length (ResolveChord
            // reads a stick as 0 -- sticks resolve through the vector path).
            static float RawBindingValue(const CompiledBinding& binding, const InputSnapshot& snap, float threshold)
            {
                if (binding.chord.size() == 1 && binding.chord[0].source == ControlSource::GamepadStick)
                    return glm::length(ResolveControlVec(binding.chord[0], snap));
                return ResolveChord(binding.chord, snap, threshold);
            }

            // Compute the max-magnitude scalar strength for a composite part's
            // binding array (oracle: partStrength in Input.lua).
            // Each binding in the array is a simple/chord path (no nested composites).
            static float PartStrength(const std::vector<CompiledBinding>& partBindings,
                                      const InputSnapshot& snap, std::string_view activeGroup,
                                      float threshold, const InputDeadzoneSettings& deadzone)
            {
                float best = 0.0f;
                for (const auto& pb : partBindings)
                {
                    if (!SchemeAllows(pb, activeGroup)) continue;
                    float raw = ResolveChord(pb.chord, snap, threshold);
                    float val = ApplyScalarProcessors(pb.processors, raw, deadzone);
                    float mag = std::abs(val);
                    if (mag > best) best = mag;
                }
                return best;
            }
        };

    }  // anonymous namespace

    // ---- control-path vocabulary (input-editor redesign spec s2.4) ----
    // Every function below reads the path compiler's own tables and its quiet
    // core (TryCompileSinglePath): what they offer and what LoadAsset accepts
    // are one vocabulary.
    namespace
    {
        const char* GamepadDisplayName(const std::string& token)
        {
            for (std::span<const GamepadToken> table : { std::span<const GamepadToken>(kGamepadButtonTokens),
                                       std::span<const GamepadToken>(kGamepadAxisTokens),
                                       std::span<const GamepadToken>(kGamepadStickTokens) })
                for (const auto& e : table)
                    if (token == e.name) return e.display;
            return nullptr;
        }

        // Camel-case split with every word capitalised: the generic readable
        // form for a control the tables and SDL do not name
        // ("someControl/x" -> "Some Control X", "page down" -> "Page Down").
        std::string CamelToWords(const std::string& control)
        {
            std::string readable;
            for (char c : control)
            {
                if (c == '/') readable += ' ';
                else if (std::isupper(static_cast<unsigned char>(c)) && !readable.empty() &&
                         std::islower(static_cast<unsigned char>(readable.back()))) { readable += ' '; readable += c; }
                else readable += c;
            }
            for (std::size_t i = 0; i < readable.size(); ++i)
                if (i == 0 || readable[i - 1] == ' ')
                    readable[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(readable[i])));
            return readable;
        }

        std::string ReadableControl(const std::string& device, std::string control)
        {
            if (device == "Gamepad")
                if (const char* n = GamepadDisplayName(control)) return n;
            if (device == "Keyboard")
            {
                constexpr std::string_view scan = "scancode/";
                const bool isScancode = control.starts_with(scan);
                if (isScancode) control.erase(0, scan.size());
                if (const char* sdl = LoveToSdlName(control)) return sdl;   // "Left Shift", "Space"
                if (control.size() == 1)
                    return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(control[0]))));
                if (control.size() >= 2 && control[0] == 'f' && std::isdigit(static_cast<unsigned char>(control[1])))
                {
                    control[0] = 'F';
                    return control;
                }
                // A captured path carries SDL's own (lower-cased) name: resolve it
                // through the same lookup the compiler uses so it displays with
                // SDL's canonical spelling ("left shift" -> "Left Shift"), never
                // through string surgery. Empty means SDL does not know the text.
                if (isScancode)
                {
                    if (const char* n = SDL_GetScancodeName(SDL_GetScancodeFromName(control.c_str())); n && *n) return n;
                }
                else
                {
                    if (const char* n = SDL_GetKeyName(SDL_GetKeyFromName(control.c_str())); n && *n) return n;
                }
            }
            if (device == "Mouse")
            {
                for (const auto& e : kMouseNamedButtons)
                    if (control == e.name) return e.display;
                if (control.starts_with("button/")) return "Button " + control.substr(7);
            }
            // Fallback: the camel-case split BindingDisplayString always did.
            return CamelToWords(control);
        }
    }

    InputControlDisplay InputActions::DisplayForPath(std::string_view pathView)
    {
        const std::string path(pathView);
        std::vector<std::pair<std::string, std::string>> parts;   // (device, readable)
        for (const std::string& part : SplitChordParts(path))
        {
            const std::size_t close = part.find(">/");
            if (part.empty() || part.front() != '<' || close == std::string::npos)
                return { {}, path };   // unparseable: hand the raw text back whole
            std::string device = part.substr(1, close - 1);
            std::string name = ReadableControl(device, part.substr(close + 2));
            parts.emplace_back(std::move(device), std::move(name));
        }
        const bool oneDevice = std::all_of(parts.begin(), parts.end(),
            [&](const auto& p) { return p.first == parts.front().first; });
        InputControlDisplay out;
        if (oneDevice) out.device = parts.front().first;
        for (const auto& [device, name] : parts)
        {
            if (!out.control.empty()) out.control += " + ";
            out.control += oneDevice ? name : device + " " + name;
        }
        return out;
    }

    bool InputActions::IsKnownControlPath(std::string_view path)
    {
        if (path.empty()) return false;
        for (const std::string& part : SplitChordParts(path))
            if (part.empty() || !TryCompileSinglePath(part)) return false;
        return true;
    }

    std::string InputActions::CanonicalControlKey(std::string_view path)
    {
        if (path.empty()) return {};
        std::vector<std::string> keys;
        for (const std::string& part : SplitChordParts(path))
        {
            std::optional<ControlId> id;
            if (!part.empty()) id = TryCompileSinglePath(part);
            if (!id) return {};
            if (id->source == ControlSource::Scancode)
            {
                // The running layout's translation -- the exact call
                // InputDevices.cpp makes when it fills a snapshot -- so
                // "<Keyboard>/scancode/space" and "<Keyboard>/space" meet at one
                // keycode. An untranslatable scancode stays a Scancode part.
                const SDL_Keycode kc = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(id->code), SDL_KMOD_NONE, false);
                if (kc != SDLK_UNKNOWN) id = ControlId{ ControlSource::Keycode, static_cast<uint32_t>(kc) };
            }
            keys.push_back(std::to_string(static_cast<int>(id->source)) + ':' + std::to_string(id->code));
        }
        std::sort(keys.begin(), keys.end());   // "a+b" == "b+a": ResolveChord needs every part down, order is spelling
        std::string out;
        for (const std::string& k : keys) { if (!out.empty()) out += '+'; out += k; }
        return out;
    }

    std::vector<InputControlChoice> InputActions::KnownControls()
    {
        std::vector<InputControlChoice> out;
        auto add = [&](std::string path) { InputControlDisplay d = DisplayForPath(path); out.push_back({ std::move(path), std::move(d) }); };
        // Keyboard: the LOVE-named table, then the names the Keyboard branch
        // passes straight to SDL (letters, digits, F1..F12).
        for (const auto& e : kLoveKeyNames) add(std::string("<Keyboard>/") + e.love);
        for (char c = 'a'; c <= 'z'; ++c) add(std::string("<Keyboard>/") + c);
        for (char c = '0'; c <= '9'; ++c) add(std::string("<Keyboard>/") + c);
        for (int f = 1; f <= 12; ++f) add("<Keyboard>/f" + std::to_string(f));
        // Mouse: the named buttons, then the numbered ones they do not cover.
        for (const auto& e : kMouseNamedButtons) add(std::string("<Mouse>/") + e.name);
        for (int n = static_cast<int>(std::size(kMouseNamedButtons)) + 1; n <= kMouseButtonCount; ++n)
            add("<Mouse>/button/" + std::to_string(n));
        // Gamepad: buttons, axes, sticks -- the three token tables.
        for (const auto& e : kGamepadButtonTokens) add(std::string("<Gamepad>/") + e.name);
        for (const auto& e : kGamepadAxisTokens)   add(std::string("<Gamepad>/") + e.name);
        for (const auto& e : kGamepadStickTokens)  add(std::string("<Gamepad>/") + e.name);
        return out;
    }
    std::unique_ptr<InputActions> InputActions::Create()
    {
        return std::make_unique<InputActionsImpl>();
    }

}  // namespace Arcane
