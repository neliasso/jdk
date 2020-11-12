/*
 * Copyright (c) 1998, 2020, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "jvm.h"
#include "classfile/symbolTable.hpp"
#include "compiler/compilerOracle.hpp"
#include "compiler/methodMatcher.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "oops/klass.hpp"
#include "oops/method.hpp"
#include "oops/symbol.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/os.hpp"

enum class OptionType {
#define enum_of_types(type, internal_type, name) type,
    OPTION_TYPES(enum_of_types)
#undef enum_of_types
    Unknown
};

static const char * optiontype_names[] = {
#define enum_of_types(type, internal_type, name) name,
        OPTION_TYPES(enum_of_types)
#undef enum_of_types
};

static enum OptionType command2types[] = {
#define enum_of_options(option, name, cvariant, ctype) OptionType::ctype,
        COMPILECOMMAND_OPTIONS(enum_of_options)
#undef enum_of_options
};

/* Methods to map real type names to OptionType */
template<typename T>
static OptionType get_type_for() {
  return OptionType::Unknown;
};

template<> OptionType get_type_for<intx>() {
  return OptionType::Intx;
}

template<> OptionType get_type_for<uintx>() {
  return OptionType::Uintx;
}

template<> OptionType get_type_for<bool>() {
  return OptionType::Bool;
}

template<> OptionType get_type_for<ccstr>() {
  return OptionType::Ccstr;
}

template<> OptionType get_type_for<double>() {
  return OptionType::Double;
}

class MethodMatcher;
class TypedMethodOptionMatcher;

// static BasicMatcher* lists[static_cast<int>(CompileCommand::Count)] = { 0, }; // CMH - use a a single linked list instead
static TypedMethodOptionMatcher* option_list = NULL;
static bool any_set = false;

class TypedMethodOptionMatcher : public MethodMatcher {
 private:
  TypedMethodOptionMatcher* _next;
  enum CompileCommand _option;
  OptionType    _type;
 public:

  union {
    bool bool_value;
    intx intx_value;
    uintx uintx_value;
    double double_value;
    ccstr ccstr_value;
  } _u;

  TypedMethodOptionMatcher() : MethodMatcher(),
    _next(NULL),
    _option(CompileCommand::Unknown),
    _type(OptionType::Unknown) {
      memset(&_u, 0, sizeof(_u));
  }

  static TypedMethodOptionMatcher* parse_method_pattern(char*& line, const char*& error_msg);
  TypedMethodOptionMatcher* match(const methodHandle &method, enum CompileCommand option, OptionType type);

  void init(enum CompileCommand cc_option, OptionType type, TypedMethodOptionMatcher* next) {
    _next = next;
    _type = type;
    _option = cc_option;
  }

  void set_next(TypedMethodOptionMatcher* next) {_next = next; }
  TypedMethodOptionMatcher* next() { return _next; }
  OptionType type() { return _type; }
  enum CompileCommand option() { return _option; }
  template<typename T> T value();
  template<typename T> void set_value(T value);
  void print();
  void print_all();
  TypedMethodOptionMatcher* clone();
};

// A few templated accessors instead of a full template class.
template<> intx TypedMethodOptionMatcher::value<intx>() {
  return _u.intx_value;
}

template<> uintx TypedMethodOptionMatcher::value<uintx>() {
  return _u.uintx_value;
}

template<> bool TypedMethodOptionMatcher::value<bool>() {
  return _u.bool_value;
}

template<> double TypedMethodOptionMatcher::value<double>() {
  return _u.double_value;
}

template<> ccstr TypedMethodOptionMatcher::value<ccstr>() {
  return _u.ccstr_value;
}

template<> void TypedMethodOptionMatcher::set_value(intx value) {
  _u.intx_value = value;
}

template<> void TypedMethodOptionMatcher::set_value(uintx value) {
  _u.uintx_value = value;
}

template<> void TypedMethodOptionMatcher::set_value(double value) {
  _u.double_value = value;
}

template<> void TypedMethodOptionMatcher::set_value(bool value) {
  _u.bool_value = value;
}

template<> void TypedMethodOptionMatcher::set_value(ccstr value) {
  _u.ccstr_value = (const ccstr)os::strdup_check_oom(value);
}

