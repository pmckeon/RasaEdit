// The UI workspace's Definitions panel: Colors, Fonts, Textures and Animations of the UI export, edited with the game's rules
// (uischema: mandatory fields, value forms, unique ids, references from the widget trees).
#include "UiWorkspace.h"

#include "GpuTexture.h"

#include "imgui.h"
#include "imgui_internal.h"   // FindWindowByName

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>

namespace {

const tr::UiSection kSections[] = {tr::UiSection::Colours, tr::UiSection::Textures, tr::UiSection::Fonts, tr::UiSection::Animations};

const char* sectionName(tr::UiSection s) {
    switch (s) {
        case tr::UiSection::Colours: return "Colors";
        case tr::UiSection::Fonts: return "Fonts";
        case tr::UiSection::Textures: return "Textures";
        case tr::UiSection::Animations: return "Animations";
        default: return "";
    }
}

const char* sectionTitle(tr::UiSection s) {
    switch (s) {
        case tr::UiSection::Colours: return "Colours";
        case tr::UiSection::Fonts: return "Fonts";
        case tr::UiSection::Textures: return "Textures";
        case tr::UiSection::Animations: return "Animations";
        default: return "";
    }
}

// The element each entry of a section is.
const char* entryTag(tr::UiSection s) {
    switch (s) {
        case tr::UiSection::Colours: return "Color";
        case tr::UiSection::Fonts: return "MultiFont";
        case tr::UiSection::Textures: return "textureDef";
        case tr::UiSection::Animations: return "Animation";
        default: return "";
    }
}

// The order an Animation's children are written in (the export's).
int animationChildOrder(const std::string& tag) {
    static const char* order[] = {"id", "playbackType", "animType", "uvTrack", "rotTrack", "textureTrack", "scaleTrack", "transTrack", "colorTrack"};
    for (int k = 0; k < 9; ++k)
        if (tag == order[k]) return k;
    return 9;
}

// Where a new child with that tag goes among an Animation's children.
size_t animationInsertIndex(const tr::XmlNode& animation, const std::string& tag) {
    const int order = animationChildOrder(tag);
    size_t index = 0;
    for (size_t k = 0; k < animation.children.size(); ++k)
        if (animationChildOrder(animation.children[k].tag) < order) index = k + 1;
    return index;
}

int32_t childIndex(const tr::XmlNode& n, const std::string& tag) {
    for (size_t k = 0; k < n.children.size(); ++k)
        if (n.children[k].tag == tag) return int32_t(k);
    return -1;
}

std::string pathKey(tr::UiSection section, const std::vector<int32_t>& path, const std::string& name) {
    std::string key = std::to_string(int(section));
    for (int32_t i : path) key += "/" + std::to_string(i);
    return key + ":" + name;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const ImVec4 kRefused(1.0f, 0.45f, 0.4f, 1.0f);

}  // namespace

// ------------------------------------------------------------------------------------------
// Edits
// ------------------------------------------------------------------------------------------
std::string UiWorkspace::editDefinition(const std::function<void()>& edit) {
    if (!doc_) return "no UI export loaded";
    if (validatedRevision_ != doc_->revision()) validateNow();
    std::multiset<std::string> before;
    for (const auto& i : issues_)
        if (i.error) before.insert(i.message);
    const size_t revision = doc_->revision();
    std::string refusal;
    doc_->beginGroup();
    try {
        edit();
    } catch (const std::exception& e) {
        refusal = e.what();
    }
    doc_->endGroup();
    if (doc_->revision() != revision) {
        syncWithDocument();
        if (refusal.empty()) {
            validateNow();
            for (const auto& i : issues_) {
                if (!i.error) continue;
                const auto it = before.find(i.message);
                if (it != before.end()) before.erase(it);
                else if (refusal.empty()) refusal = i.message;
            }
        }
        if (!refusal.empty()) {                    // back as it was; the refused edit is not left to redo
            doc_->undo();
            doc_->dropRedo();
            syncWithDocument();
            validateNow();
        }
        lastEditTime_ = ImGui::GetTime();
    }
    if (!refusal.empty()) ctx_.setStatus("UI definitions: refused - " + refusal, true);
    return refusal;
}

std::string UiWorkspace::definitionId(tr::UiSection section, size_t index) const {
    const tr::XmlNode* n = doc_ ? doc_->node(section, {int32_t(index)}) : nullptr;
    if (!n) return "";
    if (section == tr::UiSection::Animations) {
        const tr::XmlNode* id = n->child("id");
        return id ? id->text : "";
    }
    return n->attribute("id");
}

std::vector<UiWorkspace::Reference> UiWorkspace::findReferences(tr::UiSection section, const std::string& id) const {
    std::vector<Reference> out;
    if (!doc_ || id.empty()) return out;
    const auto names = [&](uischema::Type t, const std::string& v) {
        switch (section) {
            case tr::UiSection::Textures: return t == uischema::Type::TextureDef && v == id;
            case tr::UiSection::Fonts: return t == uischema::Type::Font && (v == id || "Fonts." + v == id);
            case tr::UiSection::Animations: return (t == uischema::Type::WidgetAnim || t == uischema::Type::BitmapAnim) && v == id;
            default: return false;           // colours: no layout attribute names one
        }
    };
    std::vector<int32_t> path;
    std::function<void(const tr::XmlNode&, bool)> visit = [&](const tr::XmlNode& n, bool templates) {
        const std::string cls = uischema::elementClass(n.tag);
        for (const auto& [name, value] : n.attributes)
            if (const uischema::Attribute* a = uischema::attribute(cls, name); a && names(a->type, value)) out.push_back({templates, path, name});
        for (size_t k = 0; k < n.children.size(); ++k) {
            path.push_back(int32_t(k));
            visit(n.children[k], templates);
            path.pop_back();
        }
    };
    for (bool templates : {false, true}) {
        const tr::XmlNode* root = doc_->node(templates, {});
        if (!root) continue;
        for (size_t k = 0; k < root->children.size(); ++k) {
            path.assign(1, int32_t(k));
            visit(root->children[k], templates);
        }
    }
    return out;
}

std::string UiWorkspace::referenceLabel(const Reference& r) const {
    std::string s = r.templates ? "Templates" : "Widgets";
    const tr::XmlNode* n = doc_ ? doc_->node(r.templates, {}) : nullptr;
    for (int32_t i : r.path) {
        if (!n || i < 0 || size_t(i) >= n->children.size()) break;
        n = &n->children[size_t(i)];
        s += "/" + (n->hasAttribute("uiid") ? n->attribute("uiid") : n->tag);
    }
    return s + " (" + r.attribute + ")";
}

std::string UiWorkspace::renameDefinition(tr::UiSection section, size_t index, const std::string& newId, bool updateReferences) {
    const tr::XmlNode* n = doc_ ? doc_->node(section, {int32_t(index)}) : nullptr;
    if (!n) return "no such entry";
    const std::string old = definitionId(section, index);
    if (newId == old) return "";
    const auto refuse = [&](const std::string& why) {
        ctx_.setStatus("UI definitions: refused - " + why, true);
        return why;
    };
    if (newId.empty()) return refuse(std::string("a ") + entryTag(section) + " needs an id (without one the UI fails to load)");
    if (section == tr::UiSection::Fonts && (newId.rfind("Fonts.", 0) != 0 || newId.size() <= 6))
        return refuse("a MultiFont id is \"Fonts.<name>\" (the loader drops the first 6 characters)");
    const tr::XmlNode* root = doc_->node(section, {});
    for (size_t k = 0; k < root->children.size(); ++k)
        if (k != index && root->children[k].tag == entryTag(section) && definitionId(section, k) == newId)
            return refuse(std::string("there is already a ") + entryTag(section) + " \"" + newId + "\"");
    std::vector<Reference> refs;
    if (updateReferences) refs = findReferences(section, old);
    const std::string refusal = editDefinition([&] {
        if (section == tr::UiSection::Animations) {
            const tr::XmlNode* a = doc_->node(section, {int32_t(index)});
            const int32_t id = childIndex(*a, "id");
            if (id >= 0) doc_->setText(section, {int32_t(index), id}, newId);
            else {
                tr::XmlNode e;
                e.tag = "id";
                e.text = newId;
                doc_->insertElement(section, {int32_t(index)}, 0, e);
            }
        } else doc_->setAttribute(section, {int32_t(index)}, "id", newId);
        for (const Reference& r : refs) {
            const tr::XmlNode* w = doc_->node(r.templates, r.path);
            if (!w) continue;
            // A font named without "Fonts." stays so.
            const bool shortFont = section == tr::UiSection::Fonts && w->attribute(r.attribute).rfind("Fonts.", 0) != 0;
            doc_->setAttribute(r.templates, r.path, r.attribute, shortFont ? newId.substr(6) : newId);
        }
    });
    if (refusal.empty() && !refs.empty())
        ctx_.setStatus("Renamed " + old + " to " + newId + " and " + std::to_string(refs.size()) + " reference(s).", false);
    return refusal;
}

std::string UiWorkspace::setDefinitionAttribute(tr::UiSection section, const std::vector<int32_t>& path, const std::string& name,
                                                const std::string& value) {
    const tr::XmlNode* n = doc_ ? doc_->node(section, path) : nullptr;
    if (!n || path.empty()) return "no such entry";
    const auto refuse = [&](const std::string& why) {
        ctx_.setStatus("UI definitions: refused - " + why, true);
        return why;
    };
    const uischema::Attribute* a = uischema::definitionAttribute(n->tag, name);
    if (!a) return refuse("the game does not read " + name + " on a " + n->tag);
    if (name == "id" && path.size() == 1) return renameDefinition(section, size_t(path[0]), value, defRenameReferences_);
    if (value.empty() && a->defaultValue.empty()) return refuse(n->tag + " " + name + " is required (no default)");
    if (!value.empty()) {
        const std::string why = uischema::checkValue(*a, value, doc_->ui(), ctx_.assets.get(), ctx_.gameData.get());
        if (!why.empty()) return refuse(name + " \"" + value + "\": " + why);
    }
    // Booleans at false are left out, as the export writes them; other values are written as given (the export writes the
    // definitions' defaults - modes "stretch", regions - explicitly).
    const bool remove = value.empty() || (a->type == uischema::Type::Bool && value == "false" && a->defaultValue == "false");
    if (remove && !n->hasAttribute(name)) return "";
    if (!remove && n->hasAttribute(name) && n->attribute(name) == value) return "";
    return editDefinition([&] {
        if (remove) doc_->removeAttribute(section, path, name);
        else doc_->setAttribute(section, path, name, value);
    });
}

std::string UiWorkspace::setDefinitionText(tr::UiSection section, const std::vector<int32_t>& path, const std::string& value) {
    const tr::XmlNode* n = doc_ ? doc_->node(section, path) : nullptr;
    if (!n || path.empty()) return "no such entry";
    if (n->text == value) return "";
    return editDefinition([&] { doc_->setText(section, path, value); });
}

std::string UiWorkspace::setAnimationField(size_t index, const std::string& field, const std::string& value) {
    if (field == "id") return renameDefinition(tr::UiSection::Animations, index, value, defRenameReferences_);
    const tr::XmlNode* a = doc_ ? doc_->node(tr::UiSection::Animations, {int32_t(index)}) : nullptr;
    if (!a) return "no such animation";
    const uischema::Attribute* spec = uischema::definitionAttribute("Animation", field);
    if (!spec) return "not an Animation field";
    const std::string why = value.empty() ? "a value is required (an empty " + field + " fails the UI load)"
                                          : uischema::checkValue(*spec, value, doc_->ui(), ctx_.assets.get(), ctx_.gameData.get());
    if (!why.empty()) {
        ctx_.setStatus("UI definitions: refused - " + field + ": " + why, true);
        return why;
    }
    const tr::XmlNode* current = a->child(field);
    if (current && current->text == value) return "";
    const std::vector<int32_t> at{int32_t(index)};
    return editDefinition([&] {
        const tr::XmlNode* an = doc_->node(tr::UiSection::Animations, at);
        const int32_t c = childIndex(*an, field);
        if (c >= 0) doc_->setText(tr::UiSection::Animations, {int32_t(index), c}, value);
        else {
            tr::XmlNode e;
            e.tag = field;
            e.text = value;
            doc_->insertElement(tr::UiSection::Animations, at, animationInsertIndex(*an, field), e);
        }
        if (field != "animType") return;
        // The other kind's tracks go; the new kind's are added empty, as the export writes them.
        const std::vector<std::string>& keep = uischema::animationTracks(value);
        for (int32_t k = int32_t(doc_->node(tr::UiSection::Animations, at)->children.size()) - 1; k >= 0; --k) {
            const std::string tag = doc_->node(tr::UiSection::Animations, at)->children[size_t(k)].tag;
            const bool track = !uischema::frameTag(tag).empty() || tag == "textureTrack";
            if (track && std::find(keep.begin(), keep.end(), tag) == keep.end()) doc_->removeElement(tr::UiSection::Animations, {int32_t(index), k});
        }
        for (const std::string& track : keep) {
            const tr::XmlNode* now = doc_->node(tr::UiSection::Animations, at);
            if (childIndex(*now, track) >= 0) continue;
            tr::XmlNode e;
            e.tag = track;
            doc_->insertElement(tr::UiSection::Animations, at, animationInsertIndex(*now, track), e);
        }
    });
}

std::string UiWorkspace::addAnimationTrack(size_t index, const std::string& track) {
    const tr::XmlNode* a = doc_ ? doc_->node(tr::UiSection::Animations, {int32_t(index)}) : nullptr;
    if (!a || childIndex(*a, track) >= 0) return "";
    tr::XmlNode e;
    e.tag = track;
    const size_t at = animationInsertIndex(*a, track);
    return editDefinition([&] { doc_->insertElement(tr::UiSection::Animations, {int32_t(index)}, at, e); });
}

int32_t UiWorkspace::addDefinition(tr::UiSection section, bool copySelected) {
    const tr::XmlNode* root = doc_ ? doc_->node(section, {}) : nullptr;
    if (!root) {
        ctx_.setStatus(std::string("UI definitions: the file has no Definitions/") + sectionName(section), true);
        return -1;
    }
    const std::string tag = entryTag(section);
    tr::XmlNode e;
    std::string base;
    size_t index = root->children.size();
    const tr::XmlNode* selected = defSection_ == section && defIndex_ >= 0 ? doc_->node(section, {defIndex_}) : nullptr;
    if (copySelected && selected && selected->tag == tag) {
        e = *selected;
        base = definitionId(section, size_t(defIndex_)) + "_copy";
        index = size_t(defIndex_) + 1;
    } else {
        e.tag = tag;
        switch (section) {
            case tr::UiSection::Colours:
                base = "NewColor";
                e.attributes = {{"id", ""}, {"R", "255"}, {"G", "255"}, {"B", "255"}, {"A", "255"}};
                break;
            case tr::UiSection::Textures: {
                // A file is mandatory: the selected definition's, else the first one's.
                std::string file = selected ? selected->attribute("name") : "";
                for (const tr::XmlNode& t : root->children)
                    if (file.empty() && t.tag == "textureDef") file = t.attribute("name");
                base = "NewTexture";
                e.attributes = {{"id", ""}, {"name", file}, {"modeU", "stretch"}, {"modeV", "stretch"}};
                break;
            }
            case tr::UiSection::Fonts: {
                base = "Fonts.NewFont";
                e.attributes = {{"id", ""}};
                tr::XmlNode range;
                range.tag = "FontRange";
                range.attributes = {{"RangeBegin", "0"}, {"RangeEnd", "65535"}, {"faceName", "Conduit ITC Light"}, {"pointSize", "10"}};
                e.children.push_back(range);
                break;
            }
            case tr::UiSection::Animations: {
                base = "NewAnimation";
                for (const char* field : {"id", "playbackType", "animType"}) {
                    tr::XmlNode f;
                    f.tag = field;
                    f.text = std::string(field) == "playbackType" ? "stall" : std::string(field) == "animType" ? "Widget" : "";
                    e.children.push_back(f);
                }
                for (const std::string& track : uischema::animationTracks("Widget")) {
                    tr::XmlNode t;
                    t.tag = track;
                    e.children.push_back(t);
                }
                break;
            }
            default: return -1;
        }
    }
    // A unique id.
    std::string id = base;
    for (int n = 2;; ++n) {
        bool taken = false;
        for (size_t k = 0; k < root->children.size() && !taken; ++k) taken = root->children[k].tag == tag && definitionId(section, k) == id;
        if (!taken) break;
        id = base + std::to_string(n);
    }
    if (section == tr::UiSection::Animations) {
        for (tr::XmlNode& c : e.children)
            if (c.tag == "id") c.text = id;
    } else {
        for (auto& [name, value] : e.attributes)
            if (name == "id") value = id;
    }
    const std::string refusal = editDefinition([&] { doc_->insertElement(section, {}, index, e); });
    if (!refusal.empty()) return -1;
    selectDefinition(section, int32_t(index));
    return int32_t(index);
}

std::string UiWorkspace::removeDefinition(tr::UiSection section, size_t index) {
    const tr::XmlNode* n = doc_ ? doc_->node(section, {int32_t(index)}) : nullptr;
    if (!n) return "no such entry";
    const std::string id = definitionId(section, index);
    const std::vector<Reference> refs = findReferences(section, id);
    if (!refs.empty()) {
        const std::string why = id + " is used by " + std::to_string(refs.size()) + " element(s), e.g. " + referenceLabel(refs.front());
        ctx_.setStatus("UI definitions: refused - " + why, true);
        return why;
    }
    const std::string refusal = editDefinition([&] { doc_->removeElement(section, {int32_t(index)}); });
    if (refusal.empty() && defSection_ == section) {
        const tr::XmlNode* root = doc_->node(section, {});
        defIndex_ = std::min<int32_t>(int32_t(index), int32_t(root->children.size()) - 1);
        defBuffers_.clear();
    }
    return refusal;
}

std::string UiWorkspace::addDefinitionChild(tr::UiSection section, const std::vector<int32_t>& parent) {
    const tr::XmlNode* p = doc_ ? doc_->node(section, parent) : nullptr;
    if (!p) return "no such entry";
    tr::XmlNode e;
    if (p->tag == "MultiFont") {
        // A copy of the last range (all four mandatory attributes), else a basic one.
        if (const tr::XmlNode* last = p->children.empty() ? nullptr : &p->children.back(); last && last->tag == "FontRange") e = *last;
        else {
            e.tag = "FontRange";
            e.attributes = {{"RangeBegin", "0"}, {"RangeEnd", "65535"}, {"faceName", "Conduit ITC Light"}, {"pointSize", "10"}};
        }
    } else if (const std::string frame = p->tag == "textureTrack" ? std::string() : uischema::frameTag(p->tag); !frame.empty()) {
        // After the last frame (time + 100 ms, its values), else at time 0 with neutral values.
        if (!p->children.empty()) {
            e = p->children.back();
            const long t = std::strtol(e.attribute("time").c_str(), nullptr, 10) + 100;
            for (auto& [name, value] : e.attributes)
                if (name == "time") value = std::to_string(t);
        } else {
            e.tag = frame;
            e.attributes = {{"time", "0"}};
            if (frame == "uvFrame") e.attributes.insert(e.attributes.end(), {{"X", "0.000000"}, {"Y", "0.000000"}, {"Z", "1.000000"}, {"W", "1.000000"}});
            else if (frame == "rotFrame") e.attributes.push_back({"rot", "0.000000"});
            else if (frame == "scaleFrame") e.attributes.insert(e.attributes.end(), {{"X", "1.000000"}, {"Y", "1.000000"}});
            else if (frame == "transFrame") e.attributes.insert(e.attributes.end(), {{"X", "0.000000"}, {"Y", "0.000000"}});
            else e.attributes.insert(e.attributes.end(), {{"R", "255"}, {"G", "255"}, {"B", "255"}, {"A", "255"}});
        }
    } else return "no child to add here";
    const size_t at = p->children.size();
    return editDefinition([&] { doc_->insertElement(section, parent, at, e); });
}

std::string UiWorkspace::removeDefinitionChild(tr::UiSection section, const std::vector<int32_t>& path) {
    const tr::XmlNode* n = doc_ ? doc_->node(section, path) : nullptr;
    if (!n || path.size() < 2) return "no such entry";
    const tr::XmlNode* parent = doc_->node(section, std::vector<int32_t>(path.begin(), path.end() - 1));
    if (n->tag == "FontRange" && parent) {
        size_t ranges = 0;
        for (const tr::XmlNode& c : parent->children) ranges += c.tag == "FontRange" ? 1 : 0;
        if (ranges <= 1) {
            ctx_.setStatus("UI definitions: refused - a font keeps at least one range", true);
            return "a font keeps at least one range";
        }
    }
    return editDefinition([&] { doc_->removeElement(section, path); });
}

void UiWorkspace::selectDefinition(tr::UiSection section, int32_t index) {
    if (defSection_ != section || defIndex_ != index) defBuffers_.clear();
    defSection_ = section;
    defIndex_ = index;
}

// ------------------------------------------------------------------------------------------
// Panel
// ------------------------------------------------------------------------------------------
void UiWorkspace::updateDefinitionPreview() {
    if (previewWanted_ == previewLoaded_) return;
    if (previewTexture_) SDL_ReleaseGPUTexture(ctx_.device, previewTexture_);
    previewTexture_ = nullptr;
    previewWidth_ = previewHeight_ = 0;
    previewLoaded_ = previewWanted_;
    previewZoom_ = 0.0f;                       // another file opens fitted
    previewScroll_ = ImVec2(0, 0);
    previewScrollPending_ = true;
    regionDragging_ = false;
    if (previewWanted_.empty() || !ctx_.assets || !ctx_.assets->exists(previewWanted_)) return;
    try {
        const tr::Bytes dds = ctx_.assets->read(previewWanted_);
        if (dds.size() < 20) return;
        const auto u32 = [&](size_t at) { return uint32_t(dds[at]) | uint32_t(dds[at + 1]) << 8 | uint32_t(dds[at + 2]) << 16 | uint32_t(dds[at + 3]) << 24; };
        previewHeight_ = int32_t(u32(12));
        previewWidth_ = int32_t(u32(16));
        previewTexture_ = uploadDdsTexture(ctx_.device, dds);
    } catch (const std::exception&) {
        previewTexture_ = nullptr;
    }
}

void UiWorkspace::definitionField(tr::UiSection section, const std::vector<int32_t>& path, const std::string& name) {
    const tr::XmlNode* n = doc_->node(section, path);
    if (!n) return;
    const uischema::Attribute* a = uischema::definitionAttribute(n->tag, name);
    if (!a) return;
    ImGui::PushID(name.c_str());
    const bool present = n->hasAttribute(name);
    const std::string value = present ? n->attribute(name) : a->defaultValue;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (a->type == uischema::Type::Bool) {
        bool v = value == "true";
        if (ImGui::Checkbox("##v", &v)) setDefinitionAttribute(section, path, name, v ? "true" : "false");
    } else if (a->type == uischema::Type::Enum) {
        if (ImGui::BeginCombo("##v", value.empty() ? "(not set)" : value.c_str())) {
            for (const std::string& option : a->values)
                if (ImGui::Selectable(option.c_str(), option == value)) setDefinitionAttribute(section, path, name, option);
            ImGui::EndCombo();
        }
    } else {
        // Written when the field is left (Enter or focus elsewhere); a refused value springs back.
        const std::string key = pathKey(section, path, name);
        auto it = defBuffers_.find(key);
        if (it == defBuffers_.end()) it = defBuffers_.emplace(key, present ? value : "").first;
        char buffer[512] = {};
        std::snprintf(buffer, sizeof buffer, "%s", it->second.c_str());
        const char* hint = a->defaultValue.empty() ? "required" : a->defaultValue.c_str();
        if (ImGui::InputTextWithHint("##v", hint, buffer, sizeof buffer)) it->second = buffer;
        const bool active = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            const std::string v = it->second;
            defBuffers_.erase(it);
            setDefinitionAttribute(section, path, name, v);
        } else if (!active && it->second != (present ? value : "")) it->second = present ? value : "";
    }
    if (ImGui::IsItemHovered() && a->type != uischema::Type::Bool)
        ImGui::SetTooltip("%s - default: %s", name.c_str(), a->defaultValue.empty() ? "none (required)" : a->defaultValue.c_str());
    ImGui::PopID();
}

