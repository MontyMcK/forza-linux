#include "hle/registry.h"

#include <cctype>
#include <unordered_map>

namespace fl::hle {
namespace {

using Map = std::unordered_map<std::string, void*>;

// Function-local static: registrars in other translation units run during
// static init, in unspecified order.
Map& table() {
    static Map m;
    return m;
}

std::string key(std::string_view dll, std::string_view name) {
    return normalize_dll(dll) + "!" + std::string(name);
}

}  // namespace

std::string normalize_dll(std::string_view dll) {
    std::string s;
    for (char c : dll) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".dll") == 0) s.resize(s.size() - 4);
    return s;
}

Registrar::Registrar(const char* dll, const char* name, void* fn) { table()[key(dll, name)] = fn; }

void* lookup(std::string_view dll, std::string_view name) {
    auto it = table().find(key(dll, name));
    return it == table().end() ? nullptr : it->second;
}

size_t count() { return table().size(); }

}  // namespace fl::hle
