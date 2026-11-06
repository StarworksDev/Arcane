#include <Arcane/Remote/Schema.hpp>
#include <Arcane/Remote/Commands/CommandSupport.hpp>   // WriteMaskedFields: a Protected default is masked (C-M8)

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Reflection.hpp>
#include <Arcane/Serialization/ReflectionJson.hpp>
#include <Astra/Reflection/JsonSchema.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

bool IsGlmWire(std::uint64_t typeHash) {
    return typeHash == Astra::TypeID<glm::vec2>::Hash()
        || typeHash == Astra::TypeID<glm::vec3>::Hash()
        || typeHash == Astra::TypeID<glm::vec4>::Hash()
        || typeHash == Astra::TypeID<glm::quat>::Hash()
        || typeHash == Astra::TypeID<glm::mat3>::Hash()
        || typeHash == Astra::TypeID<glm::mat4>::Hash();
}

int GlmItemCount(std::uint64_t typeHash) {
    if (typeHash == Astra::TypeID<glm::vec2>::Hash()) return 2;
    if (typeHash == Astra::TypeID<glm::vec3>::Hash()) return 3;
    if (typeHash == Astra::TypeID<glm::vec4>::Hash()) return 4;
    if (typeHash == Astra::TypeID<glm::quat>::Hash()) return 4;
    if (typeHash == Astra::TypeID<glm::mat3>::Hash()) return 9;
    if (typeHash == Astra::TypeID<glm::mat4>::Hash()) return 16;
    return 0;
}

// ---- Fallback sub-task (Task 4): the vendored Astra (c46a90ed) has no
// `Optional` attribute, lists every non-hidden field in `required`, and emits
// "type":"object" for a nested class. This adapter gives the shape the Astra
// 1.0 lane ships. Delete FieldIsOptional, EmitDef, ApplySchemaAdapter, its
// call in SchemaFor and the local Arcane::Attr::Optional once a sync brings
// `struct Optional` into Astra's Attribute.hpp.

bool FieldIsOptional(const Astra::FieldInfo& field) {
    return field.HasAttribute<Arcane::Attr::Optional>();
}

void EmitDef(nlohmann::json& defs, const Astra::TypeMeta& meta,
             std::vector<const Astra::TypeMeta*>& pending) {
    const std::string name(meta.typeName);
    if (defs.contains(name))
        return;
    nlohmann::json def = nlohmann::json::object();
    def["type"] = "object";
    def["properties"] = nlohmann::json::object();
    def["additionalProperties"] = false;
    nlohmann::json required = nlohmann::json::array();
    for (const Astra::FieldInfo& field : meta.fields) {
        if (field.IsHidden())
            continue;
        const std::string fname(field.name);
        def["properties"][fname] = nlohmann::json::object();
        if (!FieldIsOptional(field))
            required.push_back(fname);
        const Astra::TypeMeta* nested = Astra::GetMeta(field.typeHash);
        if (nested == nullptr || nested->isEnum || IsGlmWire(field.typeHash) || nested == &meta)
            continue;
        def["properties"][fname]["$ref"] = std::string("#/$defs/") + std::string(nested->typeName);
        pending.push_back(nested);
    }
    if (!required.empty())
        def["required"] = std::move(required);
    defs[name] = std::move(def);
}

void ApplySchemaAdapter(nlohmann::json& schema, const Astra::TypeMeta& meta) {
    if (!schema.contains("properties") || !schema["properties"].is_object())
        schema["properties"] = nlohmann::json::object();
    if (!schema.contains("$defs") || !schema["$defs"].is_object())
        schema["$defs"] = nlohmann::json::object();
    nlohmann::json required = schema.contains("required") && schema["required"].is_array()
        ? schema["required"] : nlohmann::json::array();
    std::vector<const Astra::TypeMeta*> pending;
    for (const Astra::FieldInfo& field : meta.fields) {
        if (field.IsHidden())
            continue;
        const std::string fname(field.name);
        if (FieldIsOptional(field)) {
            nlohmann::json kept = nlohmann::json::array();
            for (const auto& item : required) {
                if (!item.is_string() || item.get<std::string>() != fname)
                    kept.push_back(item);
            }
            required = std::move(kept);
        }
        const Astra::TypeMeta* nested = Astra::GetMeta(field.typeHash);
        if (nested == nullptr || nested->isEnum || IsGlmWire(field.typeHash))
            continue;
        if (!schema["properties"].contains(fname) || !schema["properties"][fname].is_object())
            schema["properties"][fname] = nlohmann::json::object();
        auto& prop = schema["properties"][fname];
        if (!prop.contains("$ref"))
            prop["$ref"] = std::string("#/$defs/") + std::string(nested->typeName);
        prop.erase("type");
        pending.push_back(nested);
    }
    if (required.empty())
        schema.erase("required");
    else
        schema["required"] = std::move(required);
    for (std::size_t i = 0; i < pending.size(); ++i)
        EmitDef(schema["$defs"], *pending[i], pending);
    if (schema["$defs"].empty())
        schema.erase("$defs");
}

