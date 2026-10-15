#pragma once
#include <algorithm>
#include <cctype>
#include <set>
#include <map>
#include <string>
#include <vector>
namespace chili {
inline std::string fat_upper(std::string text) {
    for (auto &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
// Reserve existing 8.3 names before assigning aliases, so a literal ~1 file
// cannot be shadowed by a generated alias. The caller supplies listing order.
inline std::vector<std::string> fat_short_names(const std::vector<std::string> &names,
    const std::map<std::string, std::string> &previous = {}) {
    std::vector<std::string> result(names.size());
    std::set<std::string> used;
    auto valid_character = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            std::string("!#$%&'()-@^_`{}~").find(c) != std::string::npos;
    };
    auto parts = [](const std::string &name) {
        const auto dot = name.find_last_of('.');
        return dot == std::string::npos ? std::pair{name, std::string{}} :
            std::pair{name.substr(0u, dot), name.substr(dot + 1u)};
    };
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto upper = fat_upper(names[i]);
        const auto [stem, extension] = parts(upper);
        if (!stem.empty() && stem.size() <= 8u && extension.size() <= 3u &&
            std::all_of(stem.begin(), stem.end(), valid_character) &&
            std::all_of(extension.begin(), extension.end(), valid_character)) {
            result[i] = stem + (extension.empty() ? "" : "." + extension); used.insert(result[i]);
        }
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!result[i].empty()) continue;
        const auto old = previous.find(names[i]);
        if (old != previous.end() && used.insert(old->second).second) result[i] = old->second;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!result[i].empty()) continue;
        auto [stem, extension] = parts(fat_upper(names[i]));
        auto clean = [&](std::string text) {
            std::string output;
            for (unsigned char c : text) {
                if (c == ' ' || c == '.') continue;
                output += valid_character(c) ? static_cast<char>(c) : '_';
            }
            return output;
        };
        stem = clean(stem); extension = clean(extension).substr(0u, 3u);
        if (stem.empty()) stem = "_";
        for (unsigned n = 1u; n <= 9999999u; ++n) {
            const auto suffix = "~" + std::to_string(n);
            const auto candidate = stem.substr(0u, 8u - suffix.size()) + suffix +
                (extension.empty() ? "" : "." + extension);
            if (used.insert(candidate).second) { result[i] = candidate; break; }
        }
    }
    return result;
}
}
