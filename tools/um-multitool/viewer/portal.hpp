// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// File dialogs through the xdg-desktop-portal (org.freedesktop.portal.FileChooser over D-Bus, libdbus): the
// Flatpak build's dialogs (make linux PORTAL=1 defines UM_PORTAL). Inside the sandbox zenity / kdialog do not
// exist, and the portal is the sanctioned way out: the desktop shows its own dialog, and a file outside what the
// app may read comes back through the document portal (/run/user/<uid>/doc/...), so no host access is needed.
// Blocks until the dialog closes, like the zenity / kdialog pickers.
#pragma once

#ifdef UM_PORTAL
#include <dbus/dbus.h>
#include <unistd.h>

#include <string>

namespace portal {

enum class Kind { Open, Save, Folder };

namespace detail {
inline void AddEntry(DBusMessageIter* dict, const char* key, const char* sig, int type, const void* value) {
    DBusMessageIter e, v;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, sig, &v);
    dbus_message_iter_append_basic(&v, type, value);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}
// A byte-string option ("ay", NUL-terminated as the portal wants paths).
inline void AddBytes(DBusMessageIter* dict, const char* key, const std::string& s) {
    DBusMessageIter e, v, a;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "ay", &v);
    dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "y", &a);
    const char* p = s.c_str();
    dbus_message_iter_append_fixed_array(&a, DBUS_TYPE_BYTE, &p, static_cast<int>(s.size() + 1));
    dbus_message_iter_close_container(&v, &a);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}
// One "*.ext" filter as the portal's a(sa(us)) list.
inline void AddFilter(DBusMessageIter* dict, const std::string& ext) {
    const char* key = "filters";
    const std::string name = ext + " files", glob = "*." + ext;
    const char* n = name.c_str();
    const char* g = glob.c_str();
    const dbus_uint32_t globKind = 0;
    DBusMessageIter e, v, list, filter, rules, rule;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "a(sa(us))", &v);
    dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "(sa(us))", &list);
    dbus_message_iter_open_container(&list, DBUS_TYPE_STRUCT, nullptr, &filter);
    dbus_message_iter_append_basic(&filter, DBUS_TYPE_STRING, &n);
    dbus_message_iter_open_container(&filter, DBUS_TYPE_ARRAY, "(us)", &rules);
    dbus_message_iter_open_container(&rules, DBUS_TYPE_STRUCT, nullptr, &rule);
    dbus_message_iter_append_basic(&rule, DBUS_TYPE_UINT32, &globKind);
    dbus_message_iter_append_basic(&rule, DBUS_TYPE_STRING, &g);
    dbus_message_iter_close_container(&rules, &rule);
    dbus_message_iter_close_container(&filter, &rules);
    dbus_message_iter_close_container(&list, &filter);
    dbus_message_iter_close_container(&v, &list);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}