void UiWorkspace::drawReferences(tr::UiSection section, const std::string& id) {
    if (section == tr::UiSection::Colours) {
        ImGui::TextDisabled("No layout attribute names a colour definition (scripts may: renaming can break one).");
        return;
    }
    if (referencesRevision_ != doc_->revision() || referencesSection_ != section || referencesId_ != id) {
        references_ = findReferences(section, id);
        referencesRevision_ = doc_->revision();
        referencesSection_ = section;
        referencesId_ = id;
    }
    if (references_.empty()) {
        ImGui::TextDisabled("Not used by any widget or template.");
        return;
    }
    if (!ImGui::TreeNode("refs", "Used by %zu element(s)", references_.size())) return;
    size_t shown = 0;
    for (const Reference& r : references_) {
        if (++shown > 200) {
            ImGui::TextDisabled("(%zu more)", references_.size() - 200);
            break;
        }
        ImGui::PushID(int(shown));
        if (ImGui::Selectable(referenceLabel(r).c_str())) {
            if (canvas_.templates() != r.templates) {
                canvas_.setTemplates(r.templates);
                syncWithDocument();
            }
            select(branchFor(r.path), true, false);
            select(r.path, false, true);
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

void UiWorkspace::drawDefinitionsPanel() {
    if (focusDefinitions_) ImGui::SetWindowFocus("Definitions###UI.Definitions");
    if (!ImGui::Begin("Definitions###UI.Definitions")) {
        ImGui::End();
        return;
    }
    if (!doc_) {
        ImGui::TextDisabled("No UI export loaded.");
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("sections")) {
        for (tr::UiSection s : kSections) {
            const ImGuiTabItemFlags flags = focusDefinitions_ && s == defSection_ ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(sectionTitle(s), nullptr, flags)) continue;
            if (focusDefinitions_ && s == defSection_) focusDefinitions_ = false;
            if (!focusDefinitions_ && s != defSection_) selectDefinition(s, -1);
            drawDefinitionList(s);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void UiWorkspace::drawDefinitionList(tr::UiSection section) {
    const tr::XmlNode* root = doc_->node(section, {});
    if (!root) {
        ImGui::TextColored(kRefused, "The file has no Definitions/%s.", sectionName(section));
        return;
    }
    const std::string tag = entryTag(section);
    if (section == tr::UiSection::Fonts && ImGui::CollapsingHeader("Loaded font files")) {
        // LoadedFontFile: each just registers a font file (TrueType, data\misc).
        for (size_t k = 0; k < root->children.size(); ++k) {
            if (root->children[k].tag != "LoadedFontFile") continue;
            ImGui::PushID(int(k));
            const std::string key = pathKey(section, {int32_t(k)}, "#text");
            auto it = defBuffers_.find(key);
            if (it == defBuffers_.end()) it = defBuffers_.emplace(key, root->children[k].text).first;
            char buffer[256] = {};
            std::snprintf(buffer, sizeof buffer, "%s", it->second.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##file", buffer, sizeof buffer)) it->second = buffer;
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                const std::string v = it->second;
                defBuffers_.erase(it);
                setDefinitionText(section, {int32_t(k)}, v);
            }
            ImGui::PopID();
            root = doc_->node(section, {});
        }
    }
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - 3 * 70.0f));
    ImGui::InputTextWithHint("##filter", "filter by id", defFilter_, sizeof defFilter_);
    ImGui::SameLine();
    if (ImGui::Button("Add")) addDefinition(section, false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A new %s with its required fields filled", tag.c_str());
    ImGui::SameLine();
    const bool hasEntry = defIndex_ >= 0 && doc_->node(section, {defIndex_}) && doc_->node(section, {defIndex_})->tag == tag;
    ImGui::BeginDisabled(!hasEntry);
    if (ImGui::Button("Copy")) addDefinition(section, true);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Duplicate the selected entry under a new id");
    ImGui::SameLine();
    if (ImGui::Button("Delete") && hasEntry) removeDefinition(section, size_t(defIndex_));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Refused while a widget or template uses it");
    ImGui::EndDisabled();
    root = doc_->node(section, {});
    // The entries, filtered.
    const std::string filter = lower(defFilter_);
    std::vector<int32_t> shown;
    for (size_t k = 0; k < root->children.size(); ++k)
        if (root->children[k].tag == tag && (filter.empty() || lower(definitionId(section, k)).find(filter) != std::string::npos)) shown.push_back(int32_t(k));
    const float listHeight = std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.35f);
    ImGui::BeginChild("entries", ImVec2(0, listHeight), ImGuiChildFlags_Borders);
    ImGuiListClipper clipper;
    clipper.Begin(int(shown.size()));
    const auto selectedRow = std::find(shown.begin(), shown.end(), defIndex_);
    if (defScroll_ && selectedRow != shown.end()) clipper.IncludeItemByIndex(int(selectedRow - shown.begin()));
    while (clipper.Step())
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const int32_t k = shown[size_t(row)];
            const tr::XmlNode& e = root->children[size_t(k)];
            if (defScroll_ && k == defIndex_) {
                ImGui::SetScrollHereY(0.3f);
                defScroll_ = false;
            }
            ImGui::PushID(k);
            if (section == tr::UiSection::Colours) {
                const auto channel = [&](const char* c) { return float(e.hasAttribute(c) ? std::strtol(e.attribute(c).c_str(), nullptr, 10) : 255) / 255.0f; };
                ImGui::ColorButton("##swatch", ImVec4(channel("R"), channel("G"), channel("B"), channel("A")),
                                   ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
                ImGui::SameLine();
            }
            std::string label = definitionId(section, size_t(k));
            if (label.empty()) label = "(no id)";
            if (ImGui::Selectable(label.c_str(), k == defIndex_)) selectDefinition(section, k);
            if (section == tr::UiSection::Textures || section == tr::UiSection::Animations) {
                // The file / kind beside the id when there is room, else in a tooltip.
                const tr::XmlNode* type = e.child("animType");
                const std::string note = section == tr::UiSection::Textures ? e.attribute("name") : type ? type->text : "";
                const bool hovered = ImGui::IsItemHovered();
                const float column = std::max(ImGui::CalcTextSize(label.c_str()).x + 16.0f, ImGui::GetWindowWidth() * 0.55f);
                if (column + ImGui::CalcTextSize(note.c_str()).x < ImGui::GetWindowWidth() - 8.0f) {
                    ImGui::SameLine(column);
                    ImGui::TextDisabled("%s", note.c_str());
                } else if (hovered) ImGui::SetTooltip("%s", note.c_str());
            }
            ImGui::PopID();
        }
    ImGui::EndChild();
    ImGui::Checkbox("Renaming updates the widgets that use it", &defRenameReferences_);
    ImGui::Separator();
    if (!hasEntry) {
        if (section == tr::UiSection::Textures) previewWanted_.clear();
        ImGui::TextDisabled("Choose an entry.");
        return;
    }
    // This entry's problems.
    for (const auto& i : issues_)
        if (i.section == sectionName(section) && !i.path.empty() && i.path[0] == defIndex_)
            ImGui::TextColored(i.error ? kRefused : ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s %s", i.error ? "Error:" : "Warning:", i.message.c_str());
    ImGui::BeginChild("editor");
    const size_t index = size_t(defIndex_);
    switch (section) {
        case tr::UiSection::Colours: drawColourEditor(index); break;
        case tr::UiSection::Textures: drawTextureEditor(index); break;
        case tr::UiSection::Fonts: drawFontEditor(index); break;
        case tr::UiSection::Animations: drawAnimationEditor(index); break;
        default: break;
    }
    if (doc_->node(section, {int32_t(index)})) drawReferences(section, definitionId(section, index));
    ImGui::EndChild();
}

namespace {

// A two-column property row: label, then the caller's field.
void rowLabel(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
}

bool beginProperties(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return false;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.35f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.65f);
    return true;
}

}  // namespace

// ------------------------------------------------------------------------------------------
// Previews: colours, fonts, animations
// ------------------------------------------------------------------------------------------
namespace {

const float kSampleScales[3] = {1.0f, 1.125f, 1.5f};
const char* kSampleScaleNames[3] = {"1280 x 960 (1 pixel per unit)", "1080 lines (x 1.125)", "1440 lines (x 1.5)"};

void setAttr(tr::XmlNode& n, const std::string& name, const std::string& value) {
    for (auto& a : n.attributes)
        if (a.first == name) {
            a.second = value;
            return;
        }
    n.attributes.emplace_back(name, value);
}

void eraseAttr(tr::XmlNode& n, const std::string& name) {
    n.attributes.erase(std::remove_if(n.attributes.begin(), n.attributes.end(), [&](const auto& a) { return a.first == name; }), n.attributes.end());
}

int32_t intAttr(const tr::XmlNode& n, const char* name, int32_t fallback) {
    return n.hasAttribute(name) ? int32_t(std::strtol(n.attribute(name).c_str(), nullptr, 10)) : fallback;
}

}  // namespace

void UiWorkspace::updateSamplePreview() {
    SampleRequest wanted = std::move(sampleWanted_);
    sampleWanted_ = SampleRequest{};
    if (wanted.key.empty() || wanted.key == sampleLoaded_ || !doc_) return;
    UiCanvas::Options options;
    options.background = 2;
    options.animTime = wanted.animTime;
    std::vector<uint8_t> rgba;
    sampleCanvas_.renderDetached(wanted.widget, wanted.width, wanted.height, wanted.scale, options, wanted.text, rgba);
    // Replaced between frames: no draw list of this frame references the old texture yet.
    if (sampleTexture_) SDL_ReleaseGPUTexture(ctx_.device, sampleTexture_);
    sampleTexture_ = uploadRgbaTexture(ctx_.device, rgba.data(), uint32_t(sampleCanvas_.pixelWidth()), uint32_t(sampleCanvas_.pixelHeight()));
    sampleWidth_ = sampleCanvas_.pixelWidth();
    sampleHeight_ = sampleCanvas_.pixelHeight();
    sampleLoaded_ = wanted.key;
}

void UiWorkspace::showSample(const std::string& key) {
    // The texture shows the last request drawn; another entry's is not shown (its own is drawn between this frame and the next).
    const auto stem = [](const std::string& k) { return k.substr(0, k.find('|')); };
    if (!sampleTexture_ || sampleWidth_ <= 0 || stem(sampleLoaded_) != stem(key)) {
        ImGui::Dummy(ImVec2(1.0f, float(std::max(sampleWanted_.height, 16))));
        return;
    }
    const float avail = std::max(ImGui::GetContentRegionAvail().x, 16.0f);
    const float f = std::min(1.0f, avail / float(sampleWidth_));
    ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)sampleTexture_), ImVec2(float(sampleWidth_) * f, float(sampleHeight_) * f));
    if (f < 1.0f && ImGui::IsItemHovered()) ImGui::SetTooltip("Shown at %d %% (the panel is narrower than the sample)", int(f * 100.0f));
}

