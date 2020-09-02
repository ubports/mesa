#encoding=utf-8

# Copyright (C) 2016 Intel Corporation
# Copyright (C) 2016 Broadcom
# Copyright (C) 2020 Collabora, Ltd.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

import xml.parsers.expat
import sys
import operator
from functools import reduce

global_prefix = "mali"

pack_header = """
/* Generated code, see midgard.xml and gen_pack_header.py
 *
 * Packets, enums and structures for Panfrost.
 *
 * This file has been generated, do not hand edit.
 */

#ifndef PAN_PACK_H
#define PAN_PACK_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>
#include <inttypes.h>
#include "util/u_math.h"

#define __gen_unpack_float(x, y, z) uif(__gen_unpack_uint(x, y, z))

static inline uint64_t
__gen_uint(uint64_t v, uint32_t start, uint32_t end)
{
#ifndef NDEBUG
   const int width = end - start + 1;
   if (width < 64) {
      const uint64_t max = (1ull << width) - 1;
      assert(v <= max);
   }
#endif

   return v << start;
}

static inline uint32_t
__gen_sint(int32_t v, uint32_t start, uint32_t end)
{
#ifndef NDEBUG
   const int width = end - start + 1;
   if (width < 64) {
      const int64_t max = (1ll << (width - 1)) - 1;
      const int64_t min = -(1ll << (width - 1));
      assert(min <= v && v <= max);
   }
#endif

   return ((uint32_t) v) << start;
}

static inline uint32_t
__gen_padded(uint32_t v, uint32_t start, uint32_t end)
{
    unsigned shift = __builtin_ctz(v);
    unsigned odd = v >> (shift + 1);

#ifndef NDEBUG
    assert((v >> shift) & 1);
    assert(shift <= 31);
    assert(odd <= 7);
    assert((end - start + 1) == 8);
#endif

    return __gen_uint(shift | (odd << 5), start, end);
}


static inline uint64_t
__gen_unpack_uint(const uint8_t *restrict cl, uint32_t start, uint32_t end)
{
   uint64_t val = 0;
   const int width = end - start + 1;
   const uint32_t mask = (width == 32 ? ~0 : (1 << width) - 1 );

   for (int byte = start / 8; byte <= end / 8; byte++) {
      val |= cl[byte] << ((byte - start / 8) * 8);
   }

   return (val >> (start % 8)) & mask;
}

static inline uint64_t
__gen_unpack_sint(const uint8_t *restrict cl, uint32_t start, uint32_t end)
{
   int size = end - start + 1;
   int64_t val = __gen_unpack_uint(cl, start, end);

   /* Get the sign bit extended. */
   return (val << (64 - size)) >> (64 - size);
}

static inline uint64_t
__gen_unpack_padded(const uint8_t *restrict cl, uint32_t start, uint32_t end)
{
   unsigned val = __gen_unpack_uint(cl, start, end);
   unsigned shift = val & 0b11111;
   unsigned odd = val >> 5;

   return (2*odd + 1) << shift;
}

#define pan_pack(dst, T, name)                          \
   for (struct MALI_ ## T name = { MALI_ ## T ## _header }, \
        *_loop_terminate = (void *) (dst); \
        __builtin_expect(_loop_terminate != NULL, 1); \
        ({ MALI_ ## T ## _pack((uint32_t *) (dst), &name); \
           _loop_terminate = NULL; }))

"""

def to_alphanum(name):
    substitutions = {
        ' ': '_',
        '/': '_',
        '[': '',
        ']': '',
        '(': '',
        ')': '',
        '-': '_',
        ':': '',
        '.': '',
        ',': '',
        '=': '',
        '>': '',
        '#': '',
        '&': '',
        '*': '',
        '"': '',
        '+': '',
        '\'': '',
    }

    for i, j in substitutions.items():
        name = name.replace(i, j)

    return name

def safe_name(name):
    name = to_alphanum(name)
    if not name[0].isalpha():
        name = '_' + name

    return name

def prefixed_upper_name(prefix, name):
    if prefix:
        name = prefix + "_" + name
    return safe_name(name).upper()

def enum_name(name):
    return "{}_{}".format(global_prefix, safe_name(name)).lower()

