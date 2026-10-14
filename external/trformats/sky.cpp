#include "sky.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>

namespace tr {

namespace {

// Minimal XML: elements, attributes, self-closing tags, comments, processing instructions, the five predefined
// entities. Enough for the game's property files (no DTDs, CDATA or namespaces).
struct XmlNode {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<XmlNode> children;
    std::string attribute(const std::string& name) const {
        for (const auto& a : attributes)
            if (a.first == name) return a.second;
        return "";
    }
    const XmlNode* child(const std::string& name) const {
        for (const XmlNode& c : children)
            if (c.tag == name) return &c;
        return nullptr;
    }
};

class XmlParser {
public:
    explicit XmlParser(const Bytes& data) : text_(reinterpret_cast<const char*>(data.data()), data.size()) {}
    XmlNode document() {
        skipMisc();
        XmlNode root = element();
        skipMisc();
        if (pos_ < text_.size()) throw FormatError("XML: content after the root element");
        return root;
    }
private:
    [[noreturn]] void error(const std::string& what) const { throw FormatError("XML: " + what + " at offset " + std::to_string(pos_)); }
    bool startsWith(const char* s) const { return text_.compare(pos_, std::char_traits<char>::length(s), s) == 0; }
    void skipSpace() { while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_; }
    void skipPast(const char* end) {
        const size_t found = text_.find(end, pos_);
        if (found == std::string::npos) error(std::string("missing ") + end);
        pos_ = found + std::char_traits<char>::length(end);
    }
    void skipMisc() {                                  // whitespace, <?…?>, <!--…-->, <!…>
        for (;;) {
            skipSpace();
            if (startsWith("<?")) skipPast("?>");
            else if (startsWith("<!--")) skipPast("-->");
            else if (startsWith("<!")) skipPast(">");
            else return;
        }
    }
    std::string name() {
        const size_t start = pos_;
        while (pos_ < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '_' || text_[pos_] == ':' ||
                                       text_[pos_] == '-' || text_[pos_] == '.'))
            ++pos_;
        if (pos_ == start) error("name expected");
        return text_.substr(start, pos_ - start);
    }
    static std::string decode(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '&') { out += s[i]; continue; }
            const size_t semi = s.find(';', i);
            const std::string entity = semi == std::string::npos ? "" : s.substr(i + 1, semi - i - 1);
            const char c = entity == "amp" ? '&' : entity == "lt" ? '<' : entity == "gt" ? '>' : entity == "quot" ? '"' : entity == "apos" ? '\'' : 0;
            if (c) { out += c; i = semi; }
            else out += '&';
        }
        return out;
    }
    XmlNode element() {
        if (pos_ >= text_.size() || text_[pos_] != '<') error("'<' expected");
        ++pos_;
        XmlNode node;
        node.tag = name();
        for (;;) {
            skipSpace();
            if (startsWith("/>")) { pos_ += 2; return node; }
            if (startsWith(">")) { ++pos_; break; }
            std::string key = name();
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '=') error("'=' expected");
            ++pos_;
            skipSpace();
            const char quote = pos_ < text_.size() ? text_[pos_] : 0;
            if (quote != '"' && quote != '\'') error("quoted value expected");
            const size_t end = text_.find(quote, pos_ + 1);
            if (end == std::string::npos) error("unterminated value");
            node.attributes.emplace_back(std::move(key), decode(text_.substr(pos_ + 1, end - pos_ - 1)));
            pos_ = end + 1;
        }
        for (;;) {                                     // content: text is ignored (the v3 files keep data in attributes)
            const size_t lt = text_.find('<', pos_);
            if (lt == std::string::npos) error("unterminated element " + node.tag);
            pos_ = lt;
            if (startsWith("</")) {
                pos_ += 2;
                if (name() != node.tag) error("mismatched end tag for " + node.tag);
                skipSpace();
                if (pos_ >= text_.size() || text_[pos_] != '>') error("'>' expected");
                ++pos_;
                return node;
            }
            if (startsWith("<!--")) { skipPast("-->"); continue; }
            node.children.push_back(element());
        }
    }
    std::string text_;
    size_t pos_ = 0;
};

using Properties = std::map<std::string, std::string>;

