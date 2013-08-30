/*
 * Copyright (c) 2013, Oracle and/or its affiliates. All rights reserved.
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
#include "classfile/classLoaderExports.hpp"
#include "classfile/javaClasses.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/mutexLocker.hpp"

ClassLoaderExports* ClassLoaderExports::_the_null_class_loader_exports = NULL;
int ClassLoaderExports::_next_loader_tag = 0;

// Returns the unique tag for the given loader, generating it if required
int ClassLoaderExports::tag_for(Handle loader) {
  // null loader
  if (loader.is_null())
     return 0;

  jint tag = java_lang_ClassLoader::loader_tag(loader());
  if (tag != 0)
    return tag;

  {
    MutexLocker ml(LoaderTag_lock);
    tag = ++_next_loader_tag;
  }

  jint* tag_addr = java_lang_ClassLoader::loader_tag_addr(loader());
  jint prev = Atomic::cmpxchg(tag, tag_addr, 0);
  if (prev != 0)
    tag = prev;
  return tag;
}

// returns the ClassLoaderExports for the given loader or null if none
ClassLoaderExports* ClassLoaderExports::exports_for_or_null(Handle loader) {
  if (loader.is_null()) {
    return _the_null_class_loader_exports;
  } else {
    return java_lang_ClassLoader::exports_data(loader());
  }
}

// returns the ClassLoaderExports for the given loader, creating if if needed
ClassLoaderExports* ClassLoaderExports::exports_for(Handle loader) {
  if (loader.is_null()) {
    if (_the_null_class_loader_exports == NULL)
      _the_null_class_loader_exports = new ClassLoaderExports(_exports_table_size);
    return _the_null_class_loader_exports;
  }

  ClassLoaderExports** exports_addr = java_lang_ClassLoader::exports_data_addr(loader());
  ClassLoaderExports* exports = new ClassLoaderExports(_exports_table_size);
  ClassLoaderExports* prev = (ClassLoaderExports*) Atomic::cmpxchg_ptr(exports, exports_addr, NULL);
  if (prev != NULL) {
    delete exports;
    exports = prev;
  }
  return exports;
}

// Set access control so that types defined by loader/pkg are accessible
// only to the given runtime packages. Returns false if access control
// is already set for the loader/package.
bool ClassLoaderExports::set_package_access(Handle loader, const char* pkg,
                                            objArrayHandle loaders, const char** pkgs)
{
  ClassLoaderExports* exports = exports_for(loader);

  unsigned int hash = compute_hash(pkg);
  int index = exports->hash_to_index(hash);

  ClassLoaderExportEntry* first = exports->first_at(index);
  ClassLoaderExportEntry* entry = first;
  while (entry != NULL) {
    if (entry->hash() == hash && strcmp(entry->package(), pkg) == 0) {
        // already set, error for now
        return false;
    }
    entry = entry->next();
  }

  entry = new ClassLoaderExportEntry(hash, pkg);
  for (int i = 0; i < loaders->length(); i++) {
    hash = compute_hash(pkgs[i]);
    entry->add_allow(tag_for(loaders->obj_at(i)), pkgs[i], hash);
  }

  entry->set_next(first);
  exports->set_first(index, entry);

  return true;
}

// Verify that current_class can access new_class.
bool ClassLoaderExports::verify_package_access(Klass* current_class, Klass* new_class) {
  ClassLoaderExports* exports = exports_for_or_null(new_class->class_loader());
  if (exports == NULL)
    return true;

  // ## FIXME encoding the external name is expensive in this prototype
  ResourceMark rm;
  char* name = (char*) new_class->external_name();
  char* last = strrchr(name, '.');
  if (last == NULL) {
    // assume can't set access on the unnamed package for now
    return true;
  }
  *last = '\0';
  const char* pkg = name;

  // package access setup for the package?
  ClassLoaderExportEntry* entry = exports->find_entry(pkg);
  if (entry == NULL) {
    if (TracePackageAccess) {
      tty->print_cr("%s -> %s access allowed (package not restricted)",
        current_class->external_name(), pkg);
    }
    return true;
  }

  name = (char*) current_class->external_name();
  last = strrchr(name, '.');
  if (last == NULL) {
    // assume can't set from unnamed package for now
    if (TracePackageAccess) {
        tty->print_cr("%s -> %s illegal access", current_class->external_name(),
          new_class->external_name());
    }
    return false;
  }
  *last = '\0';
  pkg = name;

  // check access list to see if access from current_class is allowed
  int tag = tag_for(current_class->class_loader());
  unsigned int hash = compute_hash(pkg);
  bool allowed = entry->can_access(tag, pkg, hash);
  if (TracePackageAccess) {
    tty->print("%s -> %s", current_class->external_name(), new_class->external_name());
    if (allowed) {
      tty->print_cr(" access allowed");
    } else {
      tty->print_cr(" illegal access");
    }
    tty->flush();
  }
  return allowed;
}