inline std::string UriToPath(const std::string& uri) {
    if (uri.compare(0, 7, "file://") != 0) return {};
    std::string out;
    for (size_t i = 7; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            out += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += uri[i];
        }
    }
    return out;
}
// The first entry of "uris" in the Response signal's results (a{sv}).
inline std::string FirstUri(DBusMessage* m) {
    DBusMessageIter it, dict;
    if (!dbus_message_iter_init(m, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) return {};
    dbus_uint32_t code = 1;
    dbus_message_iter_get_basic(&it, &code);
    if (code != 0 || !dbus_message_iter_next(&it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return {};
    dbus_message_iter_recurse(&it, &dict);
    for (; dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&dict)) {
        DBusMessageIter e, v, arr;
        dbus_message_iter_recurse(&dict, &e);
        const char* key = nullptr;
        dbus_message_iter_get_basic(&e, &key);
        if (!key || std::string(key) != "uris" || !dbus_message_iter_next(&e)) continue;
        dbus_message_iter_recurse(&e, &v);
        if (dbus_message_iter_get_arg_type(&v) != DBUS_TYPE_ARRAY) return {};
        dbus_message_iter_recurse(&v, &arr);
        if (dbus_message_iter_get_arg_type(&arr) != DBUS_TYPE_STRING) return {};
        const char* uri = nullptr;
        dbus_message_iter_get_basic(&arr, &uri);
        return uri ? uri : "";
    }
    return {};
}
} // namespace detail

// -1: no portal (the caller may try another dialog), 0: cancelled, 1: `out` holds the chosen path.
// `suggested`: for Save, the path offered first (its folder and name); `ext`: Save's file filter ("gif"), or null.
inline int Pick(Kind kind, std::string& out, const std::string& suggested = {}, const char* ext = nullptr) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection* c = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!c) { dbus_error_free(&err); return -1; }
    dbus_connection_set_exit_on_disconnect(c, FALSE);
    auto finish = [&](int r) { dbus_connection_close(c); dbus_connection_unref(c); dbus_error_free(&err); return r; };

    // The request object the portal will answer on: .../request/<sender without ':' and '.'s as '_'>/<token>.
    static int counter = 0;
    const std::string token = "um" + std::to_string(getpid()) + "_" + std::to_string(++counter);
    std::string sender = dbus_bus_get_unique_name(c);
    if (!sender.empty() && sender[0] == ':') sender.erase(0, 1);
    for (char& ch : sender) if (ch == '.') ch = '_';
    std::string handle = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
    dbus_bus_add_match(c, "type='signal',interface='org.freedesktop.portal.Request',member='Response'", &err);
    if (dbus_error_is_set(&err)) return finish(-1);

    DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                                    "org.freedesktop.portal.FileChooser",
                                                    kind == Kind::Save ? "SaveFile" : "OpenFile");
    if (!msg) return finish(-1);
    const char* parent = "";
    const char* title = kind == Kind::Save ? "Save As" : kind == Kind::Folder ? "Select Folder" : "Open File";
    DBusMessageIter args, dict;
    dbus_message_iter_init_append(msg, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &title);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    const char* tok = token.c_str();
    detail::AddEntry(&dict, "handle_token", "s", DBUS_TYPE_STRING, &tok);
    const dbus_bool_t yes = TRUE;
    detail::AddEntry(&dict, "modal", "b", DBUS_TYPE_BOOLEAN, &yes);
    if (kind == Kind::Folder) detail::AddEntry(&dict, "directory", "b", DBUS_TYPE_BOOLEAN, &yes);
    if (kind == Kind::Save && !suggested.empty()) {
        const size_t slash = suggested.find_last_of('/');
        const std::string name = slash == std::string::npos ? suggested : suggested.substr(slash + 1);
        const char* n = name.c_str();
        if (!name.empty()) detail::AddEntry(&dict, "current_name", "s", DBUS_TYPE_STRING, &n);
        if (slash != std::string::npos && slash > 0) detail::AddBytes(&dict, "current_folder", suggested.substr(0, slash));
    }
    if (kind == Kind::Save && ext && *ext) detail::AddFilter(&dict, ext);
    dbus_message_iter_close_container(&args, &dict);

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(c, msg, 10000, &err);
    dbus_message_unref(msg);
    if (!reply) return finish(-1);
    const char* returned = nullptr;
    if (dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &returned, DBUS_TYPE_INVALID) && returned)
        handle = returned; // older portals ignore handle_token
    dbus_message_unref(reply);

    while (dbus_connection_read_write(c, -1)) {
        while (DBusMessage* m = dbus_connection_pop_message(c)) {
            const char* path = dbus_message_get_path(m);
            if (dbus_message_is_signal(m, "org.freedesktop.portal.Request", "Response") && path && handle == path) {
                out = detail::UriToPath(detail::FirstUri(m));
                dbus_message_unref(m);
                return finish(out.empty() ? 0 : 1);
            }
            dbus_message_unref(m);
        }
    }
    return finish(-1); // the bus went away
}

} // namespace portal
#endif