// A colour definition with its alpha over a checkerboard, beside the colour opaque.
void UiWorkspace::drawColourSwatch(const float rgba[4]) {
    const float w = std::max(ImGui::GetContentRegionAvail().x, 64.0f), h = ImGui::GetFrameHeight() * 2.5f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float split = std::floor(w * 0.7f), cell = 8.0f;
    for (float y = 0; y < h; y += cell)
        for (float x = 0; x < split; x += cell) {
            const bool odd = ((int(x / cell) + int(y / cell)) & 1) != 0;
            dl->AddRectFilled(ImVec2(p.x + x, p.y + y), ImVec2(p.x + std::min(x + cell, split), p.y + std::min(y + cell, h)),
                              odd ? IM_COL32(204, 204, 204, 255) : IM_COL32(128, 128, 128, 255));
        }
    const ImU32 blended = ImGui::ColorConvertFloat4ToU32(ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]));
    const ImU32 opaque = ImGui::ColorConvertFloat4ToU32(ImVec4(rgba[0], rgba[1], rgba[2], 1.0f));
    dl->AddRectFilled(p, ImVec2(p.x + split, p.y + h), blended);
    dl->AddRectFilled(ImVec2(p.x + split, p.y), ImVec2(p.x + w, p.y + h), opaque);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::Dummy(ImVec2(w, h));
    const auto byte = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Left: with its alpha (%d) over a checkerboard. Right: opaque.\nR %d  G %d  B %d  A %d", byte(rgba[3]), byte(rgba[0]),
                          byte(rgba[1]), byte(rgba[2]), byte(rgba[3]));
}

