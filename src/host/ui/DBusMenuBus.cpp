// Gridline - the dbusmenu object on the session bus (K-112). See DBusMenuBus.h.
//
// libdbus-1's C interface is reached through dlsym; the few structures the caller allocates
// (DBusError, DBusMessageIter, DBusObjectPathVTable) are declared here with the layout of the
// library's public headers and extra room at the end.
#if !defined(_WIN32)
#include "DBusMenuBus.h"
#include "../../core/ThreadJoin.h"
#include "../../core/ThreadName.h"
#include <dlfcn.h>
#include <cstdint>
#include <cstring>

namespace gl::host::dbusmenu {

namespace {

constexpr const char* kPath = "/com/gridline/menubar";
constexpr const char* kIface = "com.canonical.dbusmenu";
constexpr const char* kRegistrar = "com.canonical.AppMenu.Registrar";
constexpr const char* kRegistrarPath = "/com/canonical/AppMenu/Registrar";

using u32 = uint32_t;
struct Err {
  const char* name;
  const char* message;
  unsigned int bits;
  void* padding1;
  void* room[4];
};
struct Iter {
  void* d1;
  void* d2;
  u32 d3;
  int d4, d5, d6, d7, d8, d9, d10, d11, pad1;
  void* pad2;
  void* pad3;
  void* room[8];
};
using MsgFn = int (*)(void* conn, void* msg, void* user);
using FreeFn = void (*)(void* data);
using UnregFn = void (*)(void* conn, void* user);
struct VTable {
  UnregFn unregister;
  MsgFn message;
  void* pad[4];
};

constexpr int kString = 's', kInt32 = 'i', kUint32 = 'u', kBool = 'b', kVariant = 'v', kArray = 'a', kStruct = 'r', kDictEntry = 'e',
              kObjectPath = 'o';
constexpr int kHandled = 0, kNotHandled = 1;
constexpr int kDataRemains = 0;

struct Api {
  void (*error_init)(Err*);
  void (*error_free)(Err*);
  u32 (*error_is_set)(const Err*);
  void* (*bus_get_private)(int, Err*);
  const char* (*bus_get_unique_name)(void*);
  u32 (*bus_name_has_owner)(void*, const char*, Err*);
  void (*connection_set_exit_on_disconnect)(void*, u32);
  void (*connection_close)(void*);
  void (*connection_unref)(void*);
  u32 (*connection_try_register_object_path)(void*, const char*, const VTable*, void*, Err*);
  u32 (*connection_read_write)(void*, int);
  int (*connection_dispatch)(void*);
  u32 (*connection_send)(void*, void*, u32*);
  void (*connection_flush)(void*);
  u32 (*message_is_method_call)(void*, const char*, const char*);
  const char* (*message_get_member)(void*);
  const char* (*message_get_interface)(void*);
  void* (*message_new_method_return)(void*);
  void* (*message_new_method_call)(const char*, const char*, const char*, const char*);
  void* (*message_new_signal)(const char*, const char*, const char*);
  void* (*message_new_error)(void*, const char*, const char*);
  void (*message_unref)(void*);
  u32 (*message_iter_init)(void*, Iter*);
  void (*message_iter_init_append)(void*, Iter*);
  u32 (*message_iter_append_basic)(Iter*, int, const void*);
  u32 (*message_iter_open_container)(Iter*, int, const char*, Iter*);
  u32 (*message_iter_close_container)(Iter*, Iter*);
  int (*message_iter_get_arg_type)(Iter*);
  void (*message_iter_get_basic)(Iter*, void*);
  u32 (*message_iter_next)(Iter*);
  void (*message_iter_recurse)(Iter*, Iter*);
  void (*bus_add_match)(void*, const char*, Err*);
  u32 (*connection_add_filter)(void*, MsgFn, void*, FreeFn);
  u32 (*message_is_signal)(void*, const char*, const char*);
  int (*bus_request_name)(void*, const char*, unsigned int, Err*);
  int (*bus_release_name)(void*, const char*, Err*);
};
Api D{};

template <class T>
bool sym(void* lib, T& fn, const char* name) {
  fn = reinterpret_cast<T>(dlsym(lib, name));
  return fn != nullptr;
}

bool loadApi(void* lib) {
  bool ok = true;
  ok &= sym(lib, D.error_init, "dbus_error_init");
  ok &= sym(lib, D.error_free, "dbus_error_free");
  ok &= sym(lib, D.error_is_set, "dbus_error_is_set");
  ok &= sym(lib, D.bus_get_private, "dbus_bus_get_private");
  ok &= sym(lib, D.bus_get_unique_name, "dbus_bus_get_unique_name");
  ok &= sym(lib, D.bus_name_has_owner, "dbus_bus_name_has_owner");
  ok &= sym(lib, D.connection_set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect");
  ok &= sym(lib, D.connection_close, "dbus_connection_close");
  ok &= sym(lib, D.connection_unref, "dbus_connection_unref");
  ok &= sym(lib, D.connection_try_register_object_path, "dbus_connection_try_register_object_path");
  ok &= sym(lib, D.connection_read_write, "dbus_connection_read_write");
  ok &= sym(lib, D.connection_dispatch, "dbus_connection_dispatch");
  ok &= sym(lib, D.connection_send, "dbus_connection_send");
  ok &= sym(lib, D.connection_flush, "dbus_connection_flush");
  ok &= sym(lib, D.message_is_method_call, "dbus_message_is_method_call");
  ok &= sym(lib, D.message_get_member, "dbus_message_get_member");
  ok &= sym(lib, D.message_get_interface, "dbus_message_get_interface");
  ok &= sym(lib, D.message_new_method_return, "dbus_message_new_method_return");
  ok &= sym(lib, D.message_new_method_call, "dbus_message_new_method_call");
  ok &= sym(lib, D.message_new_signal, "dbus_message_new_signal");
  ok &= sym(lib, D.message_new_error, "dbus_message_new_error");
  ok &= sym(lib, D.message_unref, "dbus_message_unref");
  ok &= sym(lib, D.message_iter_init, "dbus_message_iter_init");
  ok &= sym(lib, D.message_iter_init_append, "dbus_message_iter_init_append");
  ok &= sym(lib, D.message_iter_append_basic, "dbus_message_iter_append_basic");
  ok &= sym(lib, D.message_iter_open_container, "dbus_message_iter_open_container");
  ok &= sym(lib, D.message_iter_close_container, "dbus_message_iter_close_container");
  ok &= sym(lib, D.message_iter_get_arg_type, "dbus_message_iter_get_arg_type");
  ok &= sym(lib, D.message_iter_get_basic, "dbus_message_iter_get_basic");
  ok &= sym(lib, D.message_iter_next, "dbus_message_iter_next");
  ok &= sym(lib, D.message_iter_recurse, "dbus_message_iter_recurse");
  ok &= sym(lib, D.bus_add_match, "dbus_bus_add_match");
  ok &= sym(lib, D.connection_add_filter, "dbus_connection_add_filter");
  ok &= sym(lib, D.message_is_signal, "dbus_message_is_signal");
  ok &= sym(lib, D.bus_request_name, "dbus_bus_request_name");
  ok &= sym(lib, D.bus_release_name, "dbus_bus_release_name");
  return ok;
}

// ---- writing ----
void putString(Iter* it, const std::string& raw) {
  const std::string s = validUtf8(raw);  // libdbus ends the program on anything else
  const char* c = s.c_str();
  D.message_iter_append_basic(it, kString, &c);
}
void putInt(Iter* it, int32_t v) { D.message_iter_append_basic(it, kInt32, &v); }
void putUint(Iter* it, u32 v) { D.message_iter_append_basic(it, kUint32, &v); }
void putBool(Iter* it, bool b) {
  const u32 v = b ? 1 : 0;
  D.message_iter_append_basic(it, kBool, &v);
}

template <class F>
void dictEntry(Iter* dict, const char* key, const char* sig, F&& value) {
  Iter e{}, v{};
  D.message_iter_open_container(dict, kDictEntry, nullptr, &e);
  putString(&e, key);
  D.message_iter_open_container(&e, kVariant, sig, &v);
  value(&v);
  D.message_iter_close_container(&e, &v);
  D.message_iter_close_container(dict, &e);
}

// One entry's properties (a{sv}).
void putProps(Iter* it, const Entry& e) {
  Iter dict{};
  D.message_iter_open_container(it, kArray, "{sv}", &dict);
  if (e.id != 0) {
    if (e.separator) {
      dictEntry(&dict, "type", "s", [](Iter* v) { putString(v, "separator"); });
    } else {
      dictEntry(&dict, "label", "s", [&](Iter* v) { putString(v, e.label); });
      dictEntry(&dict, "enabled", "b", [&](Iter* v) { putBool(v, e.enabled); });
      if (e.checkable) {
        dictEntry(&dict, "toggle-type", "s", [](Iter* v) { putString(v, "checkmark"); });
        dictEntry(&dict, "toggle-state", "i", [&](Iter* v) { putInt(v, e.checked ? 1 : 0); });
      }
      if (!e.shortcut.empty())
        dictEntry(&dict, "shortcut", "aas", [&](Iter* v) {
          Iter outer{};
          D.message_iter_open_container(v, kArray, "as", &outer);
          for (const auto& combo : e.shortcut) {
            Iter inner{};
            D.message_iter_open_container(&outer, kArray, "s", &inner);
            for (const std::string& k : combo) putString(&inner, k);
            D.message_iter_close_container(&outer, &inner);
          }
          D.message_iter_close_container(v, &outer);
        });
    }
    dictEntry(&dict, "visible", "b", [](Iter* v) { putBool(v, true); });
  }
  if (e.submenu) dictEntry(&dict, "children-display", "s", [](Iter* v) { putString(v, "submenu"); });
  D.message_iter_close_container(it, &dict);
}

// (ia{sv}av), `depth` levels of children (-1: all).
void putLayout(Iter* it, const Model& m, const Entry& e, int depth) {
  Iter st{}, kids{};
  D.message_iter_open_container(it, kStruct, nullptr, &st);
  putInt(&st, e.id);
  putProps(&st, e);
  D.message_iter_open_container(&st, kArray, "v", &kids);
  if (depth != 0)
    for (int c : e.children)
      if (const Entry* ce = m.entry(c)) {
        Iter v{};
        D.message_iter_open_container(&kids, kVariant, "(ia{sv}av)", &v);
        putLayout(&v, m, *ce, depth < 0 ? -1 : depth - 1);
        D.message_iter_close_container(&kids, &v);
      }
  D.message_iter_close_container(&st, &kids);
  D.message_iter_close_container(it, &st);
}

// ---- reading ----
bool getInt(Iter* it, int32_t& v) {
  if (D.message_iter_get_arg_type(it) != kInt32) return false;
  D.message_iter_get_basic(it, &v);
  D.message_iter_next(it);
  return true;
}
bool getString(Iter* it, std::string& s) {
  const int t = D.message_iter_get_arg_type(it);
  if (t != kString && t != kObjectPath) return false;
  const char* c = nullptr;
  D.message_iter_get_basic(it, &c);
  s = c ? c : "";
  D.message_iter_next(it);
  return true;
}
std::vector<int> getInts(Iter* it) {
  std::vector<int> v;
  if (D.message_iter_get_arg_type(it) != kArray) return v;
  Iter sub{};
  D.message_iter_recurse(it, &sub);
  int32_t x = 0;
  while (getInt(&sub, x)) v.push_back(x);
  D.message_iter_next(it);
  return v;
}

const char* kIntrospect =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node><interface name=\"com.canonical.dbusmenu\">"
    "<property name=\"Version\" type=\"u\" access=\"read\"/><property name=\"TextDirection\" type=\"s\" access=\"read\"/>"
    "<property name=\"Status\" type=\"s\" access=\"read\"/><property name=\"IconThemePath\" type=\"as\" access=\"read\"/>"
    "<method name=\"GetLayout\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/><arg type=\"as\" direction=\"in\"/>"
    "<arg type=\"u\" direction=\"out\"/><arg type=\"(ia{sv}av)\" direction=\"out\"/></method>"
    "<method name=\"GetGroupProperties\"><arg type=\"ai\" direction=\"in\"/><arg type=\"as\" direction=\"in\"/>"
    "<arg type=\"a(ia{sv})\" direction=\"out\"/></method>"
    "<method name=\"GetProperty\"><arg type=\"i\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>"
    "<method name=\"Event\"><arg type=\"i\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"in\"/>"
    "<arg type=\"u\" direction=\"in\"/></method>"
    "<method name=\"EventGroup\"><arg type=\"a(isvu)\" direction=\"in\"/><arg type=\"ai\" direction=\"out\"/></method>"
    "<method name=\"AboutToShow\"><arg type=\"i\" direction=\"in\"/><arg type=\"b\" direction=\"out\"/></method>"
    "<method name=\"AboutToShowGroup\"><arg type=\"ai\" direction=\"in\"/><arg type=\"ai\" direction=\"out\"/>"
    "<arg type=\"ai\" direction=\"out\"/></method>"
    "<signal name=\"ItemsPropertiesUpdated\"><arg type=\"a(ia{sv})\"/><arg type=\"a(ias)\"/></signal>"
    "<signal name=\"LayoutUpdated\"><arg type=\"u\"/><arg type=\"i\"/></signal>"
    "<signal name=\"ItemActivationRequested\"><arg type=\"i\"/><arg type=\"u\"/></signal>"
    "</interface><interface name=\"org.freedesktop.DBus.Properties\">"
    "<method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>"
    "<method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
    "</interface><interface name=\"org.freedesktop.DBus.Introspectable\">"
    "<method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method></interface></node>\n";

void putMenuProperty(Iter* v, const std::string& name) {
  if (name == "Version") putUint(v, 3);
  else if (name == "Status") putString(v, "normal");
  else putString(v, "ltr");
}
const char* menuPropertySig(const std::string& name) { return name == "Version" ? "u" : "s"; }

int onMessage(void*, void* msg, void* user) { return static_cast<Bus*>(user)->handle(msg); }
int onFilter(void*, void* msg, void* user) { return static_cast<Bus*>(user)->filter(msg); }
constexpr const char* kHostNames[] = {kRegistrar, "org.kde.kappmenu"};
void onUnregister(void*, void*) {}
const VTable kVTable = {onUnregister, onMessage, {nullptr, nullptr, nullptr, nullptr}};

// KWin on X11 reads the menu's place from two window properties (Xlib from the process, as SDL
// loaded it; nothing when X11 is not in use).
void setKdeProperties(unsigned long xid, const std::string& service, const char* path) {
  if (!xid) return;
  void* x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
  if (!x11) return;
  using OpenFn = void* (*)(const char*);
  using AtomFn = unsigned long (*)(void*, const char*, int);
  using ChangeFn = int (*)(void*, unsigned long, unsigned long, unsigned long, int, int, const unsigned char*, int);
  using FlushFn = int (*)(void*);
  using CloseFn = int (*)(void*);
  OpenFn open = reinterpret_cast<OpenFn>(dlsym(x11, "XOpenDisplay"));
  AtomFn atom = reinterpret_cast<AtomFn>(dlsym(x11, "XInternAtom"));
  ChangeFn change = reinterpret_cast<ChangeFn>(dlsym(x11, "XChangeProperty"));
  FlushFn flush = reinterpret_cast<FlushFn>(dlsym(x11, "XFlush"));
  CloseFn closeD = reinterpret_cast<CloseFn>(dlsym(x11, "XCloseDisplay"));
  if (open && atom && change && flush && closeD) {
    if (void* dpy = open(nullptr)) {
      const unsigned long utf8 = atom(dpy, "UTF8_STRING", 0);
      const unsigned long svc = atom(dpy, "_KDE_NET_WM_APPMENU_SERVICE_NAME", 0);
      const unsigned long obj = atom(dpy, "_KDE_NET_WM_APPMENU_OBJECT_PATH", 0);
      change(dpy, xid, svc, utf8, 8, 0, reinterpret_cast<const unsigned char*>(service.c_str()), static_cast<int>(service.size()));
      change(dpy, xid, obj, utf8, 8, 0, reinterpret_cast<const unsigned char*>(path), static_cast<int>(std::strlen(path)));
      flush(dpy);
      closeD(dpy);
    }
  }
  dlclose(x11);
}

}  // namespace

Bus::~Bus() { close(); }

bool Bus::open(const char* library) {
  if (conn_) return true;
  if (!lib_) {
    const char* names[] = {library, "libdbus-1.so.3", "libdbus-1.so", "libdbus-1.3.dylib"};
    for (const char* n : names)
      if (n && !lib_) lib_ = dlopen(n, RTLD_NOW | RTLD_LOCAL);
    if (!lib_) {
      status_ = "no libdbus-1";
      return false;
    }
    if (!loadApi(lib_)) {
      status_ = "libdbus-1 lacks a function this needs";
      dlclose(lib_);
      lib_ = nullptr;
      return false;
    }
  }
  Err err{};
  D.error_init(&err);
  conn_ = D.bus_get_private(0 /* session */, &err);
  if (!conn_ || D.error_is_set(&err)) {
    status_ = std::string("no session bus") + (err.message ? std::string(" (") + err.message + ")" : std::string());
    D.error_free(&err);
    conn_ = nullptr;
    return false;
  }
  D.connection_set_exit_on_disconnect(conn_, 0);
  if (!D.connection_try_register_object_path(conn_, kPath, &kVTable, this, &err)) {
    status_ = std::string("cannot put the menu on the bus") + (err.message ? std::string(" (") + err.message + ")" : std::string());
    D.error_free(&err);
    close();
    return false;
  }
  D.error_free(&err);
  status_ = "dbusmenu on the session bus, no global menu host";
  return true;
}

void Bus::close() {
  hosts_.clear();
  watching_ = false;
  if (conn_) {
    D.connection_close(conn_);
    D.connection_unref(conn_);
    conn_ = nullptr;
  }
}

std::string Bus::uniqueName() const {
  const char* n = conn_ ? D.bus_get_unique_name(conn_) : nullptr;
  return n ? n : "";
}

bool Bus::hasHost() {
  if (!conn_) return false;
  Err err{};
  D.error_init(&err);
  registrar_.clear();
  for (const char* name : kHostNames) {
    if (D.bus_name_has_owner(conn_, name, &err)) {
      hosts_.nameOwner(name, true);
      if (registrar_.empty()) registrar_ = name;
    }
    D.error_free(&err);
    D.error_init(&err);
  }
  D.error_free(&err);
  status_ = registrar_.empty() ? "dbusmenu on the session bus, no global menu host" : "dbusmenu: " + registrar_;
  return !registrar_.empty();
}

bool Bus::watchHosts() {
  if (!conn_) return false;
  if (watching_) return true;
  if (!D.connection_add_filter(conn_, onFilter, this, nullptr)) return false;
  for (const char* name : kHostNames) {
    const std::string rule = std::string("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='") +
                             name + "'";
    D.bus_add_match(conn_, rule.c_str(), nullptr);  // no error: sent without waiting for the answer
  }
  watching_ = true;
  return true;
}

bool Bus::takeHostAppeared() {
  const bool a = appeared_;
  appeared_ = false;
  return a;
}

void Bus::step(int timeoutMs) {
  if (!conn_) return;
  D.connection_read_write(conn_, timeoutMs);
  for (int n = 0; n < 64 && D.connection_dispatch(conn_) == kDataRemains; ++n) {
  }
}

int Bus::filter(void* msg) {
  if (!D.message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged")) return kNotHandled;
  Iter in{};
  if (!D.message_iter_init(msg, &in)) return kNotHandled;
  std::string name, before, after;
  if (!getString(&in, name) || !getString(&in, before) || !getString(&in, after)) return kNotHandled;
  bool ours = false;
  for (const char* h : kHostNames) ours = ours || name == h;
  if (!ours) return kNotHandled;
  if (after.empty()) {
    hosts_.nameOwner(name, false);
  } else {
    if (!hosts_.has(name)) appeared_ = true;
    hosts_.nameOwner(name, true);
  }
  registrar_ = hosts_.first();
  status_ = registrar_.empty() ? "dbusmenu on the session bus, no global menu host" : "dbusmenu: " + registrar_;
  return kNotHandled;  // others may watch it too
}

bool Bus::requestName(const char* name) {
  if (!conn_) return false;
  Err err{};
  D.error_init(&err);
  const int r = D.bus_request_name(conn_, name, 0, &err);
  const bool ok = !D.error_is_set(&err) && r == 1;  // the primary owner
  D.error_free(&err);
  return ok;
}

bool Bus::releaseName(const char* name) {
  if (!conn_) return false;
  Err err{};
  D.error_init(&err);
  D.bus_release_name(conn_, name, &err);
  const bool ok = !D.error_is_set(&err);
  D.error_free(&err);
  return ok;
}

bool Bus::sendEvent(const std::string& dest, int id, const char* event) {
  if (!conn_) return false;
  void* call = D.message_new_method_call(dest.c_str(), kPath, kIface, "Event");
  if (!call) return false;
  Iter it{}, v{};
  D.message_iter_init_append(call, &it);
  putInt(&it, id);
  putString(&it, event);
  D.message_iter_open_container(&it, kVariant, "s", &v);
  putString(&v, "");
  D.message_iter_close_container(&it, &v);
  putUint(&it, 0);
  D.connection_send(conn_, call, nullptr);
  D.message_unref(call);
  D.connection_flush(conn_);
  return true;
}

bool Bus::sendGetLayout(const std::string& dest) {
  if (!conn_) return false;
  void* call = D.message_new_method_call(dest.c_str(), kPath, kIface, "GetLayout");
  if (!call) return false;
  Iter it{}, names{};
  D.message_iter_init_append(call, &it);
  putInt(&it, 0);
  putInt(&it, -1);
  D.message_iter_open_container(&it, kArray, "s", &names);
  D.message_iter_close_container(&it, &names);
  D.connection_send(conn_, call, nullptr);
  D.message_unref(call);
  D.connection_flush(conn_);
  return true;
}

void Bus::announce(unsigned long xid) {
  if (!conn_) return;
  if (void* call = D.message_new_method_call(kRegistrar, kRegistrarPath, kRegistrar, "RegisterWindow")) {
    Iter it{};
    D.message_iter_init_append(call, &it);
    putUint(&it, static_cast<u32>(xid));
    const char* path = kPath;
    D.message_iter_append_basic(&it, kObjectPath, &path);
    D.connection_send(conn_, call, nullptr);
    D.message_unref(call);
  }
  setKdeProperties(xid, uniqueName(), kPath);
  D.connection_flush(conn_);
}

void Bus::update(const std::vector<nativemenu::Item>& appMenu, const std::vector<nativemenu::Item>& menus) {
  if (!model_.set(appMenu, menus) || !conn_) return;
  if (void* sig = D.message_new_signal(kPath, kIface, "LayoutUpdated")) {
    Iter it{};
    D.message_iter_init_append(sig, &it);
    putUint(&it, model_.revision());
    putInt(&it, 0);
    D.connection_send(conn_, sig, nullptr);
    D.message_unref(sig);
    D.connection_flush(conn_);
  }
}

void Bus::poll() {
  if (!conn_) return;
  D.connection_read_write(conn_, 0);
  for (int n = 0; n < 64 && D.connection_dispatch(conn_) == kDataRemains; ++n) {
  }
}

std::string Bus::takeChosen() {
  std::string c;
  c.swap(chosen_);
  return c;
}

int Bus::handle(void* msg) {
  const char* memberC = D.message_get_member(msg);
  const char* ifaceC = D.message_get_interface(msg);
  const std::string member = memberC ? memberC : "", iface = ifaceC ? ifaceC : "";
  void* reply = nullptr;
  Iter in{}, out{};
  const bool hasArgs = D.message_iter_init(msg, &in) != 0;
  if (iface == "org.freedesktop.DBus.Introspectable" && member == "Introspect") {
    reply = D.message_new_method_return(msg);
    D.message_iter_init_append(reply, &out);
    putString(&out, kIntrospect);
  } else if (iface == "org.freedesktop.DBus.Properties" && (member == "Get" || member == "GetAll")) {
    std::string on, name;
    if (hasArgs) getString(&in, on);
    if (member == "Get" && hasArgs) getString(&in, name);
    reply = D.message_new_method_return(msg);
    D.message_iter_init_append(reply, &out);
    if (member == "Get") {
      Iter v{};
      D.message_iter_open_container(&out, kVariant, menuPropertySig(name), &v);
      putMenuProperty(&v, name);
      D.message_iter_close_container(&out, &v);
    } else {
      Iter dict{};
      D.message_iter_open_container(&out, kArray, "{sv}", &dict);
      for (const char* p : {"Version", "TextDirection", "Status"}) dictEntry(&dict, p, menuPropertySig(p), [&](Iter* v) { putMenuProperty(v, p); });
      D.message_iter_close_container(&out, &dict);
    }
  } else if (iface.empty() || iface == kIface) {
    // A menu bar reading the menus or acting on them: the host shows them (GitHub #85).
    if (member == "GetLayout" || member == "GetGroupProperties" || member == "GetProperty" || member == "Event" || member == "EventGroup" ||
        member == "AboutToShow" || member == "AboutToShowGroup")
      hosts_.menusRead();
    if (member == "GetLayout") {
      int32_t parent = 0, depth = -1;
      if (hasArgs) getInt(&in, parent), getInt(&in, depth);
      const Entry* e = model_.entry(parent);
      if (!e) {
        reply = D.message_new_error(msg, "com.canonical.dbusmenu.Error.UnknownId", "no such menu item");
      } else {
        reply = D.message_new_method_return(msg);
        D.message_iter_init_append(reply, &out);
        putUint(&out, model_.revision());
        putLayout(&out, model_, *e, depth);
      }
    } else if (member == "GetGroupProperties") {
      std::vector<int> ids = hasArgs ? getInts(&in) : std::vector<int>{};
      if (ids.empty())
        for (const Entry& e : model_.entries()) ids.push_back(e.id);
      reply = D.message_new_method_return(msg);
      D.message_iter_init_append(reply, &out);
      Iter arr{};
      D.message_iter_open_container(&out, kArray, "(ia{sv})", &arr);
      for (int id : ids)
        if (const Entry* e = model_.entry(id)) {
          Iter st{};
          D.message_iter_open_container(&arr, kStruct, nullptr, &st);
          putInt(&st, id);
          putProps(&st, *e);
          D.message_iter_close_container(&arr, &st);
        }
      D.message_iter_close_container(&out, &arr);
    } else if (member == "GetProperty") {
      int32_t id = 0;
      std::string name;
      if (hasArgs) getInt(&in, id), getString(&in, name);
      const Entry* e = model_.entry(id);
      reply = D.message_new_method_return(msg);
      D.message_iter_init_append(reply, &out);
      Iter v{};
      const bool isBool = name == "enabled" || name == "visible";
      D.message_iter_open_container(&out, kVariant, isBool ? "b" : "s", &v);
      if (isBool) putBool(&v, name == "visible" || (e && e->enabled));
      else putString(&v, e && name == "label" ? e->label : std::string());
      D.message_iter_close_container(&out, &v);
    } else if (member == "Event") {
      int32_t id = 0;
      std::string event;
      if (hasArgs) getInt(&in, id), getString(&in, event);
      if (event == "clicked") {
        const std::string app = model_.appIdOf(id);
        const Entry* e = model_.entry(id);
        if (!app.empty() && e && e->enabled) chosen_ = app;
      }
      reply = D.message_new_method_return(msg);
    } else if (member == "EventGroup") {
      if (hasArgs && D.message_iter_get_arg_type(&in) == kArray) {
        Iter arr{};
        D.message_iter_recurse(&in, &arr);
        while (D.message_iter_get_arg_type(&arr) == kStruct) {
          Iter st{};
          D.message_iter_recurse(&arr, &st);
          int32_t id = 0;
          std::string event;
          getInt(&st, id);
          getString(&st, event);
          if (event == "clicked") {
            const std::string app = model_.appIdOf(id);
            const Entry* e = model_.entry(id);
            if (!app.empty() && e && e->enabled) chosen_ = app;
          }
          D.message_iter_next(&arr);
        }
      }
      reply = D.message_new_method_return(msg);
      D.message_iter_init_append(reply, &out);
      Iter none{};
      D.message_iter_open_container(&out, kArray, "i", &none);
      D.message_iter_close_container(&out, &none);
    } else if (member == "AboutToShow") {
      reply = D.message_new_method_return(msg);
      D.message_iter_init_append(reply, &out);
      putBool(&out, false);  // the layout is always current (LayoutUpdated tells the changes)
    } else if (member == "AboutToShowGroup") {
      reply = D.message_new_method_return(msg);
      D.message_iter_init_append(reply, &out);
      for (int k = 0; k < 2; ++k) {
        Iter none{};
        D.message_iter_open_container(&out, kArray, "i", &none);
        D.message_iter_close_container(&out, &none);
      }
    }
  }
  if (!reply) return kNotHandled;
  D.connection_send(conn_, reply, nullptr);
  D.message_unref(reply);
  return kHandled;
}

// ---------------------------------------------------------------- Worker ----

Worker::~Worker() { stop(); }

void Worker::start(const char* library) {
  if (thread_.joinable()) return;
  stop_ = false;
  {
    std::lock_guard<std::mutex> l(m_);
    status_ = "starting";
  }
  thread_ = std::thread([this, lib = std::string(library ? library : "")]() { gl::threads::setName("gl-dbus-menu"); run(lib); });
}

void Worker::stop() {
  stop_ = true;
  gl::threads::safeJoin(thread_);
}

std::string Worker::status() const {
  std::lock_guard<std::mutex> l(m_);
  return status_;
}

std::string Worker::uniqueName() const {
  std::lock_guard<std::mutex> l(m_);
  return unique_;
}

void Worker::post(const std::vector<nativemenu::Item>& appMenu, const std::vector<nativemenu::Item>& menus, bool announce, unsigned long xid) {
  std::lock_guard<std::mutex> l(m_);
  app_ = appMenu;
  menus_ = menus;
  pending_ = true;
  announce_ = announce_ || announce;
  xid_ = xid;
}

std::string Worker::takeChosen() {
  std::lock_guard<std::mutex> l(m_);
  std::string c;
  c.swap(chosen_);
  return c;
}

void Worker::run(std::string library) {
  Bus bus;
  if (!bus.open(library.empty() ? nullptr : library.c_str())) {
    std::lock_guard<std::mutex> l(m_);
    status_ = bus.status();
    return;
  }
  bus.watchHosts();
  bus.hasHost();  // the first look; from here on the bus tells
  {
    std::lock_guard<std::mutex> l(m_);
    unique_ = bus.uniqueName();
    status_ = bus.status();
  }
  host_ = bus.hostSeen();
  shows_ = bus.hostShows();
  up_ = true;
  bool announced = false;
  unsigned long xid = 0;
  std::vector<nativemenu::Item> app, menus;
  while (!stop_) {
    bus.step(50);
    bool update = false, announce = false;
    {
      std::lock_guard<std::mutex> l(m_);
      if (pending_) {
        app.swap(app_);
        menus.swap(menus_);
        update = true;
        pending_ = false;
      }
      announce = announce_;
      announce_ = false;
      xid = xid_;
    }
    if (update) bus.update(app, menus);
    if (announce) bus.announce(xid), announced = true;
    if (bus.takeHostAppeared() && announced) bus.announce(xid);  // a host that (re)started needs the window again
    host_ = bus.hostSeen();
    shows_ = bus.hostShows();
    std::string chosen = bus.takeChosen();
    std::lock_guard<std::mutex> l(m_);
    if (!chosen.empty()) chosen_ = chosen;
    status_ = bus.status();
  }
  up_ = false;
  host_ = false;
  shows_ = false;
  bus.close();
}

}  // namespace gl::host::dbusmenu
#endif
