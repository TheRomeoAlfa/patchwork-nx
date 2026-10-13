#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mm::msbt {

// MSBT message files (Switch: UTF-16LE, version 3). Messages are kept as raw UTF-16 bytes,
// control tags included, so files round-trip exactly; edits replace whole messages.
struct Message {
    std::string text;       // UTF-16LE code units, without the terminating 0x0000
    std::string attribute;  // this message's ATR1 row (attrSize bytes)
    uint32_t style = 0;     // TSY1
};

struct File {
    uint32_t buckets = 101;
    uint32_t attrSize = 0;
    std::string attrExtra;  // ATR1 bytes after the rows, kept as is
    bool hasAttributes = false, hasStyles = false;
    std::vector<std::pair<std::string, Message>> messages;  // label -> message, in TXT2 order
    std::vector<std::pair<std::string, std::string>> otherBlocks;  // magic -> data, kept as is

    Message* find(const std::string& label);
    const Message* find(const std::string& label) const;
    void set(const std::string& label, Message m);  // replace, or add at the end
};

File parse(const std::string& data);  // throws on bad data
std::string write(const File& f);

// UTF-8 <-> UTF-16LE for plain text.
std::string toUtf16(const std::string& utf8);
std::string toUtf8(const std::string& utf16);

}  // namespace mm::msbt