Properties properties(const XmlNode* element) {     // element's <PropertyObject><property name value/>…
    Properties p;
    const XmlNode* object = element ? element->child("PropertyObject") : nullptr;
    if (!object) return p;
    for (const XmlNode& c : object->children)
        if (c.tag == "property") p[c.attribute("name")] = c.attribute("value");
    return p;
}

std::vector<float> numbers(const std::string& s) {   // "1.0, 2.0, 3.0"
    std::vector<float> out;
    const char* p = s.c_str();
    while (*p) {
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p) { ++p; continue; }
        out.push_back(v);
        p = end;
    }
    return out;
}

float number(const Properties& p, const char* key, float fallback = 0) {
    const auto it = p.find(key);
    return it == p.end() || it->second.empty() ? fallback : std::strtof(it->second.c_str(), nullptr);
}
bool flag(const Properties& p, const char* key, bool fallback = false) {
    const auto it = p.find(key);
    return it == p.end() ? fallback : it->second == "true";
}
Vec3 vec3(const Properties& p, const char* key, const Vec3& fallback) {
    const auto it = p.find(key);
    if (it == p.end()) return fallback;
    const std::vector<float> v = numbers(it->second);
    return v.size() >= 3 ? Vec3{v[0], v[1], v[2]} : fallback;
}
// Packed colour properties (LightKeyFrame Diffuse, FogKeyFrame Color: property type 10, string <-> packed ARGB by
// FUN_00409650 / FUN_00409680 -> FUN_004191f0) are written as the bytes low to high: "b, g, r, a" in 0..255.
// Verified 2026-10-11: Property_LightKeyFrame_DiffuseColorRef's getter (FUN_0082c150) reads the same packed value as
// R = byte 2, G = byte 1, B = byte 0, and in all 4,381 shipped light keys DiffuseColor = Diffuse reversed.
Vec3 packedColour(const Properties& p, const char* key) {
    const auto it = p.find(key);
    if (it == p.end()) return Vec3{};
    const std::vector<float> v = numbers(it->second);
    return v.size() >= 3 ? Vec3{v[2] / 255.0f, v[1] / 255.0f, v[0] / 255.0f} : Vec3{};
}

SkyLight readLight(const XmlNode& node) {
    const Properties p = properties(&node);
    SkyLight light;
    light.name = p.count("Name") ? p.at("Name") : "";
    light.hemi = flag(p, "Hemi");
    light.active = flag(p, "Active", true);
    light.castsShadows = flag(p, "CastsShadows");
    light.orientation = vec3(p, "Orientation", Vec3{0, 1, 0});
    light.renderMode = uint32_t(number(p, "RenderMode"));
    if (const XmlNode* frames = node.child("LightKeyFrames"))
        for (const XmlNode& f : frames->children) {
            const Properties k = properties(&f);
            SkyLightKey key;
            key.time = number(k, "Time");
            key.type = uint32_t(number(k, "Type"));
            key.colour = packedColour(k, "Diffuse");
            key.diffuseIntensity = number(k, "DIntensity", 1);
            key.ambientIntensity = number(k, "AIntensity");
            key.specularIntensity = number(k, "SIntensity");
            key.brightness = number(k, "DBrightness", 1);
            key.shadowOpacity = number(k, "ShadowOpacity", 1);
            light.keys.push_back(key);
        }
    std::sort(light.keys.begin(), light.keys.end(), [](const SkyLightKey& a, const SkyLightKey& b) { return a.time < b.time; });
    return light;
}

// Cyclic interpolation over keys sorted by time: before the first key, wrap from the last (time - 1).
template <typename Key, typename Value, typename Get>
Value cyclic(const std::vector<const Key*>& keys, float time, Get get) {
    if (keys.empty()) return Value{};
    if (keys.size() == 1) return get(*keys.front());
    time -= std::floor(time);
    const Key* a = keys.back();
    const Key* b = keys.front();
    float ta = a->time - 1.0f, tb = b->time;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i]->time > time) {
            b = keys[i];
            tb = b->time;
            if (i > 0) { a = keys[i - 1]; ta = a->time; }
            break;
        }
        if (i + 1 == keys.size()) { a = keys[i]; ta = a->time; b = keys.front(); tb = b->time + 1.0f; }
    }
    const float u = tb > ta ? (time - ta) / (tb - ta) : 0.0f;
    return get(*a) * (1.0f - u) + get(*b) * u;
}