void TypedMethodOptionMatcher::print() {
  ttyLocker ttyl;
  print_base(tty);
  const char* command_name = command_names[static_cast<int>(_option)];
  switch (_type) {
  case OptionType::Intx:
    tty->print_cr(" intx %s = " INTX_FORMAT, command_name, value<intx>());
    break;
    case OptionType::Uintx:
    tty->print_cr(" uintx %s = " UINTX_FORMAT, command_name, value<uintx>());
    break;
    case OptionType::Bool:
    tty->print_cr(" bool %s = %s", command_name, value<bool>() ? "true" : "false");
    break;
    case OptionType::Double:
    tty->print_cr(" double %s = %f", command_name, value<double>());
    break;
    case OptionType::Ccstr:
    tty->print_cr(" const char* %s = '%s'", command_name, value<ccstr>());
    break;
  default:
    ShouldNotReachHere();
  }
}

void TypedMethodOptionMatcher::print_all() {
   print();
   if (_next != NULL) {
     tty->print(" ");
     _next->print_all();
   }
 }

TypedMethodOptionMatcher* TypedMethodOptionMatcher::clone() {
  TypedMethodOptionMatcher* m = new TypedMethodOptionMatcher();
  m->_class_mode = _class_mode;
  m->_class_name = _class_name;
  m->_method_mode = _method_mode;
  m->_method_name = _method_name;
  m->_signature = _signature;
  // Need to ref count the symbols
  if (_class_name != NULL) {
    _class_name->increment_refcount();
  }
  if (_method_name != NULL) {
    _method_name->increment_refcount();
  }
  if (_signature != NULL) {
    _signature->increment_refcount();
  }
  return m;
}

TypedMethodOptionMatcher* TypedMethodOptionMatcher::parse_method_pattern(char*& line, const char*& error_msg) {
  assert(error_msg == NULL, "Dont call here with error_msg already set");
  TypedMethodOptionMatcher* tom = new TypedMethodOptionMatcher();
  MethodMatcher::parse_method_pattern(line, error_msg, tom);
  if (error_msg != NULL) {
    delete tom;
    return NULL;
  }
  return tom;
}

TypedMethodOptionMatcher* TypedMethodOptionMatcher::match(const methodHandle& method, enum CompileCommand option, OptionType type) {
  TypedMethodOptionMatcher* current = this;
  while (current != NULL) {
    if (current->_option == option) {
      if (current->matches(method)) {
        return current;
      }
    }
    current = current->next();
  }
  return NULL;
}

template<typename T>
static void add_option_string(TypedMethodOptionMatcher* matcher,  // CHM chnage name
                              enum CompileCommand cc_option,
                              T value) {
  assert(matcher != option_list, "No circular lists please");
  if (cc_option == CompileCommand::Log && !LogCompilation) {
    tty->print_cr("Warning:  +LogCompilation must be enabled in order for individual methods to be logged with ");
    tty->print_cr("          CompileCommand=log,<method pattern>");
  }
  enum OptionType type = command2types[static_cast<int>(cc_option)];
  if (type == OptionType::Ccstrlist) {
    // ccstrlists are stores as ccstr
    type = OptionType::Ccstr;
  }
  assert(type == get_type_for<T>(), "sanity");
  matcher->init(cc_option, type, option_list);
  matcher->set_value<T>(value);
  option_list = matcher;
  if ((cc_option != CompileCommand::DontInline) &&
      (cc_option != CompileCommand::Inline) &&
      (cc_option != CompileCommand::Log)) {
    any_set = true;
  }
  return;
}

template<typename T>
bool CompilerOracle::has_option_value(const methodHandle& method, enum CompileCommand option, T& value, bool verify_type) {
  enum OptionType type = command2types[static_cast<int>(option)];
  if (type == OptionType::Ccstrlist) {
    // CCstrList type options are stored as Ccstr
    type = OptionType::Ccstr;
  }
  if (verify_type) {
    if (type != get_type_for<T>()) {
      // Whitebox API expects false if option and type doesn't match
      return false;
    }
  } else {
    assert(type == get_type_for<T>(), "Value type must match command type");
  }
  if (option_list != NULL) {
    TypedMethodOptionMatcher* m = option_list->match(method, option, type);
    if (m != NULL) {
      value = m->value<T>();
      return true;
    }
  }
  return false;
}

