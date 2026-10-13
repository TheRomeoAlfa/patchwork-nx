#include "yaml.hpp"

#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace mm::yaml {

namespace {

const Node kNull;

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    Node document() {
        skipBlankLines();
        if (startsWith("---")) {
            pos_ += 3;
            skipBlankLines();
        }
        Node n = blockNode(0);
        skipBlankLines();
        if (startsWith("...")) pos_ += 3;
        skipBlankLines();
        if (pos_ < s_.size()) fail("unexpected content");
        return n;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    [[noreturn]] void fail(const std::string& what) const {
        size_t line = 1;
        for (size_t i = 0; i < pos_ && i < s_.size(); ++i) line += s_[i] == '\n';
        throw std::runtime_error("YAML line " + std::to_string(line) + ": " + what);
    }

    bool eof() const { return pos_ >= s_.size(); }
    char peek(size_t ahead = 0) const { return pos_ + ahead < s_.size() ? s_[pos_ + ahead] : '\0'; }
    bool startsWith(const char* t) const { return s_.compare(pos_, std::char_traits<char>::length(t), t) == 0; }
    size_t column() const {
        size_t start = s_.rfind('\n', pos_ == 0 ? 0 : pos_ - 1);
        return start == std::string::npos || (pos_ == 0) ? pos_ : pos_ - start - 1;
    }
    static bool isSpace(char c) { return c == ' ' || c == '\t'; }
    static bool isBreak(char c) { return c == '\n' || c == '\r'; }

    void skipSpaces() {
        while (isSpace(peek())) ++pos_;
    }
    // Skip spaces and a comment to the end of the line (not the line break).
    void skipToLineEnd() {
        skipSpaces();
        if (peek() == '#')
            while (!eof() && !isBreak(peek())) ++pos_;
    }
    bool atLineEnd() {
        size_t save = pos_;
        skipToLineEnd();
        bool end = eof() || isBreak(peek());
        pos_ = save;
        return end;
    }
    // Move past blank and comment-only lines; leaves pos_ at the first content character.
    void skipBlankLines() {
        for (;;) {
            size_t lineStart = pos_;
            skipToLineEnd();
            if (eof()) return;
            if (isBreak(peek())) {
                if (peek() == '\r') ++pos_;
                if (peek() == '\n') ++pos_;
                continue;
            }
            // Content: back to the line start so column() works, then to the first non-space.
            pos_ = lineStart;
            skipSpaces();
            return;
        }
    }
    void nextLine() {
        while (!eof() && !isBreak(peek())) ++pos_;
        if (peek() == '\r') ++pos_;
        if (peek() == '\n') ++pos_;
    }

    bool atSeqDash() const { return peek() == '-' && (isSpace(peek(1)) || isBreak(peek(1)) || peek(1) == '\0'); }

    std::string readTag() {
        size_t start = pos_;
        ++pos_;  // '!'
        while (!eof() && !isSpace(peek()) && !isBreak(peek()) && peek() != ',' && peek() != '}' && peek() != ']' && peek() != '{' &&
               peek() != '[')
            ++pos_;
        return s_.substr(start, pos_ - start);
    }

    // Is there a "key:" mapping entry starting at pos_ (on this line)?
    bool looksLikeKey() {
        size_t save = pos_;
        bool ok = false;
        try {
            if (peek() == '\'' || peek() == '"') {
                quoted();
                skipSpaces();
                ok = peek() == ':' && (isSpace(peek(1)) || isBreak(peek(1)) || peek(1) == '\0');
            } else if (peek() != '{' && peek() != '[' && peek() != '!' && peek() != '|' && peek() != '>') {
                while (!eof() && !isBreak(peek())) {
                    if (peek() == ':' && (isSpace(peek(1)) || isBreak(peek(1)) || peek(1) == '\0')) {
                        ok = true;
                        break;
                    }
                    if (peek() == ' ' && peek(1) == '#') break;
                    ++pos_;
                }
            }
        } catch (const std::runtime_error&) {
            ok = false;
        }
        pos_ = save;
        return ok;
    }

    std::string key() {
        if (peek() == '\'' || peek() == '"') {
            auto k = quoted();
            skipSpaces();
            if (peek() != ':') fail("expected ':' after key");
            ++pos_;
            return k;
        }
        size_t start = pos_;
        while (!(peek() == ':' && (isSpace(peek(1)) || isBreak(peek(1)) || peek(1) == '\0'))) ++pos_;
        size_t end = pos_;
        while (end > start && isSpace(s_[end - 1])) --end;
        ++pos_;  // ':'
        return s_.substr(start, end - start);
    }

    // A node in block context whose first line starts at or after `minIndent`.
    Node blockNode(size_t minIndent) {
        skipBlankLines();
        if (eof() || column() < minIndent) return Node{};
        std::string tag;
        if (peek() == '!') {
            tag = readTag();
            if (atLineEnd()) {
                Node n = blockNode(minIndent);
                n.tag = tag;
                return n;
            }
            skipSpaces();
        }
        Node n;
        if (atSeqDash())
            n = blockSeq(column());
        else if (looksLikeKey())
            n = blockMap(column());
        else
            n = inlineValue(column() > 0 ? column() - 1 : 0);
        if (!tag.empty()) n.tag = tag;
        return n;
    }

    Node blockMap(size_t indent) {
        Node n;
        n.kind = Node::Kind::Map;
        for (;;) {
            skipBlankLines();
            if (eof() || column() != indent || atSeqDash() || peek() == '.' || startsWith("---")) break;
            if (!looksLikeKey()) fail("expected a mapping key");
            auto k = key();
            n.map.emplace_back(k, valueAfterColon(indent));
        }
        return n;
    }

    // The value after "key:" in a block mapping at `indent`.
    Node valueAfterColon(size_t indent) {
        skipSpaces();
        std::string tag;
        if (peek() == '!') {
            tag = readTag();
            skipSpaces();
        }
        Node v;
        if (atLineEnd()) {
            nextLine();
            size_t save = pos_;
            skipBlankLines();
            // A sequence may sit at the key's own indent.
            if (!eof() && (column() > indent || (column() == indent && atSeqDash()))) {
                size_t col = column();
                pos_ = save;
                v = blockNode(col == indent ? indent : indent + 1);
            } else {
                pos_ = save;
                if (tag == "!obj" || tag == "!list" || tag == "!io") v.kind = Node::Kind::Map;  // empty
            }
        } else {
            v = inlineValue(indent);
        }
        if (!tag.empty()) v.tag = tag;
        return v;
    }

    Node blockSeq(size_t indent) {
        Node n;
        n.kind = Node::Kind::Seq;
        for (;;) {
            skipBlankLines();
            if (eof() || column() != indent || !atSeqDash()) break;
            ++pos_;  // '-'
            skipSpaces();
            if (atLineEnd()) {
                nextLine();
                n.items.push_back(blockNode(indent + 1));
                continue;
            }
            std::string tag;
            if (peek() == '!') {
                tag = readTag();
                skipSpaces();
                if (atLineEnd()) {
                    nextLine();
                    Node item = blockNode(indent + 1);
                    item.tag = tag;
                    n.items.push_back(std::move(item));
                    continue;
                }
            }
            Node item;
            if (atSeqDash())
                item = blockSeq(column());
            else if (looksLikeKey())
                item = blockMap(column());  // "- key: v" then more keys at the same column
            else
                item = inlineValue(indent);
            if (!tag.empty()) item.tag = tag;
            n.items.push_back(std::move(item));
        }
        return n;
    }

    // A value that starts on the current line: flow collection, quoted or plain scalar, or
    // block scalar. `indent` is the enclosing block's indent (continuation lines are deeper).
    Node inlineValue(size_t indent) {
        Node n;
        if (peek() == '!') {
            n.tag = readTag();
            skipSpaces();
            if (atLineEnd()) {
                nextLine();
                Node inner = blockNode(indent + 1);
                inner.tag = n.tag;
                return inner;
            }
        }
        std::string tag = n.tag;
        if (peek() == '{' || peek() == '[') {
            n = flowNode();
        } else if (peek() == '\'' || peek() == '"') {
            n.kind = Node::Kind::Scalar;
            n.value = quoted();
            n.quoted = true;
        } else if (peek() == '|' || peek() == '>') {
            n.kind = Node::Kind::Scalar;
            n.value = blockScalar(indent);
            n.quoted = true;
            return n.tag = tag, n;
        } else {
            n.kind = Node::Kind::Scalar;
            n.value = plainBlockScalar(indent);
        }
        if (!tag.empty()) n.tag = tag;
        skipToLineEnd();
        if (!eof() && !isBreak(peek())) fail("unexpected text after value");
        nextLine();
        return n;
    }

    // A plain scalar in block context, possibly continued on deeper-indented lines.
    std::string plainBlockScalar(size_t indent) {
        std::string out = plainLine();
        for (;;) {
            size_t save = pos_;
            if (!isBreak(peek())) break;
            nextLine();
            size_t blank = 0;
            for (;;) {
                size_t ls = pos_;
                skipSpaces();
                if (isBreak(peek())) {
                    ++blank;
                    nextLine();
                    continue;
                }
                pos_ = ls;
                break;
            }
            size_t ls = pos_;
            skipSpaces();
            if (eof() || column() <= indent || peek() == '#' || atSeqDash() || looksLikeKey()) {
                pos_ = save;
                break;
            }
            (void)ls;
            out += blank ? std::string(blank, '\n') : " ";
            out += plainLine();
        }
        return out;
    }

    std::string plainLine() {
        size_t start = pos_;
        while (!eof() && !isBreak(peek()) && !(isSpace(peek()) && peek(1) == '#')) ++pos_;
        size_t end = pos_;
        while (end > start && isSpace(s_[end - 1])) --end;
        return s_.substr(start, end - start);
    }

    std::string blockScalar(size_t indent) {
        bool literal = peek() == '|';
        ++pos_;
        char chomp = 'c';
        size_t explicitIndent = 0;
        while (!eof() && !isBreak(peek()) && !isSpace(peek()) && peek() != '#') {
            if (peek() == '-' || peek() == '+') chomp = peek();
            else if (peek() >= '1' && peek() <= '9') explicitIndent = size_t(peek() - '0');
            ++pos_;
        }
        skipToLineEnd();
        nextLine();
        std::vector<std::string> lines;
        size_t blockIndent = explicitIndent ? indent + explicitIndent : 0;
        while (!eof()) {
            size_t ls = pos_;
            size_t sp = 0;
            while (peek() == ' ') ++pos_, ++sp;
            if (isBreak(peek()) || eof()) {
                lines.emplace_back();
                nextLine();
                continue;
            }
            if (!blockIndent) blockIndent = sp;
            if (sp < blockIndent || sp <= indent) {
                pos_ = ls;
                break;
            }
            pos_ = ls + blockIndent;
            size_t start = pos_;
            while (!eof() && !isBreak(peek())) ++pos_;
            lines.push_back(s_.substr(start, pos_ - start));
            nextLine();
        }
        size_t trailing = 0;
        while (!lines.empty() && lines.back().empty()) lines.pop_back(), ++trailing;
        std::string out;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) {
                bool fold = !literal && !lines[i].empty() && !lines[i - 1].empty() && lines[i][0] != ' ' && lines[i - 1][0] != ' ';
                out += fold ? " " : "\n";
            }
            out += lines[i];
        }
        if (chomp != '-' && !lines.empty()) out += '\n';
        if (chomp == '+') out += std::string(trailing, '\n');
        // Block scalars end at a line start; step back so the caller's line handling works.
        if (pos_ > 0 && pos_ <= s_.size() && s_[pos_ - 1] == '\n') --pos_;
        return out;
    }

    std::string quoted() {
        char q = peek();
        ++pos_;
        std::string out;
        for (;;) {
            if (eof()) fail("unterminated string");
            char c = peek();
            if (q == '\'' && c == '\'') {
                if (peek(1) == '\'') {
                    out += '\'';
                    pos_ += 2;
                    continue;
                }
                ++pos_;
                return out;
            }
            if (q == '"' && c == '"') {
                ++pos_;
                return out;
            }
            if (q == '"' && c == '\\') {
                char e = peek(1);
                pos_ += 2;
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '0': out += '\0'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    case '/': out += '/'; break;
                    case ' ': out += ' '; break;
                    case 'x': appendUtf8(out, hex(2)); break;
                    case 'u': appendUtf8(out, hex(4)); break;
                    case 'U': appendUtf8(out, hex(8)); break;
                    case '\r':
                    case '\n':
                        // escaped line break: join without a space
                        if (e == '\r' && peek() == '\n') ++pos_;
                        while (isSpace(peek())) ++pos_;
                        break;
                    default: fail("unknown escape");
                }
                continue;
            }
            if (isBreak(c)) {
                // Folding: one break becomes a space, each further empty line a newline.
                while (!out.empty() && isSpace(out.back())) out.pop_back();
                size_t breaks = 0;
                while (isBreak(peek()) || isSpace(peek())) {
                    if (peek() == '\n') ++breaks;
                    ++pos_;
                }
                out += breaks > 1 ? std::string(breaks - 1, '\n') : " ";
                continue;
            }
            out += c;
            ++pos_;
        }
    }

    uint32_t hex(int digits) {
        if (pos_ + size_t(digits) > s_.size()) fail("bad escape");
        auto v = std::strtoul(s_.substr(pos_, size_t(digits)).c_str(), nullptr, 16);
        pos_ += size_t(digits);
        return uint32_t(v);
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    void skipFlowSpace() {
        for (;;) {
            while (isSpace(peek()) || isBreak(peek())) ++pos_;
            if (peek() == '#') {
                while (!eof() && !isBreak(peek())) ++pos_;
                continue;
            }
            return;
        }
    }

    Node flowNode() {
        skipFlowSpace();
        std::string tag;
        if (peek() == '!') {
            tag = readTag();
            skipFlowSpace();
        }
        Node n;
        if (peek() == '{') {
            ++pos_;
            n.kind = Node::Kind::Map;
            for (;;) {
                skipFlowSpace();
                if (peek() == '}') {
                    ++pos_;
                    break;
                }
                std::string k;
                if (peek() == '\'' || peek() == '"') {
                    k = quoted();
                } else {
                    size_t start = pos_;
                    while (!eof() && !(peek() == ':' && (isSpace(peek(1)) || isBreak(peek(1)) || peek(1) == ',' || peek(1) == '}')) &&
                           peek() != ',' && peek() != '}')
                        ++pos_;
                    size_t end = pos_;
                    while (end > start && (isSpace(s_[end - 1]) || isBreak(s_[end - 1]))) --end;
                    k = s_.substr(start, end - start);
                }
                skipFlowSpace();
                Node v;
                if (peek() == ':') {
                    ++pos_;
                    v = flowNode();
                }
                n.map.emplace_back(k, std::move(v));
                skipFlowSpace();
                if (peek() == ',') {
                    ++pos_;
                    continue;
                }
                if (peek() == '}') {
                    ++pos_;
                    break;
                }
                fail("expected ',' or '}'");
            }
        } else if (peek() == '[') {
            ++pos_;
            n.kind = Node::Kind::Seq;
            for (;;) {
                skipFlowSpace();
                if (peek() == ']') {
                    ++pos_;
                    break;
                }
                n.items.push_back(flowNode());
                skipFlowSpace();
                if (peek() == ',') {
                    ++pos_;
                    continue;
                }
                if (peek() == ']') {
                    ++pos_;
                    break;
                }
                fail("expected ',' or ']'");
            }
        } else if (peek() == '\'' || peek() == '"') {
            n.kind = Node::Kind::Scalar;
            n.value = quoted();
            n.quoted = true;
        } else if (peek() == ',' || peek() == '}' || peek() == ']') {
            n.kind = tag == "!obj" || tag == "!list" ? Node::Kind::Map : Node::Kind::Null;
        } else {
            size_t start = pos_;
            while (!eof() && peek() != ',' && peek() != '}' && peek() != ']' && !isBreak(peek()) &&
                   !(peek() == ':' && (isSpace(peek(1)) || peek(1) == ',')))
                ++pos_;
            size_t end = pos_;
            while (end > start && isSpace(s_[end - 1])) --end;
            n.kind = Node::Kind::Scalar;
            n.value = s_.substr(start, end - start);
        }
        if (!tag.empty()) n.tag = tag;
        return n;
    }
};

