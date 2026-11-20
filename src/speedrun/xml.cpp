// SPDX-License-Identifier: MIT
#include "speedrun/xml.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>

namespace bitforge::speedrun::xml {

namespace {

constexpr int kMaxDepth = 64;  // deeper than any splits file; stops runaway recursion
constexpr std::string_view kDeclaration = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr std::string_view kIndent = "  ";

struct Entity {
  std::string_view name;
  char c;
};
constexpr std::array<Entity, 5> kEntities = {{{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}}};

auto is_space(char c) -> bool { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
auto is_name_char(char c) -> bool {
  return !is_space(c) && c != '/' && c != '>' && c != '=' && c != '<' && c != '"' && c != '\'' && c != '?' && c != '!';
}

auto append_utf8(std::string& out, uint32_t cp) -> void {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

class Parser {
public:
  explicit Parser(std::string_view text) : s_(text) {}

  auto document() -> std::optional<Node> {
    if (s_.substr(0, kBom.size()) == kBom) pos_ = kBom.size();
    if (!skip_misc()) return std::nullopt;
    if (!peek('<')) {
      fail("no root element");
      return std::nullopt;
    }
    Node root;
    if (!element(root, 0)) return std::nullopt;
    if (!skip_misc()) return std::nullopt;
    if (pos_ != s_.size()) {
      fail("text after the root element");
      return std::nullopt;
    }
    return root;
  }

  auto error() const -> const std::string& { return error_; }

private:
  auto fail(std::string what) -> bool {
    if (error_.empty()) error_ = what + " (at byte " + std::to_string(pos_) + ")";
    return false;
  }
  auto peek(std::string_view what) const -> bool { return s_.substr(pos_, what.size()) == what; }
  auto peek(char c) const -> bool { return pos_ < s_.size() && s_[pos_] == c; }
  auto skip_space() -> void {
    while (pos_ < s_.size() && is_space(s_[pos_])) pos_++;
  }
  // Past `end`, from here; false if it never comes.
  auto skip_past(std::string_view end) -> bool {
    const size_t at = s_.find(end, pos_);
    if (at == std::string_view::npos) return false;
    pos_ = at + end.size();
    return true;
  }
  // Whitespace, comments, processing instructions and a DOCTYPE outside the root.
  auto skip_misc() -> bool {
    while (true) {
      skip_space();
      if (peek("<!--")) {
        if (!skip_past("-->")) return fail("unclosed comment");
      } else if (peek("<?")) {
        if (!skip_past("?>")) return fail("unclosed declaration");
      } else if (peek("<!DOCTYPE")) {
        if (!skip_past(">")) return fail("unclosed DOCTYPE");
      } else {
        return true;
      }
    }
  }
  auto name() -> std::string {
    const size_t start = pos_;
    while (pos_ < s_.size() && is_name_char(s_[pos_])) pos_++;
    return std::string(s_.substr(start, pos_ - start));
  }
  // Decodes `raw` (text or an attribute value) into `out`.
  auto decode(std::string_view raw, std::string& out) -> bool {
    for (size_t i = 0; i < raw.size(); i++) {
      if (raw[i] != '&') {
        out += raw[i];
        continue;
      }
      const size_t semi = raw.find(';', i);
      if (semi == std::string_view::npos) return fail("unterminated entity");
      const std::string_view ref = raw.substr(i + 1, semi - i - 1);
      if (!ref.empty() && ref.front() == '#') {
        const bool hex = ref.size() > 1 && (ref[1] == 'x' || ref[1] == 'X');
        const std::string_view digits = ref.substr(hex ? 2 : 1);
        uint32_t cp = 0;
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), cp, hex ? 16 : 10);
        if (digits.empty() || ec != std::errc{} || end != digits.data() + digits.size()) return fail("bad character reference");
        append_utf8(out, cp);
      } else {
        const auto it = std::find_if(kEntities.begin(), kEntities.end(), [ref](const Entity& e) { return e.name == ref; });
        if (it == kEntities.end()) return fail("unknown entity &" + std::string(ref) + ";");
        out += it->c;
      }
      i = semi;
    }
    return true;
  }
  auto element(Node& node, int depth) -> bool {
    if (depth > kMaxDepth) return fail("nested too deeply");
    pos_++;  // '<'
    node.name = name();
    if (node.name.empty()) return fail("an element without a name");
    while (true) {
      skip_space();
      if (peek("/>")) {
        pos_ += 2;
        return true;
      }
      if (peek('>')) {
        pos_++;
        break;
      }
      std::string key = name();
      if (key.empty()) return fail("bad attribute in <" + node.name + ">");
      skip_space();
      if (!peek('=')) return fail("attribute without a value");
      pos_++;
      skip_space();
      if (!peek('"') && !peek('\'')) return fail("unquoted attribute");
      const char quote = s_[pos_++];
      const size_t end = s_.find(quote, pos_);
      if (end == std::string_view::npos) return fail("unclosed attribute");
      std::string value;
      if (!decode(s_.substr(pos_, end - pos_), value)) return false;
      pos_ = end + 1;
      node.attributes.emplace_back(std::move(key), std::move(value));
    }
    // Content: child elements, or text and CDATA.
    std::string text;
    while (true) {
      if (pos_ >= s_.size()) return fail("<" + node.name + "> is never closed");
      if (peek("</")) {
        pos_ += 2;
        if (name() != node.name) return fail("mismatched end tag for <" + node.name + ">");
        skip_space();
        if (!peek('>')) return fail("bad end tag");
        pos_++;
        break;
      }
      if (peek("<!--")) {
        if (!skip_past("-->")) return fail("unclosed comment");
      } else if (peek("<![CDATA[")) {
        pos_ += 9;
        const size_t end = s_.find("]]>", pos_);
        if (end == std::string_view::npos) return fail("unclosed CDATA");
        text.append(s_.substr(pos_, end - pos_));
        node.cdata = true;
        pos_ = end + 3;
      } else if (peek("<?")) {
        if (!skip_past("?>")) return fail("unclosed processing instruction");
      } else if (peek('<')) {
        Node child;
        if (!element(child, depth + 1)) return false;
        node.children.push_back(std::move(child));
      } else {
        const size_t end = std::min(s_.find('<', pos_), s_.size());
        if (!decode(s_.substr(pos_, end - pos_), text)) return false;
        pos_ = end;
      }
    }
    if (node.children.empty()) node.text = std::move(text);
    return true;
  }

  std::string_view s_;
  size_t pos_ = 0;
  std::string error_;
};

auto escape(std::string_view text, bool attribute, std::string& out) -> void {
  for (const char c : text) {
    switch (c) {
    case '&': out += "&amp;"; break;
    case '<': out += "&lt;"; break;
    case '>': out += "&gt;"; break;
    case '"':
      if (attribute) {
        out += "&quot;";
        break;
      }
      [[fallthrough]];
    default: out += c;
    }
  }
}

auto write_node(const Node& n, int depth, std::string& out) -> void {
  for (int i = 0; i < depth; i++) out += kIndent;
  out += '<';
  out += n.name;
  for (const auto& [key, value] : n.attributes) {
    out += ' ';
    out += key;
    out += "=\"";
    escape(value, true, out);
    out += '"';
  }
  if (n.children.empty() && n.text.empty() && !n.cdata) {
    out += " />\n";
    return;
  }
  out += '>';
  if (n.children.empty()) {
    if (n.cdata) {
      out += "<![CDATA[";
      out += n.text;
      out += "]]>";
    } else {
      escape(n.text, false, out);
    }
  } else {
    out += '\n';
    for (const Node& c : n.children) write_node(c, depth + 1, out);
    for (int i = 0; i < depth; i++) out += kIndent;
  }
  out += "</";
  out += n.name;
  out += ">\n";
}

}  // namespace

auto Node::child(std::string_view child_name) -> Node* {
  const auto it = std::find_if(children.begin(), children.end(), [child_name](const Node& c) { return c.name == child_name; });
  return it == children.end() ? nullptr : &*it;
}

auto Node::child(std::string_view child_name) const -> const Node* { return const_cast<Node*>(this)->child(child_name); }

auto Node::child_text(std::string_view child_name) const -> std::string_view {
  const Node* c = child(child_name);
  return c ? std::string_view(c->text) : std::string_view();
}

auto Node::ensure(std::string_view child_name) -> Node& {
  if (Node* c = child(child_name)) return *c;
  return add(std::string(child_name));
}

auto Node::add(std::string child_name) -> Node& {
  text.clear();
  cdata = false;
  children.push_back({});
  children.back().name = std::move(child_name);
  return children.back();
}

auto Node::remove(std::string_view child_name) -> void {
  std::erase_if(children, [child_name](const Node& c) { return c.name == child_name; });
}

auto Node::attribute(std::string_view key) const -> std::optional<std::string_view> {
  for (const auto& [k, v] : attributes) {
    if (k == key) return std::string_view(v);
  }
  return std::nullopt;
}

auto Node::set_attribute(std::string_view key, std::string value) -> void {
  for (auto& [k, v] : attributes) {
    if (k == key) {
      v = std::move(value);
      return;
    }
  }
  attributes.emplace_back(std::string(key), std::move(value));
}

auto parse(std::string_view text, std::string* error) -> std::optional<Node> {
  Parser p(text);
  auto root = p.document();
  if (!root && error) *error = p.error();
  return root;
}

auto write(const Node& root) -> std::string {
  std::string out(kDeclaration);
  write_node(root, 0, out);
  return out;
}

}  // namespace bitforge::speedrun::xml