struct V3 {                                            // Vec3 with the arithmetic cyclic() needs
    float x = 0, y = 0, z = 0;
    V3 operator*(float s) const { return V3{x * s, y * s, z * s}; }
    V3 operator+(const V3& o) const { return V3{x + o.x, y + o.y, z + o.z}; }
};

Vec3 rotateAbout(const Vec3& v, const Vec3& axisIn, float angle) {     // Rodrigues
    const float n = std::sqrt(axisIn.x * axisIn.x + axisIn.y * axisIn.y + axisIn.z * axisIn.z);
    if (n <= 0 || angle == 0) return v;
    const Vec3 k{axisIn.x / n, axisIn.y / n, axisIn.z / n};
    const float c = std::cos(angle), s = std::sin(angle);
    const float d = k.x * v.x + k.y * v.y + k.z * v.z;
    const Vec3 cr{k.y * v.z - k.z * v.y, k.z * v.x - k.x * v.z, k.x * v.y - k.y * v.x};
    return Vec3{v.x * c + cr.x * s + k.x * d * (1 - c), v.y * c + cr.y * s + k.y * d * (1 - c), v.z * c + cr.z * s + k.z * d * (1 - c)};
}

}  // namespace

Sky readSky(const Bytes& data) {
    const XmlNode root = XmlParser(data).document();
    if (root.tag != "SkyData") throw FormatError("sky: root element " + root.tag + " (only <SkyData> files are supported)");
    Sky sky;
    sky.weatherActive = flag(properties(&root), "WeatherActive");
    if (const XmlNode* global = root.child("GlobalLights"))
        if (const XmlNode* lights = global->child("Lights"))
            for (const XmlNode& l : lights->children) sky.globalLights.push_back(readLight(l));
    if (const XmlNode* layers = root.child("SkyLayers"))
        for (const XmlNode& l : layers->children) {
            const Properties p = properties(&l);
            SkyLayer layer;
            layer.enabled = flag(p, "Enabled", true);
            layer.geoFilename = p.count("GeoFilename") ? p.at("GeoFilename") : "";
            layer.rotationAxis = vec3(p, "RotationAxis", Vec3{0, 1, 0});
            layer.rotationsPerDay = number(p, "RotsPerDay");
            layer.initialPitch = number(p, "InitialPitch");
            layer.initialYaw = number(p, "InitialYaw");
            layer.initialRoll = number(p, "InitialRoll");
            layer.useFogColour = flag(p, "UseFogColor");
            if (p.count("DiffuseColor")) layer.baseColour = packedColour(p, "DiffuseColor");
            if (const XmlNode* frames = l.child("AlphaKeyFrames"))
                for (const XmlNode& f : frames->children) {
                    const Properties k = properties(&f);
                    layer.alphaKeys.push_back(SkyAlphaKey{number(k, "Time"), number(k, "Alpha", 1)});
                    SkyColourKey key;
                    key.time = number(k, "Time");
                    const auto colour = k.find("Color");
                    const std::vector<float> v = colour == k.end() ? std::vector<float>{} : numbers(colour->second);
                    if (v.size() >= 3) key.colour = Vec3{v[0], v[1], v[2]};
                    key.alpha = v.size() >= 4 ? v[3] : number(k, "Alpha", 1);
                    layer.colourKeys.push_back(key);
                }
            if (const XmlNode* frames = l.child("OffsetKeyFrames"))
                for (const XmlNode& f : frames->children) {
                    const Properties k = properties(&f);
                    layer.offsetKeys.push_back(SkyOffsetKey{number(k, "Time"), Vec3{number(k, "X"), number(k, "Y"), number(k, "Z")}});
                }
            std::sort(layer.alphaKeys.begin(), layer.alphaKeys.end(), [](const SkyAlphaKey& a, const SkyAlphaKey& b) { return a.time < b.time; });
            std::sort(layer.colourKeys.begin(), layer.colourKeys.end(), [](const SkyColourKey& a, const SkyColourKey& b) { return a.time < b.time; });
            std::sort(layer.offsetKeys.begin(), layer.offsetKeys.end(), [](const SkyOffsetKey& a, const SkyOffsetKey& b) { return a.time < b.time; });
            if (const XmlNode* lights = l.child("Lights"))
                for (const XmlNode& light : lights->children) layer.lights.push_back(readLight(light));
            sky.layers.push_back(std::move(layer));
        }
    if (const XmlNode* frames = root.child("FogFrames"))
        for (const XmlNode& f : frames->children) {
            const Properties k = properties(&f);
            SkyFogKey key;
            key.time = number(k, "Time");
            key.colour = packedColour(k, "Color");
            key.startDistance = number(k, "StartDist");
            key.endDistance = number(k, "EndDist");
            key.used = flag(k, "Used");
            sky.fog.push_back(key);
        }
    std::sort(sky.fog.begin(), sky.fog.end(), [](const SkyFogKey& a, const SkyFogKey& b) { return a.time < b.time; });
    return sky;
}