bool isDigits(const std::string& s, size_t from) {
    if (from >= s.size()) return false;
    for (size_t i = from; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

}  // namespace

const Node* Node::find(const std::string& key) const {
    for (const auto& [k, v] : map)
        if (k == key) return &v;
    return nullptr;
}

const Node& Node::operator[](const std::string& key) const {
    const Node* n = find(key);
    return n ? *n : kNull;
}

bool Node::looksBool() const { return !quoted && (value == "true" || value == "false" || value == "True" || value == "False"); }

bool Node::looksInt() const {
    if (quoted || value.empty()) return false;
    size_t i = (value[0] == '-' || value[0] == '+') ? 1 : 0;
    if (value.compare(i, 2, "0x") == 0) {
        if (value.size() <= i + 2) return false;
        for (size_t j = i + 2; j < value.size(); ++j)
            if (!std::isxdigit(static_cast<unsigned char>(value[j]))) return false;
        return true;
    }
    return isDigits(value, i);
}

bool Node::looksFloat() const {
    if (quoted || value.empty() || looksInt()) return false;
    if (value == ".inf" || value == "-.inf" || value == ".nan" || value == "inf" || value == "-inf" || value == "nan") return true;
    char* end = nullptr;
    std::strtod(value.c_str(), &end);
    return end && *end == '\0';
}

bool Node::asBool() const {
    if (value == "true" || value == "True") return true;
    if (value == "false" || value == "False") return false;
    throw std::runtime_error("not a bool: " + value);
}

int64_t Node::asInt() const {
    if (!looksInt()) throw std::runtime_error("not an integer: " + value);
    bool neg = value[0] == '-';
    size_t i = (value[0] == '-' || value[0] == '+') ? 1 : 0;
    uint64_t v = value.compare(i, 2, "0x") == 0 ? std::strtoull(value.c_str() + i + 2, nullptr, 16) : std::strtoull(value.c_str() + i, nullptr, 10);
    return neg ? -int64_t(v) : int64_t(v);
}

uint64_t Node::asUInt() const { return uint64_t(asInt()); }

double Node::asFloat() const {
    if (value == ".inf" || value == "inf") return INFINITY;
    if (value == "-.inf" || value == "-inf") return -INFINITY;
    if (value == ".nan" || value == "nan") return NAN;
    char* end = nullptr;
    double d = std::strtod(value.c_str(), &end);
    if (!end || *end != '\0' || value.empty()) throw std::runtime_error("not a number: " + value);
    return d;
}

Node parse(const std::string& text) { return Parser(text).document(); }

}  // namespace mm::yaml