// CMH change command -> option
static bool check_predicate(enum CompileCommand command, const methodHandle& method) {
  bool value = false;
  if (CompilerOracle::has_option_value(method, command, value)) {
    return value;
  }
  return false;
}

static bool has_command(enum CompileCommand command) {
  TypedMethodOptionMatcher* m = option_list;
  while (m != NULL) {
    if (m->option() == command) {
      return true;
    } else {
      m = m->next();
    }
  }
  return false;
}

bool CompilerOracle::has_any_option() {
  return any_set;
}

// Explicit instantiation for all OptionTypes supported.
template bool CompilerOracle::has_option_value<intx>(const methodHandle& method, enum CompileCommand option, intx& value, bool verify_type);
template bool CompilerOracle::has_option_value<uintx>(const methodHandle& method, enum CompileCommand option, uintx& value, bool verify_type);
template bool CompilerOracle::has_option_value<bool>(const methodHandle& method, enum CompileCommand option, bool& value, bool verify_type);
template bool CompilerOracle::has_option_value<ccstr>(const methodHandle& method, enum CompileCommand option, ccstr& value, bool verify_type);
template bool CompilerOracle::has_option_value<double>(const methodHandle& method, enum CompileCommand option, double& value, bool verify_type);

bool CompilerOracle::has_option(const methodHandle& method, enum CompileCommand option) {
  bool value = false;
  has_option_value(method, option, value);
  return value;
}

bool CompilerOracle::should_exclude(const methodHandle& method) {
  if (check_predicate(CompileCommand::Exclude, method)) {
    return true;
  }
  if (has_command(CompileCommand::CompileOnly)) {
    return !check_predicate(CompileCommand::CompileOnly, method);
  }
  return false;
}

bool CompilerOracle::should_inline(const methodHandle& method) {
  return (check_predicate(CompileCommand::Inline, method));
}

bool CompilerOracle::should_not_inline(const methodHandle& method) {
  return check_predicate(CompileCommand::DontInline, method) || check_predicate(CompileCommand::Exclude, method);
}

bool CompilerOracle::should_print(const methodHandle& method) {
  return check_predicate(CompileCommand::Print, method);
}

bool CompilerOracle::should_print_methods() {
  return has_command(CompileCommand::Print);
}

bool CompilerOracle::should_log(const methodHandle& method) {
  if (!LogCompilation) return false;
  if (!has_command(CompileCommand::Log)) {
    return true;  // by default, log all
  }
  return (check_predicate(CompileCommand::Log, method));
}

bool CompilerOracle::should_break_at(const methodHandle& method) {
  return check_predicate(CompileCommand::Break, method);
}

static enum CompileCommand parse_command_name(const char * line, int* bytes_read) {
  assert(ARRAY_SIZE(command_names) == static_cast<int>(CompileCommand::Count), "command_names size mismatch");

  *bytes_read = 0;
  char command[256];
  int matches = sscanf(line, "%255[a-zA-Z0-9]%n", command, bytes_read);
  if (matches > 0) {
    for (uint i = 0; i < ARRAY_SIZE(command_names); i++) {
      if (strcmp(command, command_names[i]) == 0) {
        return static_cast<enum CompileCommand>(i);
      }
    }
  }
  return CompileCommand::Unknown;
}

