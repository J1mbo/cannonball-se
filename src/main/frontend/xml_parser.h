#pragma once

/*=================================================================================================
 * TinyXML2‑based replacement for
 *   boost/property_tree/xml_parser.hpp
 *
 * Part of CannonBall-SE, https://github.com/J1mbo/cannonball-se
 *
 * This file Copyright (c) 2025 James Pearce.
 *
 *
 * Dependencies:
 *   - tinyxml2.h   (system‑wide or placed in your include path)
 *   - tinyxml2 library linked with `-ltinyxml2`
 *
 * Usage:
 *   #include "xml_parser.h"
 *   using xml_parser::ptree;
 *   using xml_parser::read_xml;
 *   using xml_parser::write_xml;
 *
 *   xml_parser::ptree cfg;
 *
 *   Read a file:
 *   ------------
 *         xml_parser::read_xml([filename], [ptree], [mode]);
 *   e.g.: xml_parser::read_xml("config.xml", cfg, xml_parser::parse_mode_t::tolerant);
 *   or:   xml_parser::read_xml("config.xml", cfg);
 *
 *   Mode is either:
 *   - xml_parser::parse_mode_t::strict   - file must have a single root, otherwise returns false
 *   - xml_parser::parse_mode_t::tolerant - (default) if there is no root then it will be processed as fragments.
 *
 *   Write a file:
 *   -------------
 *         xml_parser::write_xml([filename], [ptree]);
 *   e.g.: xml_parser::write_xml("config.xml", cfg);
 *
 *   Any header and comments present in the original file will be preserved.
 *
 *   Read Element:
 *   -------------
 *      int [value] = cfg.get<int>("[key.path]", [default_value]);
 *      e.g: int mode = cfg.get<int>("video.mode", 2);
 *      or:  int mode = cfg.get<int>("video.mode");
 *
 *   Read Attribute:
 *   ---------------
 *      int [value] = cfg.get_attribute<int>("[key.path].<xmlattr>.[attribute]", [default_value]);
 *      e.g. int config.controls.analog.enabled =
 *              cfg.get_attribute<int>("controls.analog.<xmlattr>.enabled",0);
 *
 *   Update Element:
 *   ---------------
 *           cfg.put<int>("[key.path]", [value]);
 *      e.g: cfg.put<int>("video.mode", 2);
 *
 *   Update Attribute:
 *   -----------------
 *           cfg.put_attribute<int>("[key.path].<xmlattr>.[attribute]", [value]);
 *      e.g. cfg.put_attribute<int>("controls.analog.<xmlattr>.enabled", 0);
 *
 *   Also supports Boost style get and put:
 *      get_string, get_int, get_uint, get_long, get_long_long, get_double, get_bool
 *      put_string, put_int, put_uint, put_long, put_long_long, put_double, put_bool
 *
 *   Boost‑style namespace compatibility:
 *   ------------------------------------
 *   namespace boost { namespace property_tree { namespace xml_parser = ::xml_parser; } }
 *==============================================================================================*/

