#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace mm::aamp {

// AAMP v2 parameter archives (.bxml, .bdrop, .bshop, .baslist, .bgparamlist…). Lists,
// objects and parameters are identified by the CRC32 of their names; the file stores only
// the hashes. Everything keeps file order.
enum class Type : uint8_t {
    Bool = 0, F32, Int, Vec2, Vec3, Vec4, Color, String32, String64, Curve1, Curve2, Curve3, Curve4,
    BufferInt, BufferF32, String256, Quat, U32, BufferU32, BufferBinary, StringRef,
};

uint32_t hash(const std::string& name);  // CRC32

struct Parameter {
    Type type = Type::Int;
    // Little-endian value bytes (buffers: the elements, without the count), or the text for
    // string types.
    std::string data;

    bool isString() const { return type == Type::String32 || type == Type::String64 || type == Type::String256 || type == Type::StringRef; }
    bool operator==(const Parameter& o) const { return type == o.type && data == o.data; }
    bool operator!=(const Parameter& o) const { return !(*this == o); }

    static Parameter boolean(bool v);
    static Parameter f32(float v);
    static Parameter s32(int32_t v);
    static Parameter u32(uint32_t v);
    static Parameter floats(Type type, const std::vector<float>& v);  // Vec2/3/4, Color, Quat
    static Parameter string(Type type, std::string v);

    bool asBool() const;
    float asF32() const;
    int32_t asInt() const;
    uint32_t asU32() const;
};

// Objects and parameters are deques: a reference to one stays valid while more are added.
// (Lists can't be: a list holds lists. Don't keep a list reference across listOrAdd.)
struct ParameterObject {
    std::deque<std::pair<uint32_t, Parameter>> params;
    const Parameter* find(uint32_t key) const;
    const Parameter* find(const std::string& name) const { return find(hash(name)); }
    void set(uint32_t key, Parameter p);  // replace in place, or append
    bool operator==(const ParameterObject& o) const { return params == o.params; }
};

struct ParameterList {
    std::vector<std::pair<uint32_t, ParameterList>> lists;
    std::deque<std::pair<uint32_t, ParameterObject>> objects;
    const ParameterList* list(uint32_t key) const;
    ParameterList* list(uint32_t key);
    const ParameterObject* object(uint32_t key) const;
    ParameterObject* object(uint32_t key);
    ParameterList& listOrAdd(uint32_t key);
    ParameterObject& objectOrAdd(uint32_t key);
    bool operator==(const ParameterList& o) const { return lists == o.lists && objects == o.objects; }
};

struct ParameterIO {
    uint32_t version = 0;
    std::string type = "xml";
    ParameterList root;
};

ParameterIO parse(const std::string& data);  // Yaz0 or plain; throws on bad data
std::string write(const ParameterIO& pio);

}  // namespace mm::aamp