static void usage() {
  tty->cr();
  tty->print_cr("The CompileCommand option enables the user of the JVM to control specific");
  tty->print_cr("behavior of the dynamic compilers. Many commands require a pattern that defines");
  tty->print_cr("the set of methods the command shall be applied to. The CompileCommand");
  tty->print_cr("option provides the following commands:");
  tty->cr();
  tty->print_cr("  break,<pattern>       - debug breakpoint in compiler and in generated code");
  tty->print_cr("  print,<pattern>       - print assembly");
  tty->print_cr("  exclude,<pattern>     - don't compile or inline");
  tty->print_cr("  inline,<pattern>      - always inline");
  tty->print_cr("  dontinline,<pattern>  - don't inline");
  tty->print_cr("  compileonly,<pattern> - compile only");
  tty->print_cr("  log,<pattern>         - log compilation");
  tty->print_cr("  option,<pattern>,<option type>,<option name>,<value>");
  tty->print_cr("                        - set value of custom option");
  tty->print_cr("  option,<pattern>,<bool option name>");
  tty->print_cr("                        - shorthand for setting boolean flag");
  tty->print_cr("  quiet                 - silence the compile command output");
  tty->print_cr("  help                  - print this text");
  tty->cr();
  tty->print_cr("The preferred format for the method matching pattern is:");
  tty->print_cr("  package/Class.method()");
  tty->cr();
  tty->print_cr("For backward compatibility this form is also allowed:");
  tty->print_cr("  package.Class::method()");
  tty->cr();
  tty->print_cr("The signature can be separated by an optional whitespace or comma:");
  tty->print_cr("  package/Class.method ()");
  tty->cr();
  tty->print_cr("The class and method identifier can be used together with leading or");
  tty->print_cr("trailing *'s for a small amount of wildcarding:");
  tty->print_cr("  *ackage/Clas*.*etho*()");
  tty->cr();
  tty->print_cr("It is possible to use more than one CompileCommand on the command line:");
  tty->print_cr("  -XX:CompileCommand=exclude,java/*.* -XX:CompileCommand=log,java*.*");
  tty->cr();
  tty->print_cr("The CompileCommands can be loaded from a file with the flag");
  tty->print_cr("-XX:CompileCommandFile=<file> or be added to the file '.hotspot_compiler'");
  tty->print_cr("Use the same format in the file as the argument to the CompileCommand flag.");
  tty->print_cr("Add one command on each line.");
  tty->print_cr("  exclude java/*.*");
  tty->print_cr("  option java/*.* ReplayInline");
  tty->cr();
  tty->print_cr("The following commands have conflicting behavior: 'exclude', 'inline', 'dontinline',");
  tty->print_cr("and 'compileonly'. There is no priority of commands. Applying (a subset of) these");
  tty->print_cr("commands to the same method results in undefined behavior.");
  tty->cr();
};

int skip_whitespace(const char* line) {
  // Skip any leading spaces
  int whitespace_read = 0;
  sscanf(line, "%*[ \t]%n", &whitespace_read);
  return whitespace_read;
}