// The font's sample text drawn the game's way (UiCanvas::drawText: the GDI-style glyphs, the font's ranges per character), in white,
// at a screen scale - the em follows the screen (round(trunc(pointSize x 4 / 3) x scale)).
void UiWorkspace::drawFontSample(size_t index) {
    const std::string id = definitionId(tr::UiSection::Fonts, index);
    ImGui::SeparatorText("Preview");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##sample", fontSampleText_, sizeof fontSampleText_);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##scale", &sampleScale_, kSampleScaleNames, 3);
    const float scale = kSampleScales[std::clamp(sampleScale_, 0, 2)];
    const int32_t pixelWidth = std::max(int32_t(ImGui::GetContentRegionAvail().x), 64);
    const tr::UiFont* font = ui() ? ui()->font(id) : nullptr;
    int32_t em = 16;
    if (font && !font->ranges.empty()) em = int32_t(std::lround(float(font->ranges.front().pointSize * 4 / 3) * scale));
    // Room for a few wrapped lines at the font's size.
    const int32_t pixelHeight = std::clamp(em * 4 + 8, 32, 400);
    SampleRequest& r = sampleWanted_;
    r.widget = tr::XmlNode{};
    r.widget.tag = "Text";
    setAttr(r.widget, "fontId", id);
    setAttr(r.widget, "wrapText", "true");
    setAttr(r.widget, "anchorOffsetX", "4");
    setAttr(r.widget, "anchorOffsetY", "4");
    setAttr(r.widget, "sizeW", std::to_string(int32_t(float(pixelWidth - 8) / scale)));
    setAttr(r.widget, "sizeH", std::to_string(int32_t(float(pixelHeight - 8) / scale)));
    r.width = pixelWidth;
    r.height = pixelHeight;
    r.scale = scale;
    r.animTime = -1.0f;
    r.text = fontSampleText_;
    r.key = "font:" + id + "|" + std::to_string(doc_->revision()) + "|" + std::to_string(pixelWidth) + "|" + std::to_string(sampleScale_) + "|" + r.text;
    showSample(r.key);
    if (!font) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "No font by this id.");
    } else if (!sampleCanvas_.missingFonts().empty()) {
        std::string faces;
        for (const std::string& f : sampleCanvas_.missingFonts()) faces += (faces.empty() ? "" : ", ") + f;
        ImGui::TextDisabled("Faces not found: %s", faces.c_str());
    }
}

