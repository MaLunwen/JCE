/*
 * jce_reflect.cpp  Pure registry for JceReflect types.
 *
 * No ImGui, no editor_state, no undo — strictly the lookup table.
 * The ImGui drawer lives in jce_reflect_draw.cpp; keeping them
 * separate lets the registry be unit-tested without stubbing the
 * editor's UI/state surface.
 */

#include "jce_reflect.h"

#include <cstring>
#include <vector>

namespace {

struct Registry {
    std::vector<const JceReflectType *> types;
};

Registry &reg(void)
{
    static Registry r;
    return r;
}

} /* anonymous namespace */

extern "C" void jce_reflect_register(const JceReflectType *type)
{
    if (!type) return;
    auto &v = reg().types;
    for (const auto *t : v) {
        if (t == type || (t->display_name && type->display_name &&
                          std::strcmp(t->display_name, type->display_name) == 0))
            return;
    }
    v.push_back(type);
}

extern "C" const JceReflectType *jce_reflect_find(const char *display_name)
{
    if (!display_name) return nullptr;
    for (const auto *t : reg().types) {
        if (t->display_name && std::strcmp(t->display_name, display_name) == 0)
            return t;
    }
    return nullptr;
}

extern "C" int jce_reflect_count(void)
{
    return (int)reg().types.size();
}

extern "C" const JceReflectType *jce_reflect_at(int idx)
{
    auto &v = reg().types;
    if (idx < 0 || idx >= (int)v.size()) return nullptr;
    return v[(size_t)idx];
}