static void scan_value(enum OptionType type, const char* line, int& total_bytes_read,
        TypedMethodOptionMatcher* matcher, enum CompileCommand cc_option, char* errorbuf, const int buf_size) {
  int bytes_read = 0;
  const char* ccname = command_names[static_cast<int>(cc_option)];
  const char* type_str = optiontype_names[static_cast<int>(type)];
  int skipped = skip_whitespace(line); // skip any leading whitespace
  line += skipped;
  total_bytes_read += skipped;
  if (type == OptionType::Intx) {
    intx value;
    if (sscanf(line, "" INTX_FORMAT "%n", &value, &bytes_read) == 1) {
      total_bytes_read += bytes_read;
      line += bytes_read;
      add_option_string(matcher, cc_option, value);
      return;
    } else {
      jio_snprintf(errorbuf, buf_size, "  Value cannot be read for flag %s of type %s ", ccname, type_str);
    }
  } else if (type == OptionType::Uintx) {
    uintx value;
    if (sscanf(line, "" UINTX_FORMAT "%n", &value, &bytes_read) == 1) {
      total_bytes_read += bytes_read;
      line += bytes_read;
      add_option_string(matcher, cc_option, value);
      return;
    } else {
      jio_snprintf(errorbuf, buf_size, "  Value cannot be read for flag %s of type %s", ccname, type_str);
    }
  } else if (type == OptionType::Ccstr) {
    ResourceMark rm;
    char* value = NEW_RESOURCE_ARRAY(char, strlen(line) + 1);
    if (sscanf(line, "%255[_a-zA-Z0-9]%n", value, &bytes_read) == 1) {
      total_bytes_read += bytes_read;
      line += bytes_read;
      add_option_string(matcher, cc_option, (ccstr)value);
      return;
    } else {
      jio_snprintf(errorbuf, buf_size, "  Value cannot be read for flag %s of type %s", ccname, type_str);
    }
  } else if (type == OptionType::Ccstrlist) {
    // Accumulates several strings into one. The internal type is ccstr.
    ResourceMark rm;
    char* value = NEW_RESOURCE_ARRAY(char, strlen(line) + 1);
    char* next_value = value;
    if (sscanf(line, "%255[_a-zA-Z0-9+\\-]%n", next_value, &bytes_read) == 1) {
      total_bytes_read += bytes_read;
      line += bytes_read;
      next_value += bytes_read + 1;
      char* end_value = next_value - 1;
      while (sscanf(line, "%*[ \t]%255[_a-zA-Z0-9+\\-]%n", next_value, &bytes_read) == 1) {
        total_bytes_read += bytes_read;
        line += bytes_read;
        *end_value = ' '; // override '\0'
        next_value += bytes_read;
        end_value = next_value-1;
      }
      add_option_string(matcher, cc_option, (ccstr)value);
      return;
    } else {
      jio_snprintf(errorbuf, buf_size, "  Value cannot be read for flag %s of type %s", ccname, type_str);
    }
  } else if (type == OptionType::Bool) {
    char value[256];
    if (*line == '\0') {
      // Short version -XXCompileCommand=<BoolCommand>,<method pattern>
      // Implies setting value to true
      add_option_string(matcher, cc_option, true);
      return;
    }
    if (sscanf(line, "%255[a-zA-Z]%n", value, &bytes_read) == 1) {
      if (strcmp(value, "true") == 0) {
        total_bytes_read += bytes_read;
        line += bytes_read;
        add_option_string(matcher, cc_option, true);
        return;
      } else if (strcmp(value, "false") == 0) {
        total_bytes_read += bytes_read;
        line += bytes_read;
        add_option_string(matcher, cc_option, false);
        return;
      } else {
        jio_snprintf(errorbuf, buf_size, "  Value cannot be read for flag %s of type %s", ccname, type_str);
      }
    } else {
      jio_snprintf(errorbuf, buf_size, "  Value cannot be read for flag %s of type %s", ccname, type_str);
    }
  } else if (type == OptionType::Double) {
    char buffer[2][256];
    // Decimal separator '.' has been replaced with ' ' or '/' earlier,
    // so read integer and fraction part of double value separately.
    if (sscanf(line, "%255[0-9]%*[ /\t]%255[0-9]%n", buffer[0], buffer[1], &bytes_read) == 2) {
      char value[512] = "";
      jio_snprintf(value, sizeof(value), "%s.%s", buffer[0], buffer[1]);
      total_bytes_read += bytes_read;
      line += bytes_read;
      add_option_string(matcher, cc_option, atof(value));
      return;
    } else {
      jio_snprintf(errorbuf, buf_size, "  Value cannot be read for flag %s of type %s", ccname, type_str);
    }
  } else {
    jio_snprintf(errorbuf, buf_size, "  Type %s not supported ", type_str);
  }
}

// Scan next flag and value in line, return MethodMatcher object on success, NULL on failure.
// On failure, error_msg contains description for the first error.
// For future extensions: set error_msg on first error.
static void scan_flag_and_value(enum OptionType type, const char* line, int& total_bytes_read,
                                TypedMethodOptionMatcher* matcher,
                                char* errorbuf, const int buf_size) {
  total_bytes_read = 0;
  int bytes_read = 0;
  char flag[256];

  // Read flag name.
  if (sscanf(line, "%*[ \t]%255[a-zA-Z0-9]%n", flag, &bytes_read) == 1) {
    line += bytes_read;
    total_bytes_read += bytes_read;
    int bytes_read2 = 0;
    line++; // skip space or ,
    total_bytes_read++;
    enum CompileCommand cc_option = parse_command_name(flag, &bytes_read2);
    if (cc_option == CompileCommand::Unknown) {
      jio_snprintf(errorbuf, buf_size, "  Flag name unknown: %s", flag);
      return;
    }
    enum OptionType optiontype = command2types[static_cast<int>(cc_option)];
    if (command2types[static_cast<int>(cc_option)] != type) {
      const char* optiontype_name = optiontype_names[static_cast<int>(optiontype)];
      const char* type_name = optiontype_names[static_cast<int>(type)];
      jio_snprintf(errorbuf, buf_size, "  Flag %s with type %s doesn't match supplied type %s", flag, optiontype_name, type_name);
      return;
    }
    scan_value(type, line, total_bytes_read, matcher, cc_option, errorbuf, buf_size);

  } else {
    const char* type_str = optiontype_names[static_cast<int>(type)];
    jio_snprintf(errorbuf, buf_size, "  Flag name for type %s should be alphanumeric ", type_str);
  }
  return;
}