#include <tinyxml2.h>
#include <string>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <iostream>
#include <iomanip>
#include <regex>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace xml_parser
{
    /*============================================================
     * 1.  Constants
     *============================================================*/
    constexpr bool trim_whitespace = true;         // API compatibility
    enum parse_mode_t { strict, tolerant };        // tolerant = wrap stray elements
    constexpr parse_mode_t parse_mode = tolerant;

    /*============================================================
     * 2.  Forward declaration of ptree (needed for read_xml)
     *============================================================*/
    struct ptree;   // incomplete – definition follows

    /*============================================================
     * 3.  Forward declaration of read_xml
     *============================================================*/
    bool read_xml(const std::string& filename, ptree& tree, parse_mode_t mode = parse_mode);

    static inline bool split_attr_key(const std::string& key,
                                      std::string& elem_path,
                                      std::string& attr_name)
    {
        auto pos = key.find("<xmlattr>");
        if (pos == std::string::npos) return false;

        elem_path = key.substr(0, pos);
        if (!elem_path.empty() && elem_path.back() == '.') elem_path.pop_back();

        size_t p = pos + 9;                 // length of "<xmlattr>"
        if (p < key.size() && key[p] == '.') ++p;
        attr_name = key.substr(p);
        return !attr_name.empty();
    }

    /*============================================================
     * 4.  ptree – thin wrapper around tinyxml2::XMLDocument
     *============================================================*/
    struct ptree
    {
        tinyxml2::XMLDocument doc;   // owns the XML tree
        tinyxml2::XMLElement* root;  // guaranteed to exist

        /* default ctor – creates an empty <config> document */
        ptree() : ptree("config") {}                      // delegate to the string ctor

        /* ctor that accepts a root‑tag name */
        explicit ptree(const std::string& rootName)
        {
            root = doc.NewElement(rootName.c_str());      // tinyxml2 wants a C‑string
            doc.InsertFirstChild(root);
        }

        /* ctor that loads a file */
        explicit ptree(const std::string& filename, parse_mode_t mode)
        {
            if (!read_xml(filename, *this, mode))
                throw std::runtime_error("Failed to read XML: " + filename);
        }

        /* helper to change the root name after construction */
        void set_root_name(const std::string& newName)
        {
            // 1.  Create the new element
            tinyxml2::XMLElement* newRoot = doc.NewElement(newName.c_str());

            // 2.  Move all children from the old root to the new one
            while (root->FirstChild()) {                        // as long as there is a child
                tinyxml2::XMLNode* child = root->FirstChild();  // grab first child
                root->DeleteChild(child);                       // detach from old root
                newRoot->InsertEndChild(child);                 // attach to new root
            }

            // 3.  Delete the old root from the document
            //     (DeleteChild is the correct tinyxml2 call)
            doc.DeleteChild(root);                              // safe: old root is no longer referenced

            // 4.  Insert the new root at the front of the document
            doc.InsertFirstChild(newRoot);

            // 5.  Update the member pointer
            root = newRoot;
        }

        void clear()
        {
            // Replace the current object with a fresh one.
            doc.Clear();                                        // TinyXML2 clears the document
            root = doc.NewElement("config");
            doc.InsertFirstChild(root);
        }

        /* ---------------- Accessors ---------------- */

        template<typename T>
        T get(const std::string& key, const T& default_value = T{}) const
        {
            // catch attribute lines (key.xmlattr.attr)
            std::string elem_path, attr_name;
            if (split_attr_key(key, elem_path, attr_name))
                return get_attribute<T>(key, default_value);

            tinyxml2::XMLElement* node = find_node(key);
            if (!node) return default_value;

            const char* txt = node->GetText();
            std::string s = txt ? txt : "";

            if constexpr (std::is_same_v<T, int>)
                return std::stoi(s);
            else if constexpr (std::is_same_v<T, unsigned int>)
                return static_cast<unsigned int>(std::stoul(s));
            else if constexpr (std::is_same_v<T, long>)
                return std::stol(s);
            else if constexpr (std::is_same_v<T, float>)
                return std::stof(s);
            else if constexpr (std::is_same_v<T, double>)
                return std::stod(s);
            else if constexpr (std::is_same_v<T, bool>)
                return (s == "1" || s == "true" || s == "TRUE");
            else if constexpr (std::is_same_v<T, std::string>)
                return s;
            else
                throw std::runtime_error("Unsupported type in ptree::get");
        }

        template<typename T>
        void put(const std::string& key, const T& value)
        {
            // catch xmlattr lines
            std::string elem_path, attr_name;
            if (split_attr_key(key, elem_path, attr_name)) {
                put_attribute<T>(key, value);
                return;
            }

            tinyxml2::XMLElement* node = find_or_create_node(key);
            std::ostringstream oss;
            oss << value;
            node->SetText(oss.str().c_str());
        }

        void erase(const std::string& key)
        {
            tinyxml2::XMLElement* node = find_node(key);
            if (!node) return;
            tinyxml2::XMLNode* parent = node->Parent();
            if (parent) parent->DeleteChild(node);
        }

        /* ------------------------------------------------------------------ */
        /*  Typed getters – mirror Boost's API                                */
        /* ------------------------------------------------------------------ */

        // string
        std::string get_string(const std::string& key,
                            const std::string& default_value = "") const
        {
            return get<std::string>(key, default_value);
        }

        // int
        int get_int(const std::string& key,
                    int default_value = 0) const
        {
            return get<int>(key, default_value);
        }

        // unsigned int
        unsigned int get_uint(const std::string& key,
                            unsigned int default_value = 0) const
        {
            return get<unsigned int>(key, default_value);
        }

        // long
        long get_long(const std::string& key,
                    long default_value = 0L) const
        {
            return get<long>(key, default_value);
        }

        // float
        float get_float(const std::string& key,
                    float default_value = 0.0f) const
        {
            return get<float>(key, default_value);
        }

        // double
        double get_double(const std::string& key,
                    double default_value = 0.0) const
        {
            return get<double>(key, default_value);
        }

        // bool
        bool get_bool(const std::string& key,
                    bool default_value = false) const
        {
            return get<bool>(key, default_value);
        }

        /* ------------------------------------------------------------------ */
        /*  Typed setters – mirror Boost's API                                */
        /* ------------------------------------------------------------------ */

        // string
        void put_string(const std::string& key,
                        const std::string& value)
        {
            put<std::string>(key, value);
        }

        // int
        void put_int(const std::string& key,
                    int value)
        {
            put<int>(key, value);
        }

        // unsigned int
        void put_uint(const std::string& key,
                    unsigned int value)
        {
            put<unsigned int>(key, value);
        }

        // long
        void put_long(const std::string& key,
                    long value)
        {
            put<long>(key, value);
        }

        // float
        void put_float(const std::string& key, float value)
        {
            put<float>(key, value);
        }

        // double
        void put_double(const std::string& key,
                    double value)
        {
            put<double>(key, value);
        }

        // bool
        void put_bool(const std::string& key,
                    bool value)
        {
            put<bool>(key, value);
        }

        /* ---------------- Helpers ---------------- */

        tinyxml2::XMLElement* find_node(const std::string& key) const
        {
            tinyxml2::XMLElement* cur = root;
            std::istringstream iss(key);
            std::string part;
            while (std::getline(iss, part, '.')) {
                cur = cur->FirstChildElement(part.c_str());
                if (!cur) return nullptr;
            }
            return cur;
        }

        tinyxml2::XMLElement* find_or_create_node(const std::string& key)
        {
            tinyxml2::XMLElement* cur = root;
            std::istringstream iss(key);
            std::string part;
            while (std::getline(iss, part, '.')) {
                tinyxml2::XMLElement* child = cur->FirstChildElement(part.c_str());
                if (!child) {
                    child = doc.NewElement(part.c_str());
                    cur->InsertEndChild(child);
                }
                cur = child;
            }
            return cur;
        }

        /* ----------  Attribute helpers  ---------- */
        template<typename T>
        T get_attribute(const std::string& key,
                        const T& default_value = T{}) const
        {
            // key format:  a.b.<xmlattr>.attrName
            auto pos = key.find("<xmlattr>");
            if (pos == std::string::npos)          // not an attribute key
                return get<T>(key, default_value); // fallback to normal get

            // Element path is the part before "<xmlattr>"
            std::string elem_path = key.substr(0, pos);
            // attribute name is the part after "<xmlattr>."
            std::string attr_name = key.substr(pos + 10); // 10 = strlen("<xmlattr>")

            tinyxml2::XMLElement* elem = find_node(elem_path);
            if (!elem) return default_value;

            const char* txt = elem->Attribute(attr_name.c_str());
            std::string s = txt ? txt : "";

            if constexpr (std::is_same_v<T, int>)
                return std::stoi(s);
            else if constexpr (std::is_same_v<T, std::string>)
                return s;
            else if constexpr (std::is_same_v<T, bool>)
                return (s == "1" || s == "true" || s == "TRUE");
            else
                throw std::runtime_error("Unsupported type in ptree::get_attribute");
        }

        template<typename T>
        void put_attribute(const std::string& key, const T& value)
        {
            // Same parsing as above
            auto pos = key.find("<xmlattr>");
            if (pos == std::string::npos) {
                put<T>(key, value);          // normal put
                return;
            }

            std::string elem_path = key.substr(0, pos);
            std::string attr_name = key.substr(pos + 10);

            tinyxml2::XMLElement* elem = find_or_create_node(elem_path);
            if (!elem) return;  // unlikely

            std::ostringstream oss;
            oss << value;
            elem->SetAttribute(attr_name.c_str(), oss.str().c_str());
        }
    };

    // --------------------------------------------------------------------
    // 5.  read_xml – strict / tolerant parsing
    // --------------------------------------------------------------------
    inline bool read_xml(const std::string& filename,
                        ptree& tree,
                        parse_mode_t mode)
    {
        // 0) Load
        tinyxml2::XMLError err = tree.doc.LoadFile(filename.c_str());
        if (err != tinyxml2::XML_SUCCESS)
            return false;

        // 1) Count top-level ELEMENTS (ignore decl/comments)
        int elem_count = 0;
        for (auto* e = tree.doc.FirstChildElement(); e; e = e->NextSiblingElement())
            ++elem_count;

        // 2) Exactly one element → normal behavior
        if (elem_count == 1) {
            tree.root = tree.doc.RootElement();
            return true;
        }

        // 3) Zero or multiple elements
        if (mode == parse_mode_t::strict)
            return false;

        // 4) Tolerant: wrap everything under a dummy <config>, cloning as we go
        auto* dummy_root = tree.doc.NewElement("config");

        // Insert the dummy root AFTER the XML declaration if present; otherwise first.
        if (auto* first = tree.doc.FirstChild(); first && first->ToDeclaration())
            tree.doc.InsertAfterChild(first, dummy_root);
        else
            tree.doc.InsertFirstChild(dummy_root);

        // Collect all non-declaration top-level nodes
        std::vector<tinyxml2::XMLNode*> to_move;
        to_move.reserve(8);
        for (auto* n = tree.doc.FirstChild(); n; n = n->NextSibling()) {
            if (n->ToDeclaration()) continue;      // keep declaration at the top
            if (n == dummy_root) continue;         // skip the wrapper we just inserted
            to_move.push_back(n);
        }

        // Clone each node under <config>, then delete the original
        for (auto* n : to_move) {
            tinyxml2::XMLNode* clone = n->DeepClone(&tree.doc); // ← IMPORTANT: clone, don’t reuse
            dummy_root->InsertEndChild(clone);
            tree.doc.DeleteChild(n);                             // safe to free the original now
        }

        tree.root = dummy_root;
        return true;
    }

    /*============================================================
     * 6.  write_xml – preserves header & comments
     *============================================================*/
    inline bool write_xml(const std::string& filename,
                          ptree& tree,
                          const std::string& xml_declaration = R"(<?xml version="1.0" encoding="UTF-8"?>)")
    {
        /* 1. Ensure the declaration is present at the very top */
        if (!tree.doc.FirstChild())
            tree.doc.InsertFirstChild(tree.doc.NewDeclaration(xml_declaration.c_str()));

        /* 2. Pretty-print to an in-memory string (TinyXML2 has no ostream overload) */
        tinyxml2::XMLPrinter printer(nullptr, /*compact=*/false); // false → pretty print
        tree.doc.Print(&printer);
        const std::string xml = printer.CStr();                   // XML text

        /* 3. Post-process: align comments with the following element (one extra level)
            and give the same indent to all lines in multi-line comments. */
        std::string result;                  // <— declare result that we'll write out
        {
            const std::string SPACE_STEP(4, ' ');          // adjust if you prefer 2/8, etc.
            const std::string COMMENT_SPACE_STEP(5, ' ');  // extra indent for multi-line comments

            auto is_blank = [](std::string_view s) {
                for (char c : s) if (c!=' ' && c!='\t' && c!='\r') return false;
                return true;
            };

            auto rtrim = [](std::string& s) {
                while (!s.empty() && (s.back()==' ' || s.back()=='\t' || s.back()=='\r'))
                    s.pop_back();
            };

            auto ltrim = [](std::string& s) {
                // Find leading whitespace
                auto it = std::find_if(s.begin(), s.end(),
                                       [](unsigned char ch){ return !std::isspace(ch); });
                // erase the prefix that contains only whitespace
                s.erase(s.begin(), it);
            };

            auto ends_with_open_tag = [](std::string_view s) {
                // Trim trailing spaces/tabs/CR
                size_t i = s.size();
                while (i && (s[i-1]==' ' || s[i-1]=='\t' || s[i-1]=='\r')) --i;
                if (!i || s[i-1] != '>') return false;

                // Find nearest '<' before '>'
                size_t lt = s.rfind('<', i ? i-1 : 0);
                if (lt == std::string_view::npos) return false;

                // Reject close tags, comments, processing-instruction, and self-closing tags
                if (lt+1 < i && (s[lt+1]=='/' || s[lt+1]=='!' || s[lt+1]=='?')) return false;
                if (i>=2 && s[i-2]=='/') return false; // "... />"

                return true; // treat as an opening tag like <video>
            };

            std::string out;
            out.reserve(xml.size() + 64);

            std::string prev_nonempty;     // previous non-empty, trimmed line (no newline)
            bool in_comment = false;
            std::string current_indent;    // indent we apply to all lines of the current comment
            std::string newline;           // "\n" or "\r\n" or ""

            std::size_t pos = 0;
            while (pos < xml.size()) {
                // Read one physical line (including the terminal newline, if present)
                std::size_t eol = xml.find('\n', pos);
                std::string line = (eol == std::string::npos) ? xml.substr(pos)
                                                            : xml.substr(pos, eol - pos + 1);
                pos = (eol == std::string::npos) ? xml.size() : eol + 1;

                // Separate the newline (preserve CRLF vs LF)
                newline.clear();
                if (!line.empty() && line.back() == '\n') {
                    newline = "\n";
                    line.pop_back();
                    if (!line.empty() && line.back() == '\r') { // CRLF
                        newline = "\r\n";
                        line.pop_back();
                    }
                }

                std::string_view v(line);

                if (!in_comment) {
                    // Leading whitespace
                    std::size_t i = 0;
                    while (i < v.size() && (v[i]==' ' || v[i]=='\t')) ++i;
                    std::string indent(line.begin(), line.begin() + i);
                    std::string_view rest = v.substr(i);


                    if (rest.rfind("<!--", 0) == 0) {
                        // Decide whether we are the first comment after a tag
                        std::string one_level = (indent.find('\t') != std::string::npos)
                                                ? std::string("\t") : SPACE_STEP;
                        if (ends_with_open_tag(prev_nonempty) && indent.empty()) {
                            /* First child of a tag – use a single level of indent. */
                            current_indent = one_level;
                        } else {
                            /* Normal case – keep the indent that was present in the file. */
                            current_indent = indent;
                        }

                        /* Emit the comment line as‑is. */
                        out += current_indent;
                        out.append(rest.begin(), rest.end());
                        out += newline;

                        /* Enter a multi‑line comment only if the closing --> is missing. */
                        in_comment = (rest.find("-->") == std::string::npos);

                    } else {
                        // Normal line; pass through unchanged
                        out.append(line);
                        out += newline;

                        // Track previous non-empty (trimmed) line
                        std::string tmp = line;
                        rtrim(tmp);
                        if (!is_blank(tmp)) prev_nonempty = std::move(tmp);
                    }
                } else {
                    // Inside multi-line comment: normalize leading indentation of continuation lines
                    std::size_t i = 0;
                    while (i < v.size() && (v[i]==' ' || v[i]=='\t')) ++i;
                    std::string_view rest = v.substr(i);

                    out += current_indent;

                    // Leave comment if this line closes it
                    if (rest.find("-->") != std::string::npos) {
                        std::string tmp(rest);
                        ltrim(tmp); rtrim(tmp);
                        if ((tmp!="-->") && (tmp!="--->")) out += COMMENT_SPACE_STEP;
                        in_comment = false;
                    } else {
                        out += COMMENT_SPACE_STEP;
                    }

                    out.append(rest.begin(), rest.end());
                    out += newline;
                }
            }

            result.swap(out);  // <-- fill 'result' for the writer below
        }

        /* 4. Write the adjusted XML to disk */
        std::ofstream ofs(filename, std::ios::binary);
        if (!ofs) return false;
        ofs << result;
        return true;
    }

} // namespace xml_parser

/*============================================================
 * 7.  Compatibility with Boost namespace layout
 *============================================================*/
namespace boost { namespace property_tree { namespace xml_parser = ::xml_parser; } }