// The animation played on a copy of an element that uses it (its own size and children, centred), else - Widget animations - on a
// plain white box. The clock is the preview's own: playing from when the entry was opened, or paused / scrubbed.
void UiWorkspace::drawAnimationSample(size_t index) {
    const std::string id = definitionId(tr::UiSection::Animations, index);
    const tr::XmlNode* n = doc_->node(tr::UiSection::Animations, {int32_t(index)});
    const tr::XmlNode* type = n ? n->child("animType") : nullptr;
    const bool bitmap = type && type->text == "Bitmap";
    ImGui::SeparatorText("Preview");
    if (animFor_ != id) {
        animFor_ = id;
        animStart_ = ImGui::GetTime();
        animPausedAt_ = 0.0f;
        animShownOn_ = 0;
    }
    if (referencesRevision_ != doc_->revision() || referencesSection_ != tr::UiSection::Animations || referencesId_ != id) {
        references_ = findReferences(tr::UiSection::Animations, id);
        referencesRevision_ = doc_->revision();
        referencesSection_ = tr::UiSection::Animations;
        referencesId_ = id;
    }
    const std::string attribute = bitmap ? "bitmapAnimName" : "widgetAnimName";
    std::vector<const Reference*> users;
    for (const Reference& ref : references_)
        if (ref.attribute == attribute) users.push_back(&ref);
    if (animShownOn_ >= int(users.size())) animShownOn_ = 0;
    const uint32_t length = sampleCanvas_.animationLength(id);
    const tr::XmlNode* playback = n ? n->child("playbackType") : nullptr;
    const bool stalls = !playback || playback->text.empty() || playback->text == "stall";
    float t = animPlaying_ ? float((ImGui::GetTime() - animStart_) * 1000.0) : animPausedAt_;
    // A stalling animation holds its last frame; the preview shows that for a second, then starts it again.
    if (animPlaying_ && stalls && length > 0) t = std::fmod(t, float(length) + 1000.0f);
    if (ImGui::SmallButton(animPlaying_ ? "Pause" : "Play")) {
        if (animPlaying_) animPausedAt_ = t;
        else animStart_ = ImGui::GetTime() - double(animPausedAt_) / 1000.0;
        animPlaying_ = !animPlaying_;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Restart")) {
        animStart_ = ImGui::GetTime();
        animPausedAt_ = 0.0f;
        t = 0.0f;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    float shown = t;
    const float sliderEnd = stalls ? float(length) + 1000.0f : float(std::max<uint32_t>(length, 1)) * 2.0f;
    if (ImGui::SliderFloat("##time", &shown, 0.0f, sliderEnd, "%.0f ms")) {
        animPlaying_ = false;
        animPausedAt_ = t = shown;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Length %u ms (the longest track's last key). Dragging pauses.%s", length,
                          stalls ? "\nstall: the preview holds the last frame for a second, then plays it again." : "");
    if (!users.empty()) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##on", ("On " + referenceLabel(*users[size_t(animShownOn_)])).c_str())) {
            for (size_t k = 0; k < users.size() && k < 200; ++k) {
                ImGui::PushID(int(k));
                if (ImGui::Selectable(referenceLabel(*users[k]).c_str(), int(k) == animShownOn_)) animShownOn_ = int(k);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    const tr::XmlNode* source = nullptr;
    if (!users.empty()) {
        const Reference& ref = *users[size_t(animShownOn_)];
        source = doc_->node(ref.templates ? tr::UiSection::Templates : tr::UiSection::Widgets, ref.path);
    }
    SampleRequest& r = sampleWanted_;
    if (source) {
        r.widget = *source;
    } else if (!bitmap) {
        r.widget = tr::XmlNode{};
        r.widget.tag = "ColoredRect";
        setAttr(r.widget, "sizeW", "96");
        setAttr(r.widget, "sizeH", "64");
    } else {
        ImGui::TextDisabled("No Bitmap uses it: nothing to play it on (a Bitmap animation moves its texture).");
        return;
    }
    // Centred in the preview, visible, at its own size (a widget sized by its parent gets 64 x 64).
    for (const char* a : {"anchorOffsetX", "anchorOffsetY", "resizeToParent", "visible"}) eraseAttr(r.widget, a);
    setAttr(r.widget, "childAnchor", "8");
    setAttr(r.widget, "parentAnchor", "8");
    setAttr(r.widget, attribute, id);
    const int32_t w = std::max(intAttr(r.widget, "sizeW", 64), 8), h = std::max(intAttr(r.widget, "sizeH", 64), 8);
    setAttr(r.widget, "sizeW", std::to_string(w));
    setAttr(r.widget, "sizeH", std::to_string(h));
    // Room around it for scale / translation; at most the panel's width and 320 pixels high.
    const float frameW = float(w) * 1.5f + 32.0f, frameH = float(h) * 1.5f + 32.0f;
    const float availW = std::max(ImGui::GetContentRegionAvail().x, 64.0f);
    const float scale = std::min({1.0f, availW / frameW, 320.0f / frameH});
    r.width = std::max(int32_t(frameW * scale), 16);
    r.height = std::max(int32_t(frameH * scale), 16);
    r.scale = scale;
    r.text.clear();
    // The playback rule (sampleAnimation) loops, mirrors or holds the last frame.
    r.animTime = length > 0 ? std::max(t, 0.0f) : -1.0f;
    r.key = "anim:" + id + "|" + std::to_string(doc_->revision()) + "|" + std::to_string(animShownOn_) + "|" + std::to_string(r.width) + "|" +
            std::to_string(int32_t(r.animTime));
    showSample(r.key);
    if (length == 0) ImGui::TextDisabled("No keys: nothing moves.");
    if (!source) ImGui::TextDisabled("Not used by any element: shown on a plain white box.");
}

void UiWorkspace::drawColourEditor(size_t index) {
    const std::vector<int32_t> path{int32_t(index)};
    if (!beginProperties("colour")) return;
    rowLabel("id");
    definitionField(tr::UiSection::Colours, path, "id");
    const tr::XmlNode* n = doc_->node(tr::UiSection::Colours, path);
    rowLabel("colour");
    if (n) {
        float rgba[4];
        for (int c = 0; c < 4; ++c) {
            const char name[2] = {"RGBA"[c], 0};
            rgba[c] = float(n->hasAttribute(name) ? std::strtol(n->attribute(name).c_str(), nullptr, 10) : 255) / 255.0f;
        }
        const std::string key = pathKey(tr::UiSection::Colours, path, "rgba");
        if (const auto held = defColours_.find(key); held != defColours_.end()) std::copy(held->second.begin(), held->second.end(), rgba);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::ColorEdit4("##rgba", rgba, ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf))
            defColours_[key] = {rgba[0], rgba[1], rgba[2], rgba[3]};
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            // All four channels in one edit.
            defColours_.erase(key);
            std::vector<std::pair<std::string, std::string>> set;
            for (int c = 0; c < 4; ++c) set.push_back({std::string(1, "RGBA"[c]), std::to_string(int(std::lround(std::clamp(rgba[c], 0.0f, 1.0f) * 255.0f)))});
            editDefinition([&] { doc_->setAttributes(tr::UiSection::Colours, path, set, {}); });
        }
    }
    for (const char* c : {"R", "G", "B", "A"}) {
        rowLabel(c);
        definitionField(tr::UiSection::Colours, path, c);
    }
    ImGui::EndTable();
    if (const tr::XmlNode* c = doc_->node(tr::UiSection::Colours, path)) {
        float rgba[4];
        for (int k = 0; k < 4; ++k) {
            const char name[2] = {"RGBA"[k], 0};
            rgba[k] = float(c->hasAttribute(name) ? std::strtol(c->attribute(name).c_str(), nullptr, 10) : 255) / 255.0f;
        }
        const std::string key = pathKey(tr::UiSection::Colours, path, "rgba");
        if (const auto held = defColours_.find(key); held != defColours_.end()) std::copy(held->second.begin(), held->second.end(), rgba);
        drawColourSwatch(rgba);
    }
}

void UiWorkspace::drawTextureEditor(size_t index) {
    const std::vector<int32_t> path{int32_t(index)};
    const tr::XmlNode* n = doc_->node(tr::UiSection::Textures, path);
    if (!n) return;
    previewWanted_ = n->attribute("name");
    if (beginProperties("texture")) {
        for (const char* name : {"id", "name", "regionL", "regionT", "regionR", "regionB", "modeU", "modeV"}) {
            rowLabel(name);
            definitionField(tr::UiSection::Textures, path, name);
        }
        ImGui::EndTable();
    }
    n = doc_->node(tr::UiSection::Textures, path);
    if (!n) return;
    if (ImGui::Button("Whole texture")) {
        // Region attributes removed: the game's default (-1) draws the whole texture.
        editDefinition([&] { doc_->setAttributes(tr::UiSection::Textures, path, {}, {"regionL", "regionT", "regionR", "regionB"}); });
        n = doc_->node(tr::UiSection::Textures, path);
        if (!n) return;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove the region: the game then draws the whole texture (region -1)");
    if (!previewTexture_ || previewLoaded_ != previewWanted_ || previewWidth_ <= 0 || previewHeight_ <= 0) {
        ImGui::TextDisabled(previewWanted_.empty() ? "No file." : "Loading %s...", previewWanted_.c_str());
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s, %d x %d", previewLoaded_.c_str(), previewWidth_, previewHeight_);
    long l = std::strtol(n->attribute("regionL").c_str(), nullptr, 10), t = std::strtol(n->attribute("regionT").c_str(), nullptr, 10);
    long r = std::strtol(n->attribute("regionR").c_str(), nullptr, 10), b = std::strtol(n->attribute("regionB").c_str(), nullptr, 10);
    if (!n->hasAttribute("regionL") || !n->hasAttribute("regionR") || r <= l) l = 0, r = previewWidth_;
    if (!n->hasAttribute("regionT") || !n->hasAttribute("regionB") || b <= t) t = 0, b = previewHeight_;

    // The view: fit, or a zoom factor (screen pixels per texture pixel), scrolled.
    const float viewW = ImGui::GetContentRegionAvail().x;
    const float viewH = std::clamp(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - 4.0f, 240.0f, 720.0f);
    const float fit = std::min(viewW / float(previewWidth_), viewH / float(previewHeight_));
    float scale = previewZoom_ > 0.0f ? previewZoom_ : fit;
    const auto zoomTo = [&](float z, ImVec2 focusPixel, ImVec2 focusView) {
        // Keeps the texture point focusPixel under the view point focusView (view-relative); applied before the view's next frame.
        z = std::clamp(z, std::min(fit, 1.0f), 32.0f);
        previewZoom_ = z;
        previewScroll_ = ImVec2(std::max(0.0f, focusPixel.x * z - focusView.x), std::max(0.0f, focusPixel.y * z - focusView.y));
        previewScrollPending_ = true;
    };
    if (ImGui::SmallButton("Fit")) {
        previewZoom_ = 0.0f;
        previewScroll_ = ImVec2(0, 0);
        previewScrollPending_ = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("1:1")) zoomTo(1.0f, ImVec2(float(l + r) / 2, float(t + b) / 2), ImVec2(viewW / 2, viewH / 2));
    ImGui::SameLine();
    if (ImGui::SmallButton("Zoom to region") || std::exchange(zoomPreviewToRegion_, false)) {
        const float z = std::min(viewW / float(std::max(1L, r - l)), viewH / float(std::max(1L, b - t))) * 0.8f;
        zoomTo(z, ImVec2(float(l + r) / 2, float(t + b) / 2), ImVec2(viewW / 2, viewH / 2));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.0f%%", scale * 100.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Wheel: zoom around the cursor. Right / middle drag: pan.\n"
                          "Drag a handle or edge: move that edge; inside: move the region; outside (or Ctrl): draw a new one.");
    scale = previewZoom_ > 0.0f ? previewZoom_ : fit;
    const ImVec2 size(float(previewWidth_) * scale, float(previewHeight_) * scale);
    if (previewScrollPending_) {
        // With the new content size, so the scroll is not clamped to the old one.
        ImGui::SetNextWindowContentSize(size);
        ImGui::SetNextWindowScroll(previewScroll_);
        previewScrollPending_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("texview", ImVec2(0, viewH), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 clipMin = ImGui::GetWindowPos(), clipMax(clipMin.x + ImGui::GetWindowWidth(), clipMin.y + ImGui::GetWindowHeight());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Checkerboard under the texture (alpha), only where it is visible.
    {
        const float cell = 8.0f;
        const float x0 = std::max(origin.x, clipMin.x), y0 = std::max(origin.y, clipMin.y);
        const float x1 = std::min(origin.x + size.x, clipMax.x), y1 = std::min(origin.y + size.y, clipMax.y);
        for (float y = y0; y < y1; y += cell)
            for (float x = x0; x < x1; x += cell)
                dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, x1), std::min(y + cell, y1)),
                                  (int((x - x0) / cell) + int((y - y0) / cell)) % 2 ? IM_COL32(90, 90, 90, 255) : IM_COL32(60, 60, 60, 255));
    }
    ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)previewTexture_), size);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetMousePos();
    const auto toPixel = [&](ImVec2 m) {        // the nearest pixel edge
        return ImVec2(std::clamp(std::round((m.x - origin.x) / scale), 0.0f, float(previewWidth_)),
                      std::clamp(std::round((m.y - origin.y) / scale), 0.0f, float(previewHeight_)));
    };
    const ImVec2 under((mouse.x - origin.x) / scale, (mouse.y - origin.y) / scale);     // texture position (fractional)
    // Wheel zooms around the cursor; right or middle drag pans.
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
        zoomTo(scale * std::pow(1.25f, ImGui::GetIO().MouseWheel), under, ImVec2(mouse.x - clipMin.x, mouse.y - clipMin.y));
    if (ImGui::IsWindowHovered() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        ImGui::SetScrollX(ImGui::GetScrollX() - d.x);
        ImGui::SetScrollY(ImGui::GetScrollY() - d.y);
    }
    // Pixel grid when zoomed in far enough to place edges by eye.
    if (scale >= 8.0f) {
        const int px0 = std::max(0, int((clipMin.x - origin.x) / scale)), px1 = std::min(previewWidth_, int((clipMax.x - origin.x) / scale) + 1);
        const int py0 = std::max(0, int((clipMin.y - origin.y) / scale)), py1 = std::min(previewHeight_, int((clipMax.y - origin.y) / scale) + 1);
        for (int x = px0; x <= px1; ++x)
            dl->AddLine(ImVec2(origin.x + float(x) * scale, origin.y + float(py0) * scale), ImVec2(origin.x + float(x) * scale, origin.y + float(py1) * scale),
                        IM_COL32(0, 0, 0, 70));
        for (int y = py0; y <= py1; ++y)
            dl->AddLine(ImVec2(origin.x + float(px0) * scale, origin.y + float(y) * scale), ImVec2(origin.x + float(px1) * scale, origin.y + float(y) * scale),
                        IM_COL32(0, 0, 0, 70));
    }
    // Region editing: 0..7 handles (TL, T, TR, R, BR, B, BL, L), 8 inside (move), -1 a new region.
    const float hx[8] = {float(l), float(l + r) / 2, float(r), float(r), float(r), float(l + r) / 2, float(l), float(l)};
    const float hy[8] = {float(t), float(t), float(t), float(t + b) / 2, float(b), float(b), float(b), float(t + b) / 2};
    int handle = -1;
    if (hovered && !regionDragging_ && !ImGui::GetIO().KeyCtrl) {
        for (int k = 0; k < 8 && handle < 0; ++k)
            if (std::fabs(origin.x + hx[k] * scale - mouse.x) <= 5 && std::fabs(origin.y + hy[k] * scale - mouse.y) <= 5) handle = k;
        // Anywhere along an edge drags that edge.
        const float sx0 = origin.x + float(l) * scale, sx1 = origin.x + float(r) * scale, sy0 = origin.y + float(t) * scale, sy1 = origin.y + float(b) * scale;
        const bool withinX = mouse.x > sx0 - 4 && mouse.x < sx1 + 4, withinY = mouse.y > sy0 - 4 && mouse.y < sy1 + 4;
        if (handle < 0 && withinY && std::fabs(mouse.x - sx0) <= 4) handle = 7;
        if (handle < 0 && withinY && std::fabs(mouse.x - sx1) <= 4) handle = 3;
        if (handle < 0 && withinX && std::fabs(mouse.y - sy0) <= 4) handle = 1;
        if (handle < 0 && withinX && std::fabs(mouse.y - sy1) <= 4) handle = 5;
        if (handle < 0 && mouse.x > sx0 && mouse.x < sx1 && mouse.y > sy0 && mouse.y < sy1) handle = 8;
    }
    const int shownHandle = regionDragging_ ? regionHandle_ : handle;
    if (shownHandle >= 0)
        ImGui::SetMouseCursor(shownHandle == 8 ? ImGuiMouseCursor_ResizeAll : shownHandle == 1 || shownHandle == 5 ? ImGuiMouseCursor_ResizeNS
                              : shownHandle == 3 || shownHandle == 7 ? ImGuiMouseCursor_ResizeEW
                              : shownHandle == 0 || shownHandle == 4 ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        regionDragging_ = true;
        regionHandle_ = handle;
        regionDragStart_ = toPixel(mouse);
        regionStartL_ = l, regionStartT_ = t, regionStartR_ = r, regionStartB_ = b;
    }
    if (regionDragging_) {
        const ImVec2 p = toPixel(mouse);
        const int hd = regionHandle_;
        if (hd < 0) {
            l = long(std::min(regionDragStart_.x, p.x));
            r = long(std::max(regionDragStart_.x, p.x));
            t = long(std::min(regionDragStart_.y, p.y));
            b = long(std::max(regionDragStart_.y, p.y));
        } else if (hd == 8) {
            // Moved whole, kept inside the texture.
            const long dx = std::clamp(long(p.x - regionDragStart_.x), -regionStartL_, long(previewWidth_) - regionStartR_);
            const long dy = std::clamp(long(p.y - regionDragStart_.y), -regionStartT_, long(previewHeight_) - regionStartB_);
            l = regionStartL_ + dx, r = regionStartR_ + dx, t = regionStartT_ + dy, b = regionStartB_ + dy;
        } else {
            l = regionStartL_, t = regionStartT_, r = regionStartR_, b = regionStartB_;
            if (hd == 0 || hd == 6 || hd == 7) l = std::min(long(p.x), r - 1);
            if (hd == 2 || hd == 3 || hd == 4) r = std::max(long(p.x), l + 1);
            if (hd == 0 || hd == 1 || hd == 2) t = std::min(long(p.y), b - 1);
            if (hd == 4 || hd == 5 || hd == 6) b = std::max(long(p.y), t + 1);
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            regionDragging_ = false;
            const bool changed = l != regionStartL_ || t != regionStartT_ || r != regionStartR_ || b != regionStartB_;
            if (changed && r > l && b > t)
                editDefinition([&] {
                    doc_->setAttributes(tr::UiSection::Textures, path,
                                        {{"regionL", std::to_string(l)}, {"regionT", std::to_string(t)}, {"regionR", std::to_string(r)}, {"regionB", std::to_string(b)}}, {});
                });
        }
    }
    const ImVec2 a(origin.x + float(l) * scale, origin.y + float(t) * scale), c(origin.x + float(r) * scale, origin.y + float(b) * scale);
    dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(c.x + 1, c.y + 1), IM_COL32(0, 0, 0, 200), 0.0f, 0, 1.0f);
    dl->AddRect(a, c, regionDragging_ ? IM_COL32(80, 200, 255, 255) : IM_COL32(255, 220, 60, 255), 0.0f, 0, 2.0f);
    for (int k = 0; k < 8 && !regionDragging_; ++k) {
        const ImVec2 h(origin.x + hx[k] * scale, origin.y + hy[k] * scale);
        dl->AddRectFilled(ImVec2(h.x - 3, h.y - 3), ImVec2(h.x + 3, h.y + 3), IM_COL32(255, 255, 255, 255));
        dl->AddRect(ImVec2(h.x - 3, h.y - 3), ImVec2(h.x + 3, h.y + 3), IM_COL32(40, 40, 40, 255));
    }
    if (hovered || regionDragging_) {
        const int ux = std::clamp(int(std::floor(under.x)), 0, previewWidth_ - 1), uy = std::clamp(int(std::floor(under.y)), 0, previewHeight_ - 1);
        ImGui::SetTooltip("pixel (%d, %d)\nregion (%ld, %ld) - (%ld, %ld), %ld x %ld", ux, uy, l, t, r, b, r - l, b - t);
    }
    ImGui::EndChild();
}