void CompilerOracle::print_parse_error(const char*&  error_msg, char* original_line) {
  assert(error_msg != NULL, "Must have error_message");

  ttyLocker ttyl;
  tty->print_cr("CompileCommand: An error occurred during parsing");
  tty->print_cr("Line: %s", original_line); // CMH make a strdup of original line
  tty->print_cr("Error: %s", error_msg);
  CompilerOracle::print_tip();
}

enum OptionType parse_option_type(const char* option_type) {
  if (strcmp(option_type, "intx") == 0) {
    return OptionType::Intx;
  } else if (strcmp(option_type, "uintx") == 0) {
    return OptionType::Uintx;
  } else if (strcmp(option_type, "bool") == 0) {
    return OptionType::Bool;
  } else if (strcmp(option_type, "ccstr") == 0) {
    return OptionType::Ccstr;
  } else if (strcmp(option_type, "ccstrlist") == 0) {
    return OptionType::Ccstrlist;
  } else if (strcmp(option_type, "double") == 0) {
    return OptionType::Double;
  } else {
    return OptionType::Unknown;
  }
}

class LineCopy : StackObj {
  const char* _copy;
public:
    LineCopy(char* line) {
      _copy = os::strdup(line, mtInternal);
    }
    ~LineCopy() {
      os::free((void*)_copy);
    }
    char* get() {
      return (char*)_copy;
    }
};

void CompilerOracle::parse_from_line(char* line) {
  if (line[0] == '\0') return;
  if (line[0] == '#')  return;

  LineCopy original(line);
  int bytes_read;
  enum CompileCommand command = parse_command_name(line, &bytes_read);
  line += bytes_read;
  ResourceMark rm;

  if (command == CompileCommand::Unknown) {
    ttyLocker ttyl;
    tty->print_cr("CompileCommand: unrecognized command");
    tty->print_cr("  \"%s\"", original.get());
    CompilerOracle::print_tip();
    return;
  }

  if (command == CompileCommand::Quiet) {
    _quiet = true;
    return;
  }

  if (command == CompileCommand::Help) {
    usage();
    return;
  }

  const char* error_msg = NULL;
  if (command == CompileCommand::Option) {
    // Look for trailing options.
    //
    // Two types of trailing options are
    // supported:
    //
    // (1) CompileCommand=option,Klass::method,flag
    // (2) CompileCommand=option,Klass::method,type,flag,value
    //
    // Type (1) is used to enable a boolean flag for a method.
    //
    // Type (2) is used to support options with a value. Values can have the
    // the following types: intx, uintx, bool, ccstr, ccstrlist, and double.

    char option_type[256]; // stores flag for Type (1) and type of Type (2)
    line++; // skip the ',' CMH make function that checks car to
    TypedMethodOptionMatcher* archetype = TypedMethodOptionMatcher::parse_method_pattern(line, error_msg);
    if (archetype == NULL) {
      assert(error_msg != NULL, "Must have error_message");
      print_parse_error(error_msg, original.get());
      return;
    }

    line += skip_whitespace(line);

    // This is unnecessarily complex. Should retire multi-option lines and skip while loop
    while (sscanf(line, "%255[a-zA-Z0-9]%n", option_type, &bytes_read) == 1) {
      line += bytes_read;

      // typed_matcher is used as a blueprint for each option, deleted at the end
      TypedMethodOptionMatcher* typed_matcher = archetype->clone();
      enum OptionType type = parse_option_type(option_type);
      if (type != OptionType::Unknown) {
        char errorbuf[1024] = {0};
        // Type (2) option: parse flag name and value.
        scan_flag_and_value(type, line, bytes_read, typed_matcher, errorbuf, sizeof(errorbuf));
        if (*errorbuf != '\0') {
          error_msg = errorbuf;
          print_parse_error(error_msg, original.get());
          return;
        }
        line += bytes_read;
      } else {
        // Type (1) option - option_type contains the option name -> bool value = true is implied
        int bytes_read;
        enum CompileCommand cc_option = parse_command_name(option_type, &bytes_read);
        if (cc_option == CompileCommand::Unknown) {
          ttyLocker ttyl;
          tty->print_cr("CompileCommand: unrecognized command");
          tty->print_cr("  \"%s\"", original.get());
          CompilerOracle::print_tip();
          return;
        }
        add_option_string(typed_matcher, cc_option, true);
      }
      if (typed_matcher != NULL && !_quiet) {
        // Print out the last match added
        assert(error_msg == NULL, "No error here");
        ttyLocker ttyl;
        tty->print("CompileCommand: %s ", command_names[static_cast<int>(command)]);
        typed_matcher->print();
      }
      line += skip_whitespace(line);
    } // while(
    delete archetype;
  } else {  // not an OptionCommand)
    assert(error_msg == NULL, "Don't call here with error_msg already set");
    enum CompileCommandVariant variant = command2variant[static_cast<int>(command)];
    if (variant == CompileCommandVariant::Basic) {
      //BasicMatcher* matcher = BasicMatcher::parse_method_pattern(line, error_msg, false);
      // CMH Check that type is bool
      TypedMethodOptionMatcher* matcher = TypedMethodOptionMatcher::parse_method_pattern(line, error_msg);
      // CMH error if line not empty
      if (error_msg != NULL) {
        assert(matcher == NULL, "consistency");
        print_parse_error(error_msg, original.get());
        return;
      }
      add_option_string(matcher, command, true);
      //add_predicate(command, matcher);
      if (!_quiet) {
        ttyLocker ttyl;
        tty->print("CompileCommand: %s ", command_names[static_cast<int>(command)]);
        matcher->print();
        tty->cr();
      }
    } else if (variant == CompileCommandVariant::Standard) {
      // CompileCommand=<Option>,<method pattern><value>
      enum OptionType type = command2types[static_cast<int>(command)];
      int bytes_read = 0;
      char errorbuf[1024] = {0};
      line++; // skip leading ',' CMH - assert that it is , or emtpy
      TypedMethodOptionMatcher* matcher = TypedMethodOptionMatcher::parse_method_pattern(line, error_msg);
      scan_value(type, line, bytes_read, matcher, command, errorbuf, sizeof(errorbuf));
      if (*errorbuf != '\0') {
        error_msg = errorbuf;
        print_parse_error(error_msg, original.get());
        return;
      }
      if (!_quiet) {
        ttyLocker ttyl;
        tty->print("CompileCommand: %s ", command_names[static_cast<int>(command)]);
        matcher->print();
        tty->cr();
      }
    } else {
      assert(0, "sanity");
    }
  }
}