std::vector<PlacedLight> readLights(const Bytes& data, const std::map<std::string, std::string>& overrides) {
    const XmlNode root = XmlParser(data).document();
    if (root.tag != "LightRoot") throw FormatError("lights: root element " + root.tag + " (expected <LightRoot>)");
    std::vector<PlacedLight> out;
    const XmlNode* lights = root.child("Lights");
    if (!lights) return out;
    for (const XmlNode& l : lights->children) {
        Properties p = properties(&l);
        for (const auto& o : overrides) p[o.first] = o.second;
        const XmlNode* object = l.child("PropertyObject");
        const std::string cls = object ? object->attribute("class") : "";
        PlacedLight light;
        light.kind = cls == "LightImplPropsSpot" ? PlacedLight::Kind::Spot : cls == "LightImplPropsPlane" ? PlacedLight::Kind::Plane : PlacedLight::Kind::Point;
        light.name = p.count("Name") ? p.at("Name") : "";
        light.enabled = flag(p, "Enabled", true);
        light.castsShadows = flag(p, "CastsShadows");
        light.position = vec3(p, "Position", Vec3{});
        light.offset = vec3(p, "Offset", Vec3{});
        const auto colour = p.find("Color");
        if (colour != p.end()) {
            const std::vector<float> c = numbers(colour->second);
            if (c.size() >= 3) light.colour = Vec3{c[0], c[1], c[2]};
        }
        light.intensity = number(p, "Intensity", 1);
        light.range = number(p, "Range", 10);
        light.falloffPercent = number(p, "FalloffPercent", 1);
        auto orientation = p.find("WorldOrientation");
        if (orientation == p.end()) orientation = p.find("LocalOrientation");
        if (orientation != p.end()) {
            const std::vector<float> q = numbers(orientation->second);
            if (q.size() >= 4) light.worldOrientation = Quat{q[0], q[1], q[2], q[3]};
        }
        if (p.count("GlobeMapFile")) light.globeMap = p.at("GlobeMapFile");
        light.coneAngle = number(p, "ConeAngle", 45);
        light.penumbraAngle = number(p, "PenumbraAngle", 60);
        light.id = p.count("ID") ? std::strtoull(p.at("ID").c_str(), nullptr, 10) : 0;
        light.width = number(p, "Width", 0);
        light.length = number(p, "Length", 0);
        light.falloffPercentVec = vec3(p, "FalloffPercentVec", Vec3{});
        out.push_back(std::move(light));
    }
    return out;
}

// ------------------------------------------------------------------------------------------
// LightsDocument
// ------------------------------------------------------------------------------------------
LightsDocument::LightsDocument(const Bytes& xml) : text_(xml.begin(), xml.end()) { scan(); }

