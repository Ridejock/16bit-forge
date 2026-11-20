// SPDX-License-Identifier: MIT
#pragma once
// A small XML reader and writer, enough for splits files: elements, attributes, text, CDATA,
// comments, the declaration and character/entity references. The tree keeps everything it
// doesn't understand, so a file can be read, changed in a few places and written back without
// losing anything another program put there (icons, layouts, comparisons). No namespaces, DTDs
// or mixed content (text between child elements is dropped).
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bitforge::speedrun::xml {

struct Node {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<Node> children;
  std::string text;    // a leaf's text, entities decoded
  bool cdata = false;  // written back as <![CDATA[...]]>

  auto child(std::string_view child_name) -> Node*;
  auto child(std::string_view child_name) const -> const Node*;
  auto child_text(std::string_view child_name) const -> std::string_view;  // "" when absent
  // The first child named so, appended when there is none.
  auto ensure(std::string_view child_name) -> Node&;
  auto add(std::string child_name) -> Node&;
  auto remove(std::string_view child_name) -> void;  // every child named so
  auto attribute(std::string_view key) const -> std::optional<std::string_view>;
  auto set_attribute(std::string_view key, std::string value) -> void;
};

// The document's root element; nullopt (and `error`, if given) when it isn't well-formed.
auto parse(std::string_view text, std::string* error = nullptr) -> std::optional<Node>;
// With an XML declaration, two-space indents and CRLF-free '\n' line ends.
auto write(const Node& root) -> std::string;

}  // namespace bitforge::speedrun::xml