void print_Basic(const char* name, const char* type) {
  tty->print_cr("    %s,<method pattern>", name);
}

void print_Trivial(const char* name, const char* type) {
  tty->print_cr("    %s", name);
}

void print_Standard(const char* name, const char* type) {
  tty->print_cr("    %s,<method pattern>,<value>  (of type %s)", name, type);
}

void print_Legacy(const char* name, const char* type) {
  // dont use this variant
}

void CompilerOracle::print_tip() {
  tty->cr();
  tty->print_cr("Usage: '-XX:CompileCommand=command,\"package/Class.method()\"'");
  tty->print_cr("Use:   '-XX:CompileCommand=help' for more information.");
  tty->cr();
}

void CompilerOracle::print_commands() {
  tty->cr();
  tty->print_cr("All available commands:");
  tty->print_cr("-XX:CompileCommand=");
#define enum_of_options(option, name, cvariant, ctype) print_##cvariant(name, #ctype);
  COMPILECOMMAND_OPTIONS(enum_of_options)
#undef enum_of_options
  tty->cr();
}

static const char* default_cc_file = ".hotspot_compiler";

static const char* cc_file() {
#ifdef ASSERT
  if (CompileCommandFile == NULL)
    return default_cc_file;
#endif
  return CompileCommandFile;
}

bool CompilerOracle::has_command_file() {
  return cc_file() != NULL;
}

bool CompilerOracle::_quiet = false;

void CompilerOracle::parse_from_file() {
  assert(has_command_file(), "command file must be specified");
  FILE* stream = fopen(cc_file(), "rt");
  if (stream == NULL) return;

  char token[1024];
  int  pos = 0;
  int  c = getc(stream);
  while(c != EOF && pos < (int)(sizeof(token)-1)) {
    if (c == '\n') {
      token[pos++] = '\0';
      parse_from_line(token);
      pos = 0;
    } else {
      token[pos++] = c;
    }
    c = getc(stream);
  }
  token[pos++] = '\0';
  parse_from_line(token);

  fclose(stream);
}