namespace {

size_t lineStart(const std::string& s, size_t at) {
    while (at > 0 && s[at - 1] != '\n') --at;
    return at;
}

size_t lineEnd(const std::string& s, size_t at) {         // just past the newline (or the end)
    const size_t nl = s.find('\n', at);
    return nl == std::string::npos ? s.size() : nl + 1;
}

std::string escapeXml(const std::string& v) {
    std::string out;
    for (char c : v) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

}  // namespace

void LightsDocument::scan() {
    lights_.clear();
    const size_t open = text_.find("<Lights>");
    if (open == std::string::npos) return;
    const size_t close = text_.find("</Lights>", open);
    const size_t limit = close == std::string::npos ? text_.size() : close;
    size_t at = open + 8;
    for (;;) {
        size_t start = text_.find("<Light ", at);
        const size_t startBare = text_.find("<Light>", at);
        if (startBare < start) start = startBare;
        if (start == std::string::npos || start >= limit) break;
        const size_t end = text_.find("</Light>", start);
        if (end == std::string::npos || end >= limit) break;
        lights_.push_back(Span{lineStart(text_, start), lineEnd(text_, end + 8)});
        at = end + 8;
    }
}

std::string LightsDocument::property(size_t light, const std::string& name) const {
    if (light >= lights_.size()) return "";
    const Span& s = lights_[light];
    const std::string key = "<property name=\"" + name + "\"";
    const size_t p = text_.find(key, s.begin);
    if (p == std::string::npos || p >= s.end) return "";
    const size_t v = text_.find("value=\"", p);
    if (v == std::string::npos || v >= s.end) return "";
    const size_t e = text_.find('"', v + 7);
    return e == std::string::npos ? "" : text_.substr(v + 7, e - v - 7);
}

bool LightsDocument::setProperty(size_t light, const std::string& name, const std::string& value, const std::string& type) {
    if (light >= lights_.size()) return false;
    const Span s = lights_[light];
    const std::string escaped = escapeXml(value);
    const std::string key = "<property name=\"" + name + "\"";
    const size_t p = text_.find(key, s.begin);
    if (p != std::string::npos && p < s.end) {
        const size_t v = text_.find("value=\"", p);
        const size_t e = v == std::string::npos ? std::string::npos : text_.find('"', v + 7);
        if (v == std::string::npos || e == std::string::npos || e >= s.end) return false;
        text_.replace(v + 7, e - v - 7, escaped);
    } else {
        const size_t closeObject = text_.find("</PropertyObject>", s.begin);
        if (closeObject == std::string::npos || closeObject >= s.end) return false;
        // Indent like the previous property line.
        const size_t previous = text_.rfind("<property ", closeObject);
        const size_t indentStart = previous == std::string::npos ? lineStart(text_, closeObject) : lineStart(text_, previous);
        const std::string indent = text_.substr(indentStart, (previous == std::string::npos ? closeObject : previous) - indentStart);
        const std::string newline = text_.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        const std::string line = indent + "<property name=\"" + name + "\" value=\"" + escaped + "\" type=\"" + type + "\" />" + newline;
        text_.insert(lineStart(text_, closeObject), line);
    }
    scan();
    return true;
}

std::vector<LightsDocument::Span> LightsDocument::modsOf(uint64_t id) const {
    std::vector<Span> out;
    if (id == 0) return out;
    const std::string key = "<Mod LightID=\"" + std::to_string(id) + "\"";
    for (size_t at = text_.find(key); at != std::string::npos; at = text_.find(key, at + key.size())) {
        const size_t end = text_.find("</Mod>", at);
        if (end == std::string::npos) break;
        out.push_back(Span{lineStart(text_, at), lineEnd(text_, end + 6)});
    }
    return out;
}

size_t LightsDocument::duplicate(size_t light, uint64_t newId, const std::string& newName) {
    if (light >= lights_.size()) return size_t(-1);
    const uint64_t oldId = std::strtoull(property(light, "ID").c_str(), nullptr, 10);
    std::vector<std::string> mods;
    for (const Span& m : modsOf(oldId)) {
        std::string block = text_.substr(m.begin, m.end - m.begin);
        const std::string from = "LightID=\"" + std::to_string(oldId) + "\"", to = "LightID=\"" + std::to_string(newId) + "\"";
        block.replace(block.find(from), from.size(), to);
        mods.push_back(block);
    }
    const std::string copy = text_.substr(lights_[light].begin, lights_[light].end - lights_[light].begin);
    text_.insert(lights_.back().end, copy);
    scan();
    const size_t index = lights_.size() - 1;
    setProperty(index, "ID", std::to_string(newId));
    setProperty(index, "Name", newName);
    if (!mods.empty()) {
        const size_t closeMods = text_.find("</Mods>");
        if (closeMods != std::string::npos) {
            size_t at = lineStart(text_, closeMods);
            for (const std::string& m : mods) {
                text_.insert(at, m);
                at += m.size();
            }
        }
        scan();
    }
    return index;
}

void LightsDocument::remove(size_t light) {
    if (light >= lights_.size()) return;
    const uint64_t id = std::strtoull(property(light, "ID").c_str(), nullptr, 10);
    std::vector<Span> spans = modsOf(id);
    spans.push_back(lights_[light]);
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.begin > b.begin; });   // back to front
    for (const Span& s : spans) text_.erase(s.begin, s.end - s.begin);
    scan();
}