def num_from_str(num_str):
    if num_str.lower().startswith('0x'):
        return int(num_str, base=16)
    else:
        assert(not num_str.startswith('0') and 'octals numbers not allowed')
        return int(num_str)

MODIFIERS = ["shr", "minus"]

def parse_modifier(modifier):
    if modifier is None:
        return None

    for mod in MODIFIERS:
        if modifier[0:len(mod)] == mod and modifier[len(mod)] == '(' and modifier[-1] == ')':
            return [mod, int(modifier[(len(mod) + 1):-1])]

    print("Invalid modifier")
    assert(False)

class Field(object):
    def __init__(self, parser, attrs):
        self.parser = parser
        if "name" in attrs:
            self.name = safe_name(attrs["name"]).lower()
            self.human_name = attrs["name"]

        if ":" in str(attrs["start"]):
            (word, bit) = attrs["start"].split(":")
            self.start = (int(word) * 32) + int(bit)
        else:
            self.start = int(attrs["start"])

        self.end = self.start + int(attrs["size"]) - 1
        self.type = attrs["type"]

        if self.type == 'bool' and self.start != self.end:
            print("#error Field {} has bool type but more than one bit of size".format(self.name));

        if "prefix" in attrs:
            self.prefix = safe_name(attrs["prefix"]).upper()
        else:
            self.prefix = None

        if "exact" in attrs:
            self.exact = int(attrs["exact"])
        else:
            self.exact = None

        self.default = attrs.get("default")

        # Map enum values
        if self.type in self.parser.enums and self.default is not None:
            self.default = safe_name('{}_{}_{}'.format(global_prefix, self.type, self.default)).upper()

        self.modifier  = parse_modifier(attrs.get("modifier"))

    def emit_template_struct(self, dim, opaque_structs):
        if self.type == 'address':
            type = 'uint64_t'
        elif self.type == 'bool':
            type = 'bool'
        elif self.type == 'float':
            type = 'float'
        elif self.type == 'uint' and self.end - self.start > 32:
            type = 'uint64_t'
        elif self.type == 'int':
            type = 'int32_t'
        elif self.type in ['uint', 'padded']:
            type = 'uint32_t'
        elif self.type in self.parser.structs:
            type = 'struct ' + self.parser.gen_prefix(safe_name(self.type.upper()))

            if opaque_structs:
                type = type.lower() + '_packed'
        elif self.type in self.parser.enums:
            type = 'enum ' + enum_name(self.type)
        else:
            print("#error unhandled type: %s" % self.type)
            type = "uint32_t"

        print("   %-36s %s%s;" % (type, self.name, dim))

        for value in self.values:
            name = prefixed_upper_name(self.prefix, value.name)
            print("#define %-40s %d" % (name, value.value))

    def overlaps(self, field):
        return self != field and max(self.start, field.start) <= min(self.end, field.end)