void CompilerOracle::parse_from_string(const char* str, void (*parse_line)(char*)) {
  char token[1024];
  int  pos = 0;
  const char* sp = str;
  int  c = *sp++;
  while (c != '\0' && pos < (int)(sizeof(token)-1)) {
    if (c == '\n') {
      token[pos++] = '\0';
      parse_line(token);
      pos = 0;
    } else {
      token[pos++] = c;
    }
    c = *sp++;
  }
  token[pos++] = '\0';
  parse_line(token);
}

void compilerOracle_init() {
  CompilerOracle::parse_from_string(CompileCommand, CompilerOracle::parse_from_line);
  CompilerOracle::parse_from_string(CompileOnly, CompilerOracle::parse_compile_only);
  if (CompilerOracle::has_command_file()) {
    CompilerOracle::parse_from_file();
  } else {
    struct stat buf;
    if (os::stat(default_cc_file, &buf) == 0) {
      warning("%s file is present but has been ignored.  "
              "Run with -XX:CompileCommandFile=%s to load the file.",
              default_cc_file, default_cc_file);
    }
  }
  if (has_command(CompileCommand::Print)) {
    if (PrintAssembly) {
      warning("CompileCommand and/or %s file contains 'print' commands, but PrintAssembly is also enabled", default_cc_file);
    } else if (FLAG_IS_DEFAULT(DebugNonSafepoints)) {
      warning("printing of assembly code is enabled; turning on DebugNonSafepoints to gain additional output");
      DebugNonSafepoints = true;
    }
  }
}

void CompilerOracle::parse_compile_only(char * line) {
  int i;
  char name[1024];
  const char* className = NULL;
  const char* methodName = NULL;

  bool have_colon = (strstr(line, "::") != NULL);
  char method_sep = have_colon ? ':' : '.';

  if (Verbose) {
    tty->print_cr("%s", line);
  }

  ResourceMark rm;
  while (*line != '\0') {
    MethodMatcher::Mode c_match = MethodMatcher::Exact;
    MethodMatcher::Mode m_match = MethodMatcher::Exact;

    for (i = 0;
         i < 1024 && *line != '\0' && *line != method_sep && *line != ',' && !isspace(*line);
         line++, i++) {
      name[i] = *line;
      if (name[i] == '.')  name[i] = '/';  // package prefix uses '/'
    }

    if (i > 0) {
      char* newName = NEW_RESOURCE_ARRAY( char, i + 1);
      if (newName == NULL)
        return;
      strncpy(newName, name, i);
      newName[i] = '\0';

      if (className == NULL) {
        className = newName;
      } else {
        methodName = newName;
      }
    }

    if (*line == method_sep) {
      if (className == NULL) {
        className = "";
        c_match = MethodMatcher::Any;
      }
    } else {
      // got foo or foo/bar
      if (className == NULL) {
        ShouldNotReachHere();
      } else {
        // missing class name handled as "Any" class match
        if (className[0] == '\0') {
          c_match = MethodMatcher::Any;
        }
      }
    }

    // each directive is terminated by , or NUL or . followed by NUL
    if (*line == ',' || *line == '\0' || (line[0] == '.' && line[1] == '\0')) {
      if (methodName == NULL) {
        methodName = "";
        if (*line != method_sep) {
          m_match = MethodMatcher::Any;
        }
      }

      EXCEPTION_MARK;
      Symbol* c_name = SymbolTable::new_symbol(className);
      Symbol* m_name = SymbolTable::new_symbol(methodName);
      Symbol* signature = NULL;

      //BasicMatcher* bm = new BasicMatcher();
      TypedMethodOptionMatcher* tom = new TypedMethodOptionMatcher();
      BasicMatcher* bm = reinterpret_cast<BasicMatcher*>(tom); // CMH fix remove cast
      bm->init(c_name, c_match, m_name, m_match, signature);
      add_option_string(tom, CompileCommand::CompileOnly, true);
      if (PrintVMOptions) {
        tty->print("CompileOnly: compileonly ");
        tom->print();
      }

      className = NULL;
      methodName = NULL;
    }

    line = *line == '\0' ? line : line + 1;
  }
}

enum CompileCommand CompilerOracle::string_to_option(const char *name) {
  int bytes_read = 0;
  return parse_command_name(name, &bytes_read);
}