std::string LightsDocument::formatNumber(float v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.7f", double(v));
    return buffer;
}

std::string LightsDocument::formatVector(const float* v, size_t n) {
    std::string out;
    for (size_t k = 0; k < n; ++k) out += (k ? ", " : "") + formatNumber(v[k]);
    return out;
}

std::vector<LightModifier> readLightModifiers(const Bytes& data) {
    const XmlNode root = XmlParser(data).document();
    std::vector<LightModifier> out;
    const XmlNode* mods = root.child("Mods");
    if (!mods) return out;
    for (const XmlNode& m : mods->children) {
        if (m.tag != "Mod") continue;
        LightModifier mod;
        mod.lightId = std::strtoull(m.attribute("LightID").c_str(), nullptr, 10);
        for (const XmlNode& t : m.children) {
            if (t.tag != "Track") continue;
            LightModifierTrack track;
            const XmlNode* object = t.child("PropertyObject");
            const std::string cls = object ? object->attribute("class") : "";
            const Properties p = properties(&t);
            track.enabled = flag(p, "Enabled", true);
            if (cls == "IntensityModifier") {
                track.kind = LightModifierTrack::Kind::Intensity;
                track.intensity1 = number(p, "Intensity1");
                track.intensity2 = number(p, "Intensity2");
            } else if (cls == "ColorModifier") {
                track.kind = LightModifierTrack::Kind::Color;
                track.colour1 = vec3(p, "Color1", Vec3{1, 1, 1});
                track.colour2 = vec3(p, "Color2", Vec3{1, 1, 1});
            } else if (cls == "RotationModifier") {
                track.kind = LightModifierTrack::Kind::Rotation;
                track.axis = vec3(p, "RotationAxis", Vec3{0, 1, 0});
            } else if (cls == "PositionModifier") {
                track.kind = LightModifierTrack::Kind::Position;
                track.offset1 = vec3(p, "Offset1", Vec3{});
                track.offset2 = vec3(p, "Offset2", Vec3{});
            }
            if (const XmlNode* keys = t.child("Keys"))
                for (const XmlNode& k : keys->children)
                    if (k.tag == "Key")
                        track.keys.emplace_back(std::strtof(k.attribute("time").c_str(), nullptr), std::strtof(k.attribute("weight").c_str(), nullptr));
            mod.tracks.push_back(std::move(track));
        }
        out.push_back(std::move(mod));
    }
    return out;
}

float lightModifierWeight(const LightModifierTrack& track, float elapsed) {
    const auto& k = track.keys;
    if (k.empty()) return 1.0f;
    if (k.size() == 1) return k.front().second;
    const float last = k.back().first;
    float t = elapsed;
    if (last > 0.0f && t > last) t = std::fmod(t, last);
    t = std::clamp(t, 0.0f, last);
    float weight = k.front().second;
    for (size_t i = 0; i + 1 < k.size(); ++i)
        if (k[i].first <= t && t <= k[i + 1].first) {
            const float span = k[i + 1].first - k[i].first;
            if (span <= 1e-6f) return k[i + 1].second;
            return k[i].second + (t - k[i].first) / span * (k[i + 1].second - k[i].second);
        }
    return weight;
}