// ---- Vendored Astra (c46a90ed) literal bug: JsonSchema.hpp:50 ("$schema"),
// :56 (top-level "type") and :276 (array "items" "type") pass a string
// literal to WriteProperty, and const char* -> bool beats the string_view
// overload, so Astra writes `true` ("accept anything") where it means a
// string. Remove RepairAstraBoolLiterals and its call in SchemaFor once an
// Astra sync passes those literals as strings.
void RepairItems(nlohmann::json& node) {
    if (node.is_array()) {
        for (auto& child : node)
            RepairItems(child);
        return;
    }
    if (!node.is_object())
        return;
    for (auto it = node.begin(); it != node.end(); ++it) {
        // :276 means "any element type" (its comment), so the bogus key goes:
        // `items: {}` accepts anything, as the generator intends.
        if (it.key() == "items" && it->is_object() && it->contains("type") && (*it)["type"].is_boolean())
            it->erase("type");
        RepairItems(*it);
    }
}

void RepairAstraBoolLiterals(nlohmann::json& schema) {
    if (!schema.is_object())
        return;
    if (schema.contains("$schema") && schema["$schema"].is_boolean())
        schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";   // JsonSchema.hpp:50
    if (schema.contains("type") && schema["type"].is_boolean())
        schema["type"] = "object";                                            // JsonSchema.hpp:56
    RepairItems(schema);
}

}  // namespace

nlohmann::json Arcane::Remote::SchemaFor(const Astra::TypeMeta& meta)
{
    Astra::JsonSchemaGenerator gen;
    nlohmann::json schema = nlohmann::json::parse(gen.Generate(meta));
    RepairAstraBoolLiterals(schema);
    ApplySchemaAdapter(schema, meta);
    if (meta.isDefaultConstructible && meta.defaultConstruct)
    {
        void* instance = ::operator new(meta.size, std::align_val_t{meta.alignment});
        meta.defaultConstruct(instance);
        // The defaults go through the Protected walk resource.get and
        // entity.get use: a secret's default is masked, never stamped
        // (final fix pass, C-M8).
        nlohmann::json written = nlohmann::json::object();
        std::string why;
        (void)Arcane::Remote::Detail::WriteMaskedFields(meta, instance, written, why, /*skipHidden=*/true);
        if (meta.destruct)
            meta.destruct(instance);
        ::operator delete(instance, std::align_val_t{meta.alignment});

        auto stamp = [&](auto&& self, nlohmann::json& node, const nlohmann::json& value) -> void {
            if (!node.contains("properties") || !value.is_object())
                return;
            for (auto it = node["properties"].begin(); it != node["properties"].end(); ++it)
            {
                if (!value.contains(it.key()))
                    continue;
                (*it)["default"] = value[it.key()];
                if (!it->contains("$ref") || !value[it.key()].is_object() || !schema.contains("$defs"))
                    continue;
                const std::string ref = (*it)["$ref"].get<std::string>();
                const auto slash = ref.rfind('/');
                if (slash == std::string::npos)
                    continue;
                const std::string defName = ref.substr(slash + 1);
                if (schema["$defs"].contains(defName))
                    self(self, schema["$defs"][defName], value[it.key()]);
            }
        };
        stamp(stamp, schema, written);
    }
    if (schema.contains("properties") && schema["properties"].is_object()) {
        for (const Astra::FieldInfo& field : meta.fields) {
            if (field.IsHidden() || !IsGlmWire(field.typeHash))
                continue;
            const std::string fname(field.name);
            if (!schema["properties"].contains(fname) || !schema["properties"][fname].is_object())
                continue;
            auto& prop = schema["properties"][fname];
            nlohmann::json keptDefault = prop.contains("default") ? prop["default"] : nlohmann::json();
            const int n = GlmItemCount(field.typeHash);
            prop = nlohmann::json{{"type", "array"}, {"minItems", n}, {"maxItems", n},
                                  {"items", {{"type", "number"}}}};
            if (!keptDefault.is_null())
                prop["default"] = std::move(keptDefault);
        }
    }
    return schema;
}

#endif
