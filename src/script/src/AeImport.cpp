#include "ruby/script/AeImport.h"

#include <algorithm>
#include <cctype>

namespace ruby::script {
namespace {

bool isIdentChar(char c) {
    return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_' || c == '$';
}

void noteOnce(std::vector<std::string>& into, const std::string& text) {
    if (std::find(into.begin(), into.end(), text) == into.end()) {
        into.push_back(text);
    }
}

// Whether `[` opens an array literal or indexes something. Disambiguated by the
// preceding char: identifier/`)`/`]` means index, anything else means literal.
bool opensArrayLiteral(const std::string& out) {
    for (auto it = out.rbegin(); it != out.rend(); ++it) {
        const char c = *it;
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            continue;
        }
        return !(isIdentChar(c) || c == ')' || c == ']');
    }
    return true;  // nothing before it at all
}

}  // namespace

bool looksLikeAfterEffects(const std::string& source) {
    static const char* kTells[] = {"var ", "Math.", "//", "&&", "||", "!=", "thisComp",
                                   "thisLayer", "?"};
    for (const char* tell : kTells) {
        if (source.find(tell) != std::string::npos) {
            return true;
        }
    }
    // Strongest tell: Lua has no bare `[` that isn't an index.
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '[' && opensArrayLiteral(source.substr(0, i))) {
            return true;
        }
    }
    return false;
}

Conversion convertFromAfterEffects(const std::string& source) {
    Conversion out;
    std::string result;
    result.reserve(source.size() + 16);

    // Tracks which open brackets became vec(, so the matching close becomes ) not ].
    std::vector<bool> bracketIsVec;

    for (std::size_t i = 0; i < source.size();) {
        const char c = source[i];

        // String literals pass through untouched — e.g. loopOut('pingpong') must survive.
        if (c == '"' || c == '\'') {
            const char quote = c;
            result += c;
            ++i;
            while (i < source.size()) {
                result += source[i];
                if (source[i] == '\\' && i + 1 < source.size()) {
                    result += source[i + 1];
                    i += 2;
                    continue;
                }
                if (source[i] == quote) {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }

        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            result += "--";
            noteOnce(out.notes, "// comments became --");
            i += 2;
            continue;
        }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            const std::size_t end = source.find("*/", i + 2);
            result += "--[[";
            result += source.substr(i + 2, (end == std::string::npos ? source.size() : end) - i - 2);
            result += "]]";
            noteOnce(out.notes, "/* */ comments became --[[ ]]");
            i = (end == std::string::npos) ? source.size() : end + 2;
            continue;
        }

        if (c == '[') {
            if (opensArrayLiteral(result)) {
                result += "vec(";
                bracketIsVec.push_back(true);
                noteOnce(out.notes, "[a, b] became vec(a, b)");
            } else {
                result += '[';
                bracketIsVec.push_back(false);

                // AE indexes from 0, Lua from 1, so a literal index shifts by 1. Only
                // literals: `value[i]` can't be shifted without knowing i.
                std::size_t j = i + 1;
                while (j < source.size() && std::isspace(static_cast<unsigned char>(source[j])) != 0) {
                    ++j;
                }
                std::size_t digits = j;
                while (digits < source.size() &&
                       std::isdigit(static_cast<unsigned char>(source[digits])) != 0) {
                    ++digits;
                }
                std::size_t after = digits;
                while (after < source.size() &&
                       std::isspace(static_cast<unsigned char>(source[after])) != 0) {
                    ++after;
                }
                if (digits > j && after < source.size() && source[after] == ']') {
                    result += std::to_string(std::stoi(source.substr(j, digits - j)) + 1);
                    noteOnce(out.notes, "vector indexes shifted from 0-based to 1-based");
                    i = after;
                    continue;
                }
                if (digits == j && after < source.size() && source[after] != ']') {
                    noteOnce(out.warnings,
                             "a computed index was left alone; AE counts from 0 and Lua "
                             "from 1, so it may need a +1");
                }
            }
            ++i;
            continue;
        }

        if (c == ']') {
            if (!bracketIsVec.empty() && bracketIsVec.back()) {
                result += ')';
            } else {
                result += ']';
            }
            if (!bracketIsVec.empty()) {
                bracketIsVec.pop_back();
            }
            ++i;
            continue;
        }

        // Word-boundary replacements.
        if (isIdentChar(c) && (i == 0 || !isIdentChar(source[i - 1]))) {
            std::size_t end = i;
            while (end < source.size() && isIdentChar(source[end])) {
                ++end;
            }
            const std::string word = source.substr(i, end - i);

            if (word == "var" || word == "let" || word == "const") {
                result += "local";
                noteOnce(out.notes, word + " became local");
                i = end;
                continue;
            }
            if (word == "Math" && end < source.size() && source[end] == '.') {
                result += "math";
                noteOnce(out.notes, "Math. became math.");
                i = end;
                continue;
            }
            if (word == "true" || word == "false" || word == "null") {
                result += (word == "null") ? "nil" : word;
                i = end;
                continue;
            }
            // Things this shim genuinely cannot do anything about.
            if (word == "thisComp" || word == "thisLayer" || word == "thisProperty" ||
                word == "effect" || word == "function") {
                noteOnce(out.warnings,
                         "'" + word + "' is not supported yet and was left as written");
            }
            if (word == "if" || word == "for" || word == "while") {
                noteOnce(out.warnings,
                         "control flow was left as written; JavaScript blocks use braces "
                         "and Lua uses then/do and end");
            }
            result += word;
            i = end;
            continue;
        }

        if (c == '&' && i + 1 < source.size() && source[i + 1] == '&') {
            result += "and";
            noteOnce(out.notes, "&& became and");
            i += 2;
            continue;
        }
        if (c == '|' && i + 1 < source.size() && source[i + 1] == '|') {
            result += "or";
            noteOnce(out.notes, "|| became or");
            i += 2;
            continue;
        }
        if (c == '!' && i + 1 < source.size() && source[i + 1] == '=') {
            result += "~=";
            noteOnce(out.notes, "!= became ~=");
            i += 2;
            continue;
        }
        if (c == '=' && i + 2 < source.size() && source[i + 1] == '=' && source[i + 2] == '=') {
            result += "==";
            noteOnce(out.notes, "=== became ==");
            i += 3;
            continue;
        }
        if (c == '?') {
            // `cond and a or b` is wrong when the middle value is false/nil, and finding
            // branch boundaries needs a real parser — left as written.
            noteOnce(out.warnings,
                     "a ternary (? :) was left as written; Lua has no equivalent and "
                     "converting it safely needs a parser");
        }
        if (c == '{' || c == '}') {
            noteOnce(out.warnings,
                     "braces were left as written; Lua blocks end with 'end'");
        }

        result += c;
        ++i;
    }

    out.lua = result;
    return out;
}

}  // namespace ruby::script