Vec3 PlacedLight::direction() const {
    const Quat& q = worldOrientation;                  // rotate the light's local +z by q (trspotlight's cone axis)
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    return Vec3{2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
}

Vec3 skyLightColour(const SkyLight& light, uint32_t keyType, float time) {
    std::vector<const SkyLightKey*> keys;
    for (const SkyLightKey& k : light.keys)
        if (k.type == keyType) keys.push_back(&k);
    const V3 c = cyclic<SkyLightKey, V3>(keys, time, [](const SkyLightKey& k) {
        return V3{k.colour.x * k.diffuseIntensity, k.colour.y * k.diffuseIntensity, k.colour.z * k.diffuseIntensity};
    });
    return Vec3{c.x, c.y, c.z};
}

float skyLightShadowOpacity(const SkyLight& light, float time) {
    std::vector<const SkyLightKey*> keys;
    for (const SkyLightKey& k : light.keys)
        if (k.type == 3) keys.push_back(&k);
    if (keys.empty()) return 1.0f;
    return cyclic<SkyLightKey, V3>(keys, time, [](const SkyLightKey& k) { return V3{k.shadowOpacity, 0, 0}; }).x;
}

SkyFogKey sampleSkyFog(const Sky& sky, float time) {
    std::vector<const SkyFogKey*> keys;
    for (const SkyFogKey& k : sky.fog) keys.push_back(&k);
    SkyFogKey out;
    if (keys.empty()) return out;
    const V3 c = cyclic<SkyFogKey, V3>(keys, time, [](const SkyFogKey& k) { return V3{k.colour.x, k.colour.y, k.colour.z}; });
    const V3 d = cyclic<SkyFogKey, V3>(keys, time, [](const SkyFogKey& k) { return V3{k.startDistance, k.endDistance, 0}; });
    out.time = time;
    out.colour = Vec3{c.x, c.y, c.z};
    out.startDistance = d.x;
    out.endDistance = d.y;
    out.used = keys.front()->used;
    return out;
}

SkyColourKey sampleLayerColour(const SkyLayer& layer, float time) {
    SkyColourKey out;
    out.time = time;
    if (layer.colourKeys.empty()) return out;
    std::vector<const SkyColourKey*> keys;
    for (const SkyColourKey& k : layer.colourKeys) keys.push_back(&k);
    const V3 c = cyclic<SkyColourKey, V3>(keys, time, [](const SkyColourKey& k) { return V3{k.colour.x, k.colour.y, k.colour.z}; });
    out.colour = Vec3{c.x, c.y, c.z};
    out.alpha = cyclic<SkyColourKey, V3>(keys, time, [](const SkyColourKey& k) { return V3{k.alpha, 0, 0}; }).x;
    return out;
}

Vec3 sampleLayerOffset(const SkyLayer& layer, float time) {
    if (layer.offsetKeys.empty()) return Vec3{};
    std::vector<const SkyOffsetKey*> keys;
    for (const SkyOffsetKey& k : layer.offsetKeys) keys.push_back(&k);
    const V3 o = cyclic<SkyOffsetKey, V3>(keys, time, [](const SkyOffsetKey& k) { return V3{k.offset.x, k.offset.y, k.offset.z}; });
    return Vec3{o.x, o.y, o.z};
}

float sampleLayerAlpha(const SkyLayer& layer, float time) {
    std::vector<const SkyAlphaKey*> keys;
    for (const SkyAlphaKey& k : layer.alphaKeys) keys.push_back(&k);
    if (keys.empty()) return 1.0f;
    return cyclic<SkyAlphaKey, V3>(keys, time, [](const SkyAlphaKey& k) { return V3{k.alpha, 0, 0}; }).x;
}

Vec3 skyLayerDirection(const SkyLayer& layer, const Vec3& direction, float time) {
    // InitialRoll turns the other way to the other angles here (fitted to in-game shadows: Concordia Wilderness'
    // sun layer, roll -30 deg, puts the 12:00 sun east at (0.5, 0.87, 0); the opposite sign put it west and lit
    // faces that are dark in game). The engine's own rotation code is not decoded.
    Vec3 v = rotateAbout(direction, Vec3{0, 0, 1}, -layer.initialRoll);
    v = rotateAbout(v, Vec3{1, 0, 0}, layer.initialPitch);
    v = rotateAbout(v, Vec3{0, 1, 0}, layer.initialYaw);
    const float turns = layer.rotationsPerDay * (time - std::floor(time));
    return rotateAbout(v, layer.rotationAxis, turns * 6.2831853f);
}

}  // namespace tr