void UiWorkspace::drawFontEditor(size_t index) {
    const std::vector<int32_t> path{int32_t(index)};
    if (beginProperties("font")) {
        rowLabel("id");
        definitionField(tr::UiSection::Fonts, path, "id");
        ImGui::EndTable();
    }
    drawFontSample(index);
    const tr::XmlNode* n = doc_->node(tr::UiSection::Fonts, path);
    if (!n) return;
    ImGui::SeparatorText("Ranges");
    ImGui::TextDisabled("Ranges: the first one containing a character draws it (faceName, size in points).");
    static const char* columns[] = {"RangeBegin", "RangeEnd", "faceName", "pointSize", "bold", "italic", "shadow", "outline"};
    // Fixed widths, scrolled sideways in a narrow panel.
    size_t rangeCount = 0;
    for (const tr::XmlNode& c : n->children) rangeCount += c.tag == "FontRange" ? 1 : 0;
    const float rowHeight = ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginTable("ranges", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                          ImVec2(0.0f, rowHeight * float(rangeCount + 1) + ImGui::GetStyle().ScrollbarSize + 4.0f))) {
        const float em = ImGui::GetFontSize();
        const float w[8] = {4.5f * em, 4.5f * em, 11.0f * em, 3.0f * em, 1.6f * em, 1.6f * em, 1.6f * em, 1.6f * em};
        for (int c = 0; c < 8; ++c) ImGui::TableSetupColumn(c == 0 ? "Begin" : c == 1 ? "End" : c == 2 ? "Face" : c == 3 ? "Size" : c == 4 ? "B" : c == 5 ? "I" : c == 6 ? "Sh" : "Ol",
                                                            ImGuiTableColumnFlags_WidthFixed, w[c]);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();
        for (size_t k = 0; n && k < n->children.size(); ++k) {
            if (n->children[k].tag != "FontRange") continue;
            const std::vector<int32_t> range{int32_t(index), int32_t(k)};
            ImGui::PushID(int(k));
            ImGui::TableNextRow();
            for (const char* c : columns) {
                ImGui::TableNextColumn();
                definitionField(tr::UiSection::Fonts, range, c);
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) removeDefinitionChild(tr::UiSection::Fonts, range);
            ImGui::PopID();
            n = doc_->node(tr::UiSection::Fonts, path);
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Add range")) addDefinitionChild(tr::UiSection::Fonts, path);
}

void UiWorkspace::drawAnimationEditor(size_t index) {
    const std::vector<int32_t> path{int32_t(index)};
    const tr::XmlNode* n = doc_->node(tr::UiSection::Animations, path);
    if (!n) return;
    if (beginProperties("animation")) {
        rowLabel("id");
        {
            const std::string key = pathKey(tr::UiSection::Animations, path, "#id");
            const std::string value = definitionId(tr::UiSection::Animations, index);
            auto it = defBuffers_.find(key);
            if (it == defBuffers_.end()) it = defBuffers_.emplace(key, value).first;
            char buffer[256] = {};
            std::snprintf(buffer, sizeof buffer, "%s", it->second.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputTextWithHint("##id", "required", buffer, sizeof buffer)) it->second = buffer;
            const bool active = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                const std::string v = it->second;
                defBuffers_.erase(it);
                setAnimationField(index, "id", v);
            } else if (!active && it->second != value) it->second = value;
        }
        for (const char* field : {"playbackType", "animType"}) {
            rowLabel(field);
            const uischema::Attribute* spec = uischema::definitionAttribute("Animation", field);
            const tr::XmlNode* an = doc_->node(tr::UiSection::Animations, path);
            const tr::XmlNode* c = an ? an->child(field) : nullptr;
            const std::string value = c ? c->text : "";
            ImGui::PushID(field);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##v", value.empty() ? (spec->defaultValue.empty() ? "(required)" : spec->defaultValue.c_str()) : value.c_str())) {
                for (const std::string& option : spec->values)
                    if (ImGui::Selectable(option.c_str(), option == value)) setAnimationField(index, field, option);
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(std::string(field) == "animType" ? "Widget: scale / translation / colour tracks; Bitmap: uv / rotation / texture tracks.\n"
                                                                     "Changing it replaces the tracks (refused while widgets use it as the other kind)."
                                                                   : "stall: holds the last frame; repeat: loops; pingpong: forwards then backwards");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    n = doc_->node(tr::UiSection::Animations, path);
    if (!n) return;
    drawAnimationSample(index);
    n = doc_->node(tr::UiSection::Animations, path);
    if (!n) return;
    const tr::XmlNode* type = n->child("animType");
    ImGui::TextDisabled("Times in milliseconds. Interactive mode on the canvas plays it on the widgets that use it.");
    for (const std::string& track : uischema::animationTracks(type ? type->text : "")) {
        ImGui::PushID(track.c_str());
        n = doc_->node(tr::UiSection::Animations, path);
        const int32_t t = n ? childIndex(*n, track) : -1;
        if (t < 0) {
            ImGui::TextDisabled("%s: none", track.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Add track")) addAnimationTrack(index, track);
            ImGui::PopID();
            continue;
        }
        const std::vector<int32_t> trackPath{int32_t(index), t};
        const tr::XmlNode& tn = n->children[size_t(t)];
        const std::string frame = track == "textureTrack" ? std::string() : uischema::frameTag(track);
        if (ImGui::CollapsingHeader((track + " (" + std::to_string(tn.children.size()) + " frames)###" + track).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (beginProperties("track")) {
                for (const auto& a : uischema::definitionAttributes(track)) {
                    rowLabel(a.name.c_str());
                    definitionField(tr::UiSection::Animations, trackPath, a.name);
                }
                ImGui::EndTable();
            }
            if (frame.empty()) {
                ImGui::TextDisabled("Texture frames are not decoded (no shipped animation has one).");
            } else {
                const auto& specs = uischema::definitionAttributes(frame);
                if (ImGui::BeginTable("frames", int(specs.size()) + 1, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
                    for (const auto& a : specs) ImGui::TableSetupColumn(a.name.c_str());
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
                    ImGui::TableHeadersRow();
                    const tr::XmlNode* trackNode = doc_->node(tr::UiSection::Animations, trackPath);
                    for (size_t f = 0; trackNode && f < trackNode->children.size(); ++f) {
                        const std::vector<int32_t> framePath{int32_t(index), t, int32_t(f)};
                        ImGui::PushID(int(f));
                        ImGui::TableNextRow();
                        for (const auto& a : specs) {
                            ImGui::TableNextColumn();
                            definitionField(tr::UiSection::Animations, framePath, a.name);
                        }
                        ImGui::TableNextColumn();
                        if (ImGui::SmallButton("x")) removeDefinitionChild(tr::UiSection::Animations, framePath);
                        ImGui::PopID();
                        trackNode = doc_->node(tr::UiSection::Animations, trackPath);
                    }
                    ImGui::EndTable();
                }
                if (ImGui::SmallButton("Add frame")) addDefinitionChild(tr::UiSection::Animations, trackPath);
            }
        }
        ImGui::PopID();
    }
}