class Group(object):
    def __init__(self, parser, parent, start, count):
        self.parser = parser
        self.parent = parent
        self.start = start
        self.count = count
        self.size = 0
        self.length = 0
        self.fields = []

    def emit_template_struct(self, dim, opaque_structs):
        if self.count == 0:
            print("   /* variable length fields follow */")
        else:
            if self.count > 1:
                dim = "%s[%d]" % (dim, self.count)

            for field in self.fields:
                if field.exact is not None:
                    continue

                field.emit_template_struct(dim, opaque_structs)

    class Word:
        def __init__(self):
            self.size = 32
            self.fields = []

    def collect_words(self, words):
        for field in self.fields:
            first_word = field.start // 32
            last_word = field.end // 32

            for b in range(first_word, last_word + 1):
                if not b in words:
                    words[b] = self.Word()

                words[b].fields.append(field)

    def emit_pack_function(self, opaque_structs):
        # Determine number of bytes in this group.
        calculated = max(field.end // 8 for field in self.fields) + 1

        if self.length > 0:
            assert(self.length >= calculated)
        else:
            self.length = calculated

        words = {}
        self.collect_words(words)

        emitted_structs = set()

        # Validate the modifier is lossless
        for field in self.fields:
            if field.modifier is None:
                continue

            assert(field.exact is None)

            if field.modifier[0] == "shr":
                shift = field.modifier[1]
                mask = hex((1 << shift) - 1)
                print("   assert((values->{} & {}) == 0);".format(field.name, mask))
            elif field.modifier[0] == "minus":
                print("   assert(values->{} >= {});".format(field.name, field.modifier[1]))

        for index in range(self.length // 4):
            # Handle MBZ words
            if not index in words:
                print("   cl[%2d] = 0;" % index)
                continue

            word = words[index]

            word_start = index * 32

            v = None
            prefix = "   cl[%2d] =" % index

            first = word.fields[0]
            if first.type in self.parser.structs and first.start not in emitted_structs:
                pack_name = self.parser.gen_prefix(safe_name(first.type.upper()))
                start = first.start
                assert((first.start % 32) == 0)
                assert(first.end == first.start + (self.parser.structs[first.type].length * 8) - 1)
                emitted_structs.add(first.start)

                if opaque_structs:
                    print("   memcpy(cl + {}, &values->{}, {});".format(first.start // 32, first.name, (first.end - first.start + 1) // 8))
                else:
                    print("   {}_pack(cl + {}, &values->{});".format(pack_name, first.start // 32, first.name))

            for field in word.fields:
                name = field.name
                start = field.start
                end = field.end
                field_word_start = (field.start // 32) * 32
                start -= field_word_start
                end -= field_word_start

                value = str(field.exact) if field.exact is not None else "values->%s" % name
                if field.modifier is not None:
                    if field.modifier[0] == "shr":
                        value = "{} >> {}".format(value, field.modifier[1])
                    elif field.modifier[0] == "minus":
                        value = "{} - {}".format(value, field.modifier[1])

                if field.type == "uint" or field.type == "address":
                    s = "__gen_uint(%s, %d, %d)" % \
                        (value, start, end)
                elif field.type == "padded":
                    s = "__gen_padded(%s, %d, %d)" % \
                        (value, start, end)
                elif field.type in self.parser.enums:
                    s = "__gen_uint(%s, %d, %d)" % \
                        (value, start, end)
                elif field.type == "int":
                    s = "__gen_sint(%s, %d, %d)" % \
                        (value, start, end)
                elif field.type == "bool":
                    s = "__gen_uint(%s, %d, %d)" % \
                        (value, start, end)
                elif field.type == "float":
                    assert(start == 0 and end == 31)
                    s = "__gen_uint(fui({}), 0, 32)".format(value)
                elif field.type in self.parser.structs:
                    # Structs are packed directly
                    assert(len(word.fields) == 1)
                    continue
                else:
                    s = "#error unhandled field {}, type {}".format(name, field.type)

                if not s == None:
                    shift = word_start - field_word_start
                    if shift:
                        s = "%s >> %d" % (s, shift)

                    if field == word.fields[-1]:
                        print("%s %s;" % (prefix, s))
                    else:
                        print("%s %s |" % (prefix, s))
                    prefix = "           "

            continue

    # Given a field (start, end) contained in word `index`, generate the 32-bit
    # mask of present bits relative to the word
    def mask_for_word(self, index, start, end):
        field_word_start = index * 32
        start -= field_word_start
        end -= field_word_start
        # Cap multiword at one word
        start = max(start, 0)
        end = min(end, 32 - 1)
        count = (end - start + 1)
        return (((1 << count) - 1) << start)

    def emit_unpack_function(self):
        # First, verify there is no garbage in unused bits
        words = {}
        self.collect_words(words)

        for index in range(self.length // 4):
            base = index * 32
            word = words.get(index, self.Word())
            masks = [self.mask_for_word(index, f.start, f.end) for f in word.fields]
            mask = reduce(lambda x,y: x | y, masks, 0)

            ALL_ONES = 0xffffffff

            if mask != ALL_ONES:
                TMPL = '   if (((const uint32_t *) cl)[{}] & {}) fprintf(stderr, "XXX: Invalid field unpacked at word {}\\n");'
                print(TMPL.format(index, hex(mask ^ ALL_ONES), index))

        for field in self.fields:
            # Recurse for structs, see pack() for validation
            if field.type in self.parser.structs:
                pack_name = self.parser.gen_prefix(safe_name(field.type)).upper()
                print("   {}_unpack(cl + {}, &values->{});".format(pack_name, field.start // 8, field.name))
                continue

            convert = None

            args = []
            args.append('cl')
            args.append(str(field.start))
            args.append(str(field.end))

            if field.type in set(["uint", "address"]) | self.parser.enums:
                convert = "__gen_unpack_uint"
            elif field.type == "int":
                convert = "__gen_unpack_sint"
            elif field.type == "padded":
                convert = "__gen_unpack_padded"
            elif field.type == "bool":
                convert = "__gen_unpack_uint"
            elif field.type == "float":
                convert = "__gen_unpack_float"
            else:
                s = "/* unhandled field %s, type %s */\n" % (field.name, field.type)

            suffix = ""
            if field.modifier:
                if field.modifier[0] == "minus":
                    suffix = " + {}".format(field.modifier[1])
                elif field.modifier[0] == "shr":
                    suffix = " << {}".format(field.modifier[1])

            decoded = '{}({}){}'.format(convert, ', '.join(args), suffix)

            print('   values->{} = {};'.format(field.name, decoded))

    def emit_print_function(self):
        for field in self.fields:
            convert = None
            name, val = field.human_name, 'values->{}'.format(field.name)

            if field.type in self.parser.structs:
                pack_name = self.parser.gen_prefix(safe_name(field.type)).upper()
                print('   fprintf(fp, "%*s{}:\\n", indent, "");'.format(field.human_name))
                print("   {}_print(fp, &values->{}, indent + 2);".format(pack_name, field.name))
            elif field.type == "address":
                # TODO resolve to name
                print('   fprintf(fp, "%*s{}: 0x%" PRIx64 "\\n", indent, "", {});'.format(name, val))
            elif field.type in self.parser.enums:
                print('   fprintf(fp, "%*s{}: %s\\n", indent, "", {}_as_str({}));'.format(name, enum_name(field.type), val))
            elif field.type == "int":
                print('   fprintf(fp, "%*s{}: %d\\n", indent, "", {});'.format(name, val))
            elif field.type == "bool":
                print('   fprintf(fp, "%*s{}: %s\\n", indent, "", {} ? "true" : "false");'.format(name, val))
            elif field.type == "float":
                print('   fprintf(fp, "%*s{}: %f\\n", indent, "", {});'.format(name, val))
            elif field.type == "uint" and (field.end - field.start) >= 32:
                print('   fprintf(fp, "%*s{}: 0x%" PRIx64 "\\n", indent, "", {});'.format(name, val))
            else:
                print('   fprintf(fp, "%*s{}: %u\\n", indent, "", {});'.format(name, val))

class Value(object):
    def __init__(self, attrs):
        self.name = attrs["name"]
        self.value = int(attrs["value"])

class Parser(object):
    def __init__(self):
        self.parser = xml.parsers.expat.ParserCreate()
        self.parser.StartElementHandler = self.start_element
        self.parser.EndElementHandler = self.end_element

        self.struct = None
        self.structs = {}
        # Set of enum names we've seen.
        self.enums = set()

    def gen_prefix(self, name):
        return '{}_{}'.format(global_prefix.upper(), name)

    def start_element(self, name, attrs):
        if name == "panxml":
            print(pack_header)
        elif name == "struct":
            name = attrs["name"]
            self.with_opaque = attrs.get("with_opaque", False)

            object_name = self.gen_prefix(safe_name(name.upper()))
            self.struct = object_name

            self.group = Group(self, None, 0, 1)
            if "size" in attrs:
                self.group.length = int(attrs["size"]) * 4
            self.structs[attrs["name"]] = self.group
        elif name == "field":
            self.group.fields.append(Field(self, attrs))
            self.values = []
        elif name == "enum":
            self.values = []
            self.enum = safe_name(attrs["name"])
            self.enums.add(attrs["name"])
            if "prefix" in attrs:
                self.prefix = attrs["prefix"]
            else:
                self.prefix= None
        elif name == "value":
            self.values.append(Value(attrs))

    def end_element(self, name):
        if name == "struct":
            self.emit_struct()
            self.struct = None
            self.group = None
        elif name  == "field":
            self.group.fields[-1].values = self.values
        elif name  == "enum":
            self.emit_enum()
            self.enum = None
        elif name == "panxml":
            # Include at the end so it can depend on us but not the converse
            print('#include "panfrost-job.h"')
            print('#endif')

    def emit_header(self, name):
        default_fields = []
        for field in self.group.fields:
            if not type(field) is Field:
                continue
            if field.default is not None:
                default_fields.append("   .{} = {}".format(field.name, field.default))
            elif field.type in self.structs:
                default_fields.append("   .{} = {{ {}_header }}".format(field.name, self.gen_prefix(safe_name(field.type.upper()))))

        print('#define %-40s\\' % (name + '_header'))
        print(",  \\\n".join(default_fields))
        print('')

    def emit_template_struct(self, name, group, opaque_structs):
        print("struct %s {" % (name + ('_OPAQUE' if opaque_structs else '')))
        group.emit_template_struct("", opaque_structs)
        print("};\n")

        if opaque_structs:
            # Just so it isn't left undefined
            print('#define %-40s 0' % (name + '_OPAQUE_header'))

    def emit_pack_function(self, name, group, with_opaque):
        print("static inline void\n%s_pack(uint32_t * restrict cl,\n%sconst struct %s * restrict values)\n{" %
              (name, ' ' * (len(name) + 6), name))

        group.emit_pack_function(False)

        print("}\n\n")

        if with_opaque:
            print("static inline void\n%s_OPAQUE_pack(uint32_t * restrict cl,\n%sconst struct %s_OPAQUE * restrict values)\n{" %
                  (name, ' ' * (len(name) + 6), name))

            group.emit_pack_function(True)

            print("}\n")

        # Should be a whole number of words
        assert((self.group.length % 4) == 0)

        print('#define {} {}'.format (name + "_LENGTH", self.group.length))
        print('struct {}_packed {{ uint32_t opaque[{}]; }};'.format(name.lower(), self.group.length // 4))

    def emit_unpack_function(self, name, group):
        print("static inline void")
        print("%s_unpack(const uint8_t * restrict cl,\n%sstruct %s * restrict values)\n{" %
              (name.upper(), ' ' * (len(name) + 8), name))

        group.emit_unpack_function()

        print("}\n")

    def emit_print_function(self, name, group):
        print("static inline void")
        print("{}_print(FILE *fp, const struct {} * values, unsigned indent)\n{{".format(name.upper(), name))

        group.emit_print_function()

        print("}\n")

    def emit_struct(self):
        name = self.struct

        self.emit_template_struct(self.struct, self.group, False)
        if self.with_opaque:
            self.emit_template_struct(self.struct, self.group, True)
        self.emit_header(name)
        self.emit_pack_function(self.struct, self.group, self.with_opaque)
        self.emit_unpack_function(self.struct, self.group)
        self.emit_print_function(self.struct, self.group)

    def enum_prefix(self, name):
        return 

    def emit_enum(self):
        e_name = enum_name(self.enum)
        prefix = e_name if self.enum != 'Format' else global_prefix
        print('enum {} {{'.format(e_name))

        for value in self.values:
            name = '{}_{}'.format(prefix, value.name)
            name = safe_name(name).upper()
            print('        % -36s = %6d,' % (name, value.value))
        print('};\n')

        print("static inline const char *")
        print("{}_as_str(enum {} imm)\n{{".format(e_name.lower(), e_name))
        print("    switch (imm) {")
        for value in self.values:
            name = '{}_{}'.format(prefix, value.name)
            name = safe_name(name).upper()
            print('    case {}: return "{}";'.format(name, value.name))
        print('    default: return "XXX: INVALID";')
        print("    }")
        print("}\n")

    def parse(self, filename):
        file = open(filename, "rb")
        self.parser.ParseFile(file)
        file.close()

if len(sys.argv) < 2:
    print("No input xml file specified")
    sys.exit(1)

input_file = sys.argv[1]

p = Parser()
p.parse(input_file)
